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
void trap_disable_interrupts(void);

/* Installs a handler for faults the kernel is **expecting** — a self-test
 * taking a deliberate exception. It returns true when it has dealt with the
 * frame (typically by advancing `sepc` past the instruction), and the trap
 * returns; false means the fault was not the expected one and the kernel
 * reports it and stops. NULL clears it. */
void trap_set_expected(bool (*handler)(struct trapframe *tf));

#endif
