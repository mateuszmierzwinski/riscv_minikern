#include "arch/riscv/trap.h"
#include "kernel/entropy.h"

#include "arch/riscv/frame.h"
#include "arch/riscv/plic.h"
#include "arch/riscv/smp.h"
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
_Static_assert(__builtin_offsetof(struct trapframe, entry) == FRAME_ENTRY, "entry offset");
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
/* How many software interrupts have actually been **taken**, as against
 * sent. The two are not the same thing and the difference is invisible
 * without counting: a poke to an idle hart ends its `wfi` whether or not
 * the interrupt is enabled, so the whole poking mechanism appears to work
 * while no hart ever traps on one — and then a poke whose purpose is to
 * interrupt a *busy* hart does nothing at all. */
static uint64_t software_interrupts;

uint64_t trap_software_interrupts(void) {
    return software_interrupts;
}

static bool handle_interrupt(uint64_t code) {
    /* **Every interrupt feeds the entropy pool.**
     *
     * `entropy_add_timing` had no callers at all, so the pool stayed zero,
     * `entropy_fill` reported itself unseeded, and `random_fill` hashed the
     * same zeroed pool on every boot — which made address randomisation
     * produce *identical* addresses in two consecutive runs. A complete,
     * well-written mechanism that nothing ever fed.
     *
     * The cycle counter's low bits are the sample. What makes them vary is
     * that the interrupted work does: a timer tick lands at a slightly
     * different point in whatever the hart was doing, and on real hardware
     * that spread is real. Under emulation it is much smaller, which is why
     * the ARM project this came from ended up here too.
     *
     * In the trap path, so it must stay cheap: a read of a counter and an
     * exclusive-or. The hashing happens when the pool is read. */
    {
        uint64_t now;
        __asm__ volatile("rdcycle %0" : "=r"(now));
        entropy_add_timing((uint32_t)now);
    }
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
        software_interrupts++;
        /* Clearing it is the whole handler. A hart is poked because the
         * answer to "what should I be running?" has changed — work has
         * appeared, or what it is running should stop — and that question
         * is asked on the way out of every trap anyway. The interrupt's job
         * is only to end the `wfi`. */
        __asm__ volatile("csrc sip, %0" : : "r"(2ull));
        return true;
    default:
        return false;
    }
}

/* Traps whose two answers to "where did this come from?" disagreed. */
static uint64_t mismatched_entries;

uint64_t trap_mismatched_entries(void) {
    return mismatched_entries;
}

/* **The two sources of truth, compared.**
 *
 * The entry code decides "this came from user mode" from `sscratch` being
 * armed; the hardware records it in `sstatus.SPP`. They agree unless
 * `sscratch` is armed while the kernel is still running — which is a window
 * the kernel opens itself, and in that window a trap writes its frame onto
 * a stack that is in use. The consequence was a thread entering user mode
 * with a kernel program counter, found as an endless instruction page fault
 * inside `uspace_enter_frame`.
 *
 * It is checked rather than reasoned about because the window was closed by
 * argument once already: the comment in `trap_entry.S` said `sscratch` is
 * "zero while the kernel runs", and it was not. */
