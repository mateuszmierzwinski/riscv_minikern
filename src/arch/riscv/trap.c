#include "arch/riscv/trap.h"

#include "arch/riscv/frame.h"
#include "arch/riscv/plic.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "kernel/irq.h"

/* The frame the entry code writes and the structure the C reads are two
 * descriptions of one thing, so they are checked against each other here
 * rather than trusted to stay in step. */
_Static_assert(sizeof(struct trapframe) == FRAME_SIZE, "trapframe and frame.h disagree");
_Static_assert(__builtin_offsetof(struct trapframe, sepc) == FRAME_SEPC, "sepc offset");
_Static_assert(__builtin_offsetof(struct trapframe, sstatus) == FRAME_SSTATUS, "sstatus offset");
_Static_assert(__builtin_offsetof(struct trapframe, scause) == FRAME_SCAUSE, "scause offset");
_Static_assert(__builtin_offsetof(struct trapframe, stval) == FRAME_STVAL, "stval offset");
_Static_assert(__builtin_offsetof(struct trapframe, kernel_tp) == FRAME_KTP, "kernel_tp offset");
_Static_assert(FRAME_SIZE % 16u == 0u, "a stack pointer must stay 16-byte aligned");

void trap_entry(void);

/* The ABI's names, so a dump reads like the code that faulted rather than
 * like a table of x-numbers. */
static const char *const reg_names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0",
    "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5",
    "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

static const char *exception_name(uint64_t code) {
    switch (code) {
        case EXC_INSN_MISALIGNED: return "instruction address misaligned";
        case EXC_INSN_FAULT: return "instruction access fault";
        case EXC_ILLEGAL_INSN: return "illegal instruction";
        case EXC_BREAKPOINT: return "breakpoint";
        case EXC_LOAD_MISALIGNED: return "load address misaligned";
        case EXC_LOAD_FAULT: return "load access fault";
        case EXC_STORE_MISALIGNED: return "store address misaligned";
        case EXC_STORE_FAULT: return "store access fault";
        case EXC_ECALL_FROM_U: return "ecall from user mode";
        case EXC_ECALL_FROM_S: return "ecall from supervisor mode";
        case EXC_INSN_PAGE_FAULT: return "instruction page fault";
        case EXC_LOAD_PAGE_FAULT: return "load page fault";
        case EXC_STORE_PAGE_FAULT: return "store page fault";
        default: return "unknown exception";
    }
}

static const char *interrupt_name(uint64_t code) {
    switch (code) {
        case IRQ_S_SOFTWARE: return "software interrupt";
        case IRQ_S_TIMER: return "timer interrupt";
        case IRQ_S_EXTERNAL: return "external interrupt";
        default: return "unknown interrupt";
    }
}

const char *trap_cause_name(uint64_t scause) {
    uint64_t code = scause & ~SCAUSE_INTERRUPT;
    return (scause & SCAUSE_INTERRUPT) != 0 ? interrupt_name(code) : exception_name(code);
}

/* Four registers per line, named. Reading a fault report should not require
 * counting columns. */
static void dump_registers(const struct trapframe *tf) {
    for (uint32_t i = 1; i < 32u; i++) {
        kprintf("%-5s %016llx%s", reg_names[i], (unsigned long long)tf->x[i],
                (i % 4u) == 0 ? "\n" : "  ");
    }
    kprintf("\n");
}

void trap_dump(const struct trapframe *tf) {
    kprintf("trap: %s (scause %016llx)\n", trap_cause_name(tf->scause),
            (unsigned long long)tf->scause);
    kprintf("trap: sepc %016llx  stval %016llx  sstatus %016llx\n",
            (unsigned long long)tf->sepc, (unsigned long long)tf->stval,
            (unsigned long long)tf->sstatus);
    dump_registers(tf);
}

/* What the kernel does with a fault it did not expect, which for now is to
 * report it fully and stop. Resuming would be guessing, and a kernel that
 * guesses after a store fault corrupts whatever it guesses about. */
