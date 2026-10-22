/* Entering user mode, and getting back out.
 *
 * `sret` does not return: it goes to user mode, and the kernel is next
 * entered through the trap vector, on the kernel stack, with no memory of
 * the function that let the process go. So "run this process until it stops"
 * needs somewhere to come back *to* — these two functions are that.
 *
 * `uspace_enter` saves the callee-saved registers and the stack pointer,
 * then returns to user mode. `uspace_leave` restores them and returns, so
 * control reappears at `uspace_enter`'s caller as though it had simply
 * returned. It is a one-way jump between two stacks, which is the honest
 * shape of this: the process did not return, the kernel resumed.
 *
 * Only the callee-saved set is kept, because that is exactly what a C
 * caller expects to survive a call — the rest is the compiler's to spill.
 */
#include "arch/riscv/frame.h"

#define CTX_RA 0
#define CTX_SP 8
#define CTX_S(n) (16 + (n) * 8)

    .section .text
    .globl uspace_enter
/* uspace_enter(entry = a0, user_sp = a1, context = a2) */
uspace_enter:
    sd      ra, CTX_RA(a2)
    sd      sp, CTX_SP(a2)
    sd      s0, CTX_S(0)(a2)
    sd      s1, CTX_S(1)(a2)
    sd      s2, CTX_S(2)(a2)
    sd      s3, CTX_S(3)(a2)
    sd      s4, CTX_S(4)(a2)
    sd      s5, CTX_S(5)(a2)
    sd      s6, CTX_S(6)(a2)
    sd      s7, CTX_S(7)(a2)
    sd      s8, CTX_S(8)(a2)
    sd      s9, CTX_S(9)(a2)
    sd      s10, CTX_S(10)(a2)
    sd      s11, CTX_S(11)(a2)

    /* The stack a trap from user mode will land on: this frame's. The trap
     * path swaps it in from sscratch, which is how it knows the trap came
     * from user mode at all. */
    csrw    sscratch, sp

    li      t0, 0x100               /* SPP: clear it, so sret goes to user */
    csrc    sstatus, t0
    li      t0, 0x20                /* SPIE: interrupts on once there */
    csrs    sstatus, t0
    csrw    sepc, a0
    mv      sp, a1                  /* the process's own stack */
    sret

    .globl uspace_leave
/* uspace_leave(context = a0) — does not return to its caller. Control
 * reappears wherever uspace_enter was called from. */
uspace_leave:
    ld      ra, CTX_RA(a0)
    ld      s0, CTX_S(0)(a0)
    ld      s1, CTX_S(1)(a0)
    ld      s2, CTX_S(2)(a0)
    ld      s3, CTX_S(3)(a0)
    ld      s4, CTX_S(4)(a0)
    ld      s5, CTX_S(5)(a0)
    ld      s6, CTX_S(6)(a0)
    ld      s7, CTX_S(7)(a0)
    ld      s8, CTX_S(8)(a0)
    ld      s9, CTX_S(9)(a0)
    ld      s10, CTX_S(10)(a0)
    ld      s11, CTX_S(11)(a0)
    ld      sp, CTX_SP(a0)
    ret
