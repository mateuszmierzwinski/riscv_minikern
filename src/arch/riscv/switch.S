/* Switching between two threads' kernel stacks.
 *
 * Only the callee-saved registers and the stack pointer, because that is
 * exactly what a C caller expects to survive a call — everything else is
 * the compiler's to spill, and it already has. A thread that is switched
 * away from is, by construction, inside this function; it resumes by
 * returning from it, with its own stack and its own `ra`.
 *
 * Nothing here touches `satp`: threads share an address space, so there is
 * no table to change and no TLB to flush. That is the whole reason a thread
 * switch is cheaper than a process switch, and it is why `clone` without
 * `CLONE_VM` is refused rather than half-implemented.
 */
#define SW_RA 0
#define SW_S(n) (8 + (n) * 8)
#define SW_SIZE 112

    .section .text
    .globl context_switch
/* context_switch(save_sp = a0, load_sp = a1) */
context_switch:
    addi    sp, sp, -SW_SIZE
    sd      ra, SW_RA(sp)
    sd      s0, SW_S(0)(sp)
    sd      s1, SW_S(1)(sp)
    sd      s2, SW_S(2)(sp)
    sd      s3, SW_S(3)(sp)
    sd      s4, SW_S(4)(sp)
    sd      s5, SW_S(5)(sp)
    sd      s6, SW_S(6)(sp)
    sd      s7, SW_S(7)(sp)
    sd      s8, SW_S(8)(sp)
    sd      s9, SW_S(9)(sp)
    sd      s10, SW_S(10)(sp)
    sd      s11, SW_S(11)(sp)

    /* **Drop any outstanding load-reserved.**
     *
     * A thread can be preempted between its `lr` and its `sc`. The
     * reservation is the *hart's*, not the thread's, so without this the
     * next thread to run on this hart can inherit it — and a
     * store-conditional that should have failed succeeds, because the
     * reservation it checks was taken by somebody else before the value
     * changed. Every atomic read-modify-write built on `lr`/`sc` is then
     * occasionally not atomic.
     *
     * The symptom is nothing to do with locking: a Go program panicking with
     * "sync: WaitGroup misuse" on code that misuses nothing, in about one
     * run in twelve under load. An `sc` invalidates this hart's reservation
     * whether it succeeds or fails, so one instruction to a scratch word is
     * the whole fix. The result is deliberately discarded.
     */
    la      t0, reservation_scratch
    sc.w    zero, zero, (t0)

    sd      sp, 0(a0)               /* where this thread left off */
    mv      sp, a1                  /* the other thread's stack */

    ld      ra, SW_RA(sp)
    ld      s0, SW_S(0)(sp)
    ld      s1, SW_S(1)(sp)
    ld      s2, SW_S(2)(sp)
    ld      s3, SW_S(3)(sp)
    ld      s4, SW_S(4)(sp)
    ld      s5, SW_S(5)(sp)
    ld      s6, SW_S(6)(sp)
    ld      s7, SW_S(7)(sp)
    ld      s8, SW_S(8)(sp)
    ld      s9, SW_S(9)(sp)
    ld      s10, SW_S(10)(sp)
    ld      s11, SW_S(11)(sp)
    addi    sp, sp, SW_SIZE
    ret

/* uspace_enter_frame(frame = a0) — goes to user mode with every register
 * from a saved frame.
 *
 * This is how a **new** thread starts: it has no `uspace_enter` to return
 * through, because nothing called it. It is also the only path that
 * restores the whole register set rather than the callee-saved half, which
 * is what makes a cloned thread indistinguishable from its parent.
 */
#include "arch/riscv/frame.h"

    .globl uspace_enter_frame
uspace_enter_frame:
    /* **Interrupts off first, and this is not a tidiness measure.**
     *
     * `sscratch` is the trap path's one source of truth for "this trap came
     * from user mode": non-zero means user, and the entry code switches to
     * the stack it names. Arming it here therefore makes the next *handful
     * of instructions* lie — they run in the kernel, with interrupts on,
     * with `sscratch` saying user.
     *
     * An interrupt in that window was the four-hart race. The entry took
     * the user path, switched `sp` to this thread's kernel stack top, and
     * wrote the kernel's own register set **over the frame this function is
     * in the middle of loading** — the frame is at exactly that address. On
     * the way out the handler saw `sstatus.SPP` say supervisor, correctly
     * returned to this code, and the `sret` below then entered user mode
     * with a kernel program counter and a kernel stack pointer. That is the
     * fault the board reported as `sepc 0x4020f3fe` and the emulator as an
     * endless instruction page fault five instructions into *this function*
     * — which is what named it.
     *
     * `sret` restores `SIE` from `SPIE`, which is set below, so the thread
     * still gets interrupts the moment it is in user mode. The window is
     * closed rather than narrowed: there is no instruction that sets
     * `sscratch` and returns in one step, so making it small would have
     * left exactly this bug, more rarely. */
    csrci   sstatus, 2              /* SIE: no interrupts until sret */

    /* The stack a trap from this thread will land on: just above the frame,
     * which is at the top of this thread's kernel stack. Exactly what the
     * trap path records on its way back out. */
    addi    t0, a0, FRAME_SIZE
    csrw    sscratch, t0

    /* This hart's id into the frame, so the first trap out of this thread
     * restores the kernel's `tp` rather than the program's (frame.h). */
    sd      tp, FRAME_KTP(a0)

    ld      t0, FRAME_SEPC(a0)
    csrw    sepc, t0
    li      t0, 0x100               /* SPP: to user mode */
    csrc    sstatus, t0
    li      t0, 0x20                /* SPIE: interrupts on once there */
    csrs    sstatus, t0

    ld      x1, FRAME_X(1)(a0)
    ld      x3, FRAME_X(3)(a0)
    ld      x4, FRAME_X(4)(a0)
    ld      x5, FRAME_X(5)(a0)
    ld      x6, FRAME_X(6)(a0)
    ld      x7, FRAME_X(7)(a0)
    ld      x8, FRAME_X(8)(a0)
    ld      x9, FRAME_X(9)(a0)
    ld      x11, FRAME_X(11)(a0)
    ld      x12, FRAME_X(12)(a0)
    ld      x13, FRAME_X(13)(a0)
    ld      x14, FRAME_X(14)(a0)
    ld      x15, FRAME_X(15)(a0)
    ld      x16, FRAME_X(16)(a0)
    ld      x17, FRAME_X(17)(a0)
    ld      x18, FRAME_X(18)(a0)
    ld      x19, FRAME_X(19)(a0)
    ld      x20, FRAME_X(20)(a0)
    ld      x21, FRAME_X(21)(a0)
    ld      x22, FRAME_X(22)(a0)
    ld      x23, FRAME_X(23)(a0)
    ld      x24, FRAME_X(24)(a0)
    ld      x25, FRAME_X(25)(a0)
    ld      x26, FRAME_X(26)(a0)
    ld      x27, FRAME_X(27)(a0)
    ld      x28, FRAME_X(28)(a0)
    ld      x29, FRAME_X(29)(a0)
    ld      x30, FRAME_X(30)(a0)
    ld      x31, FRAME_X(31)(a0)
    /* The stack pointer and a0 last: a0 is the frame's own address, so it
     * can only be overwritten once nothing else needs it. */
    ld      x2, FRAME_X(2)(a0)
    ld      x10, FRAME_X(10)(a0)
    sret

    .section .bss
    .align 3
    .globl reservation_scratch
/* A word an `sc` can be aimed at to drop a reservation. Its contents are
 * never read; one word is enough, because what matters is that the
 * instruction executes, not where it writes. */
reservation_scratch:
    .space 8