static void check_the_entry_agrees(const struct trapframe *tf) {
    bool by_user_path = tf->entry != 0;
    bool from_user = (tf->sstatus & (1ull << 8)) == 0;
    if (by_user_path == from_user) {
        return;
    }
    if (mismatched_entries++ == 0) {
        kprintf("\ntrap: **this trap entered by the %s path but came from %s** —"
                " sepc %016llx sp %016llx\n",
                by_user_path ? "user" : "kernel", from_user ? "user mode" : "the kernel",
                (unsigned long long)tf->sepc, (unsigned long long)tf->x[2]);
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

static void (*lock_tag)(uint64_t);

void trap_set_lock_tag(void (*tag)(uint64_t)) {
    lock_tag = tag;
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


/* How many harts have been inside the kernel at the same time, and the most
 * there has ever been.
 *
 * One, by construction: `klock_enter` on the way in and `klock_exit` on the
 * way out are what make four harts safe without auditing every structure in
 * this kernel. **Which is exactly why it is counted.** Every theory about
 * the four-hart race that ends "but the lock prevents that" rests on this
 * number being one, and nothing had ever checked it. */
static uint32_t inside_the_kernel;
static uint32_t most_inside;

uint32_t trap_most_harts_inside(void) {
    return most_inside;
}

/* What each hart last brought into the kernel, read by `trap_report_hart`.
 * `entered` and `locked` differ while a hart waits for the kernel lock;
 * neither moving means the hart never reached C at all. */
static struct {
    uint64_t scause, sepc, stval;
    uint32_t entered, locked;
} last_trap[SMP_MAX_HARTS];

void trap_report_hart(uint32_t index) {
    if (index >= SMP_MAX_HARTS) {
        return;
    }
    kprintf("trap: hart index %u entered %u locked %u; last %s sepc %016llx stval %016llx\n",
            index, last_trap[index].entered, last_trap[index].locked,
            trap_cause_name(last_trap[index].scause),
            (unsigned long long)last_trap[index].sepc, (unsigned long long)last_trap[index].stval);
}

/* User time, per hart: when the hart last went to user mode (0: it is in
 * the kernel), and the total. Each is written only by its own hart. */
static struct {
    uint64_t entered_user_at;
    uint64_t user_ns;
} user_time[SMP_MAX_HARTS];

/* Trap counts by cause, written under the kernel lock (`note_trap`). */
static uint64_t cause_counts[2][TRAP_CAUSES];
static void (*user_time_sink)(uint64_t ns);

uint64_t trap_cause_count(bool interrupt, uint32_t code) {
    return code < TRAP_CAUSES ? cause_counts[interrupt ? 1 : 0][code] : 0u;
}

void trap_set_user_time_sink(void (*sink)(uint64_t ns)) {
    user_time_sink = sink;
}

static void count_cause(uint64_t scause) {
    uint64_t code = scause & ~SCAUSE_INTERRUPT;
    cause_counts[(scause & SCAUSE_INTERRUPT) != 0 ? 1 : 0][code < TRAP_CAUSES ? code : 0u]++;
}

uint64_t trap_user_ns(uint32_t index) {
    return index < SMP_MAX_HARTS ? user_time[index].user_ns : 0u;
}

void trap_note_entering_user(void) {
    uint32_t h = smp_hart_index();
    if (h < SMP_MAX_HARTS) {
        user_time[h].entered_user_at = timer_ns();
    }
}

/* Closes the stretch in user mode a trap from user mode ends. */
static void note_leaving_user(const struct trapframe *tf) {
    uint32_t h = smp_hart_index();
    if (h >= SMP_MAX_HARTS || (tf->sstatus & (1ull << 8)) != 0 ||
        user_time[h].entered_user_at == 0) {
        return;
    }
    {
        uint64_t ns = timer_ns() - user_time[h].entered_user_at;
        user_time[h].user_ns += ns;
        if (user_time_sink != NULL) {
            user_time_sink(ns);
        }
    }
    user_time[h].entered_user_at = 0;
}

/* Opens one on the way back, on whichever hart the thread now runs. */
static void note_returning_to_user(const struct trapframe *tf) {
    if ((tf->sstatus & (1ull << 8)) == 0) {
        trap_note_entering_user();
    }
}

/* Records a trap against the hart taking it, before and after the lock. */
static void note_trap(const struct trapframe *tf, bool after_lock) {
    uint32_t h = smp_hart_index();
    if (h >= SMP_MAX_HARTS) {
        return;
    }
    if (!after_lock) {
        last_trap[h].scause = tf->scause;
        last_trap[h].sepc = tf->sepc;
        last_trap[h].stval = tf->stval;
        last_trap[h].entered++;
        return;
    }
    last_trap[h].locked++;
    count_cause(tf->scause);
}

void trap_handle(struct trapframe *tf) {
    note_leaving_user(tf);
    note_trap(tf, false);
    bool took = lock_enter != NULL && lock_enter();
    note_trap(tf, true);
    /* The cause, so that the longest hold can be attributed to a kind of trap
     * rather than to `trap_handle`, which is where every one of them takes
     * the lock. A syscall overwrites this with its own number. */
    if (lock_tag != NULL) {
        lock_tag(tf->scause);
    }
    uint32_t now = __atomic_add_fetch(&inside_the_kernel, 1u, __ATOMIC_RELAXED);
    most_inside = now > most_inside ? now : most_inside;
    check_the_entry_agrees(tf);
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
    __atomic_sub_fetch(&inside_the_kernel, 1u, __ATOMIC_RELAXED);
    note_returning_to_user(tf);
    if (lock_exit != NULL) {
        lock_exit(took);
    }
}

void trap_enable_interrupts(void) {
    __asm__ volatile("csrsi sstatus, 2" : : : "memory"); /* SIE */
}

void trap_wait_for_interrupt(void) {
    __asm__ volatile("wfi\n\tcsrsi sstatus, 2\n\tnop\n\tcsrci sstatus, 2" : : : "memory");
}

void trap_disable_interrupts(void) {
    __asm__ volatile("csrci sstatus, 2" : : : "memory");
}

void trap_init_cpu(void) {
    /* Direct mode: every cause enters at one address. Vectored mode exists
     * and buys nothing here — the handler reads scause either way. */
    __asm__ volatile("csrw stvec, %0" : : "r"((uint64_t)(uintptr_t)trap_entry));
}