static void unexpected(struct trapframe *tf) {
    kprintf("\nPANIC: unhandled trap in the kernel\n");
    trap_dump(tf);
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* A handler the self-tests install to step over the instruction that
 * faulted, so a deliberate fault can be taken and survived. */
static bool (*expected_handler)(struct trapframe *tf);

void trap_set_expected(bool (*handler)(struct trapframe *tf)) {
    expected_handler = handler;
}

/* An interrupt, which is a normal event and not a fault: the timer ticks,
 * or a device wants attention. Unlike an exception, `sepc` is left alone —
 * the interrupted instruction has not run yet. */
static bool handle_interrupt(uint64_t code) {
    switch (code) {
    case IRQ_S_TIMER:
        timer_on_tick();
        return true;
    case IRQ_S_EXTERNAL: {
        /* Claim and complete are one register, so a handler cannot end the
         * interrupt without also being ready for the next — which the GIC
         * on the architecture this came from allowed you to get wrong. */
        uint32_t irq = plic_claim();
        if (irq != 0) {
            irq_dispatch(irq);
            plic_complete(irq);
        }
        return true;
    }
    case IRQ_S_SOFTWARE:
        /* Nothing sends one yet; it is how one hart will wake another. */
        __asm__ volatile("csrc sip, %0" : : "r"(2ull));
        return true;
    default:
        return false;
    }
}

static void (*user_return_hook)(struct trapframe *tf);

void trap_set_user_return_hook(void (*hook)(struct trapframe *tf)) {
    user_return_hook = hook;
}

/* Whether this trap came from user mode, which `sstatus.SPP` records: 0 for
 * user, 1 for supervisor. It is the saved value in the frame and not the
 * live register, because the handler itself is running in supervisor mode
 * and the live bit would say so. */
static bool came_from_user(const struct trapframe *tf) {
    return (tf->sstatus & (1ull << 8)) == 0;
}

/* Runs whatever must happen on the way back to user mode — which is where a
 * preemption belongs. Switching threads from **inside** the interrupt
 * handler would resume the other thread with the interrupt not yet
 * acknowledged at the controller, so the tick records the request and it is
 * acted on here. */
static void on_the_way_back(struct trapframe *tf) {
    if (user_return_hook != NULL && came_from_user(tf)) {
        user_return_hook(tf);
    }
}

/* Every trap, holding the kernel lock for as long as it is in the kernel.
 *
 * Taking it here and releasing it on the way out is what makes four harts
 * safe without auditing every structure: one hart is inside the kernel at a
 * time, and the parallelism is in user space. `klock_enter` reports whether
 * it actually took the lock, because a hart may already hold it — an idle
 * hart takes an interrupt with the lock released and re-acquires it, and a
 * thread resumed by the scheduler holds what the scheduler held.
 *
 * The lock is a port rather than a direct dependency: `arch/` must not
 * include `kernel/`, and a trap vector that knew about the scheduler's lock
 * would be the first inward-pointing dependency in the tree.
 */
static bool (*lock_enter)(void);
static void (*lock_exit)(bool);

void trap_set_lock(bool (*enter)(void), void (*exit)(bool)) {
    lock_enter = enter;
    lock_exit = exit;
}

void trap_handle(struct trapframe *tf) {
    bool took = lock_enter != NULL && lock_enter();
    if ((tf->scause & SCAUSE_INTERRUPT) != 0 &&
        handle_interrupt(tf->scause & ~SCAUSE_INTERRUPT)) {
        on_the_way_back(tf);
    } else if (expected_handler != NULL && expected_handler(tf)) {
        on_the_way_back(tf);
    } else {
        /* Does not return: the lock stays held, which is correct for a
         * machine that is about to stop. */
        unexpected(tf);
    }
    if (lock_exit != NULL) {
        lock_exit(took);
    }
}

void trap_enable_interrupts(void) {
    __asm__ volatile("csrsi sstatus, 2" : : : "memory"); /* SIE */
}

void trap_disable_interrupts(void) {
    __asm__ volatile("csrci sstatus, 2" : : : "memory");
}

void trap_init_cpu(void) {
    /* Direct mode: every cause enters at one address. Vectored mode exists
     * and buys nothing here — the handler reads scause either way. */
    __asm__ volatile("csrw stvec, %0" : : "r"((uint64_t)(uintptr_t)trap_entry));
}
