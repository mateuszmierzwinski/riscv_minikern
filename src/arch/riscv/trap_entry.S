/* The one trap vector.
 *
 * Saves x1..x31 and the four CSRs that describe what happened, calls C with
 * a pointer to the frame, restores, and returns with `sret`.
 *
 * **Two origins, one path.** A trap from the kernel is already on a kernel
 * stack; a trap from user mode is on the *user's* stack, which the kernel
 * must not touch — so `sscratch` holds the kernel stack top while user code
 * runs, and zero while the kernel runs. One swap at the top therefore both
 * finds the right stack and says where the trap came from, and the same
 * register saves the interrupted `sp` either way.
 *
 * Zeroing `sscratch` on entry matters: a fault *inside* the handler must
 * take the kernel path, not swap to a stack that is already in use. And it
 * is set again on the way out only when returning to user mode, which the
 * saved `sstatus.SPP` says.
 *
 * The ARM equivalent of all this was a different stack *and* a different
 * register bank per mode, with eight vectors to keep in step.
 */
#include "arch/riscv/frame.h"

    .section .text
    .globl trap_entry
    .align 4                      /* stvec requires a 4-byte aligned address */
trap_entry:
    /* Swap in the kernel stack. From user mode sscratch holds it and sp
     * becomes it; from the kernel sscratch is zero, so sp becomes zero and
     * the old sp is in sscratch — recovered two instructions later. */
    csrrw   sp, sscratch, sp
    bnez    sp, 1f
    csrr    sp, sscratch            /* came from the kernel: that was our sp */
1:  addi    sp, sp, -FRAME_SIZE

    sd      x1, FRAME_X(1)(sp)
    sd      x3, FRAME_X(3)(sp)
    sd      x4, FRAME_X(4)(sp)
    sd      x5, FRAME_X(5)(sp)
    sd      x6, FRAME_X(6)(sp)
    sd      x7, FRAME_X(7)(sp)
    sd      x8, FRAME_X(8)(sp)
    sd      x9, FRAME_X(9)(sp)
    sd      x10, FRAME_X(10)(sp)
    sd      x11, FRAME_X(11)(sp)
    sd      x12, FRAME_X(12)(sp)
    sd      x13, FRAME_X(13)(sp)
    sd      x14, FRAME_X(14)(sp)
    sd      x15, FRAME_X(15)(sp)
    sd      x16, FRAME_X(16)(sp)
    sd      x17, FRAME_X(17)(sp)
    sd      x18, FRAME_X(18)(sp)
    sd      x19, FRAME_X(19)(sp)
    sd      x20, FRAME_X(20)(sp)
    sd      x21, FRAME_X(21)(sp)
    sd      x22, FRAME_X(22)(sp)
    sd      x23, FRAME_X(23)(sp)
    sd      x24, FRAME_X(24)(sp)
    sd      x25, FRAME_X(25)(sp)
    sd      x26, FRAME_X(26)(sp)
    sd      x27, FRAME_X(27)(sp)
    sd      x28, FRAME_X(28)(sp)
    sd      x29, FRAME_X(29)(sp)
    sd      x30, FRAME_X(30)(sp)
    sd      x31, FRAME_X(31)(sp)

    /* The interrupted stack pointer. From user mode it is in sscratch (the
     * swap put it there); from the kernel it is just above this frame. */
    csrr    t0, sscratch
    bnez    t0, 2f
    addi    t0, sp, FRAME_SIZE
2:  sd      t0, FRAME_X(2)(sp)

    /* In the kernel now, so a fault inside the handler takes the kernel
     * path instead of swapping to a stack that is already in use. */
    csrw    sscratch, zero

    csrr    t0, sepc
    sd      t0, FRAME_SEPC(sp)
    csrr    t0, sstatus
    sd      t0, FRAME_SSTATUS(sp)
    csrr    t0, scause
    sd      t0, FRAME_SCAUSE(sp)
    csrr    t0, stval
    sd      t0, FRAME_STVAL(sp)

    mv      a0, sp
    call    trap_handle

    /* sepc may have been advanced by the handler — past an ecall, or past an
     * instruction a self-test meant to skip — so it is reloaded rather than
     * assumed unchanged. */
    ld      t0, FRAME_SEPC(sp)
    csrw    sepc, t0
    ld      t0, FRAME_SSTATUS(sp)
    csrw    sstatus, t0

    /* Returning to user mode? Then leave the kernel stack top in sscratch
     * for the next entry. SPP is bit 8 of sstatus: 0 means user. */
    andi    t1, t0, 0x100
    bnez    t1, 3f
    addi    t1, sp, FRAME_SIZE
    csrw    sscratch, t1
3:

    ld      x1, FRAME_X(1)(sp)
    ld      x3, FRAME_X(3)(sp)
    ld      x4, FRAME_X(4)(sp)
    ld      x5, FRAME_X(5)(sp)
    ld      x6, FRAME_X(6)(sp)
    ld      x7, FRAME_X(7)(sp)
    ld      x8, FRAME_X(8)(sp)
    ld      x9, FRAME_X(9)(sp)
    ld      x10, FRAME_X(10)(sp)
    ld      x11, FRAME_X(11)(sp)
    ld      x12, FRAME_X(12)(sp)
    ld      x13, FRAME_X(13)(sp)
    ld      x14, FRAME_X(14)(sp)
    ld      x15, FRAME_X(15)(sp)
    ld      x16, FRAME_X(16)(sp)
    ld      x17, FRAME_X(17)(sp)
    ld      x18, FRAME_X(18)(sp)
    ld      x19, FRAME_X(19)(sp)
    ld      x20, FRAME_X(20)(sp)
    ld      x21, FRAME_X(21)(sp)
    ld      x22, FRAME_X(22)(sp)
    ld      x23, FRAME_X(23)(sp)
    ld      x24, FRAME_X(24)(sp)
    ld      x25, FRAME_X(25)(sp)
    ld      x26, FRAME_X(26)(sp)
    ld      x27, FRAME_X(27)(sp)
    ld      x28, FRAME_X(28)(sp)
    ld      x29, FRAME_X(29)(sp)
    ld      x30, FRAME_X(30)(sp)
    ld      x31, FRAME_X(31)(sp)

    /* The interrupted sp last, because everything above is addressed
     * through this frame. For a kernel return it is this frame's top; for a
     * user return it is the user's own stack, which is why it comes from the
     * frame rather than from arithmetic. */
    ld      sp, FRAME_X(2)(sp)
    sret
