/* trap — exceptions and interrupts, of which RISC-V has one entry point.
 *
 * This is where the architecture pays off most visibly. The ARM kernel this
 * migrated from needed **eight** vectors, each a branch into mode-specific
 * code, with registers banked per mode so that reading the interrupted
 * context meant knowing which mode you came from — and a trap from user mode
 * landed on a different stack from a trap in the kernel.
 *
 * Here there is one `stvec`, and the cause is a number in `scause`. The
 * frame is saved on the stack that was in use, the handler decides from the
 * cause, and `sret` returns. One path, one frame layout, one place to look.
 *
 * `scause`'s top bit distinguishes an interrupt from an exception, and the
 * rest is the code. `sepc` is where it happened and `stval` is the address or
 * instruction that caused it — which is what makes a fault report useful
 * rather than merely alarming.
 */
#ifndef MINIKERN_ARCH_RISCV_TRAP_H
#define MINIKERN_ARCH_RISCV_TRAP_H

#include <stdbool.h>
#include <stdint.h>

/* The interrupted context, as the entry code saves it. The register order is
 * x1..x31 so that a dump can be printed by index, with the CSRs after. */
struct trapframe {
    uint64_t x[32];   /* x[0] is unused: x0 is the zero register */
    uint64_t sepc;    /* where the trap happened, and where sret returns */
    uint64_t sstatus;
    uint64_t scause;
    uint64_t stval;
    uint64_t kernel_tp;  /* the hart id to restore on a trap from user mode */
    /* Non-zero when the entry code took its **user** path, which it decides
     * by `sscratch` being armed. `sstatus.SPP` is the hardware's answer to
     * the same question, and `trap_mismatched_entries` counts the times
     * they disagree — see `frame.h`. It was the frame's alignment padding. */
    uint64_t entry;
};

/* Exception causes this kernel names. The rest are printed by number. */
#define SCAUSE_INTERRUPT (1ull << 63)
#define EXC_INSN_MISALIGNED 0u
#define EXC_INSN_FAULT 1u
#define EXC_ILLEGAL_INSN 2u
#define EXC_BREAKPOINT 3u
#define EXC_LOAD_MISALIGNED 4u
#define EXC_LOAD_FAULT 5u
#define EXC_STORE_MISALIGNED 6u
#define EXC_STORE_FAULT 7u
#define EXC_ECALL_FROM_U 8u
#define EXC_ECALL_FROM_S 9u
#define EXC_INSN_PAGE_FAULT 12u
#define EXC_LOAD_PAGE_FAULT 13u
#define EXC_STORE_PAGE_FAULT 15u

/* Interrupt causes, which are the same numbers with the top bit set. */
#define IRQ_S_SOFTWARE 1u
#define IRQ_S_TIMER 5u
#define IRQ_S_EXTERNAL 9u

/* Points this hart's `stvec` at the entry code. Per hart, because `stvec` is
 * per hart — the ARM project learned that lesson the hard way, when a core
 * that skipped its own vector-table write sent its first exception to
 * address zero. */
void trap_init_cpu(void);

/* What a cause code means, for a report. Never NULL. */
const char *trap_cause_name(uint64_t scause);

/* Prints the frame: the cause, where it happened, the faulting address and
 * every register. The ARM project's equivalent is what made each of its
 * bring-up faults diagnosable instead of a silent hang. */
void trap_dump(const struct trapframe *tf);

/* Called from the entry code with the saved frame. Exported because the
 * assembly calls it; not meant for anyone else. */
void trap_handle(struct trapframe *tf);

/* Lets interrupts reach the kernel at all (`sstatus.SIE`). Separate from
 * enabling any particular source, so the kernel decides when it is ready to
 * be interrupted rather than discovering it was not. */
void trap_enable_interrupts(void);

/* Sleeps until an interrupt is pending, then takes it, and returns with
 * interrupts off as they were.
 *
 * **`wfi` runs with interrupts OFF.** The old idiom — enable, `wfi`,
 * disable — loses a wake: an interrupt already pending is taken at the
 * enable, BEFORE the `wfi`, and the trap returns to the `wfi` with nothing
 * left to wake it. For the timer that was fatal, because the handler
 * disarms the comparator: the hart then slept with no timer at all until a
 * stray device interrupt, and a 1 ms sleeper woke up to five seconds late.
 * A pending interrupt that is enabled in `sie` ends `wfi` whatever
 * `sstatus.SIE` says, so this cannot sleep through one. */
void trap_wait_for_interrupt(void);
void trap_disable_interrupts(void);

/* Installs a handler for faults the kernel is **expecting** — a self-test
 * taking a deliberate exception. It returns true when it has dealt with the
 * frame (typically by advancing `sepc` past the instruction), and the trap
 * returns; false means the fault was not the expected one and the kernel
 * reports it and stops. NULL clears it. */
void trap_set_expected(bool (*handler)(struct trapframe *tf));

/* The kernel lock, as a port: taken on entering the kernel and released on
 * leaving it. A port rather than a direct call because `arch/` may not
 * include `kernel/` — a trap vector that knew about the scheduler's lock
 * would be the first inward-pointing dependency in the tree. `enter`
 * reports whether it took the lock; `exit` is given that answer back. */
void trap_set_lock(bool (*enter)(void), void (*exit)(bool));

/* Where the trap path reports what it is doing while the kernel lock is held,
 * so that the longest hold can be attributed to a kind of trap. Optional: a
 * kernel with no tag port measures the same times and cannot name them. */
void trap_set_lock_tag(void (*tag)(uint64_t));

/* A hook run on the way back to user mode, after a trap that came from
 * there has been handled. This is where a preemption belongs: inside the
 * interrupt handler the controller has not been told the interrupt is
 * finished, so a thread switch from there resumes the other thread with the
 * interrupt still pending. NULL to remove it. */
void trap_set_user_return_hook(void (*hook)(struct trapframe *tf));

/* Traps that entered by the user path while the hardware said they came
 * from the kernel — `sscratch` armed with the kernel still running. Any
 * value but zero means a trap frame was written over a stack that was in
 * use, which is how a kernel register set reaches user mode. */
uint64_t trap_mismatched_entries(void);

/* The most harts that have been inside the kernel at the same time. One, by
 * construction — the kernel lock is what makes four harts safe without
 * auditing every structure here — and every argument that ends "but the lock
 * prevents that" rests on it. */
uint32_t trap_most_harts_inside(void);

/* How long a hart (by enumeration index) has spent in user mode: from each
 * return to user mode to the trap that next brought it back. */
uint64_t trap_user_ns(uint32_t index);

/* Traps taken since boot by cause: `interrupt` picks the half of `scause`,
 * `code` the cause within it (0 for a code past TRAP_CAUSES). */
#define TRAP_CAUSES 16u
uint64_t trap_cause_count(bool interrupt, uint32_t code);

/* Who is told how long each stretch in user mode lasted, on the hart that
 * ran it, as it ends — before the kernel lock is taken. NULL for nobody. */
void trap_set_user_time_sink(void (*sink)(uint64_t ns));

/* Marks the calling hart as entering user mode now — for the one way into
 * user mode that is not a return from a trap, a thread's first start. */
void trap_note_entering_user(void);

/* How many software interrupts (cross-hart pokes) this machine has actually
 * taken as traps, which is not the same as how many were sent. */
uint64_t trap_software_interrupts(void);

/* Prints what the hart at `index` last trapped on, and whether it got the
 * kernel lock: for a hart that will not answer. */
void trap_report_hart(uint32_t index);

#endif
