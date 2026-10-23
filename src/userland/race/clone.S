/* `clone`, with the child entering on a frame of its own.
 *
 * Writing this in C does not work, and the way it fails is worth knowing.
 * `clone` returns twice: once in the parent and once in the child, and the
 * child returns **into the caller's C function** with its stack pointer set
 * to the new stack. That function's frame belongs to the parent's stack, so
 * every local the compiler kept in a stack slot is now addressed relative to
 * the child's fresh stack pointer — above it, where nothing is mapped. The
 * child then stores into memory that is not its own, or faults.
 *
 * Measured: a C version faulted every run at `sp + 16`, sixteen bytes past
 * the top of the stack it had just been given, before it executed a single
 * statement of its own.
 *
 * So the child never returns to C. It is handed an entry point, given a null
 * return address so a backtrace stops cleanly, and exits if that ever
 * returns.
 *
 *   long clone_thread(long flags, void *stack_top, void (*entry)(void));
 */
    .section .text
    .globl clone_thread
clone_thread:
    mv      t0, a2                  /* the entry point, kept out of the ABI's way */
    li      a2, 0                   /* ptid */
    li      a3, 0                   /* tls */
    li      a4, 0                   /* ctid */
    li      a7, 220                 /* SYS_clone */
    ecall
    bnez    a0, 1f                  /* parent, or an error: hand it back */

    /* The child. Its stack pointer is the one the caller asked for, and
     * nothing of the parent's frame is reachable from it. */
    mv      ra, zero
    jalr    t0

    /* The entry point returned, which it is not supposed to do. */
    li      a0, 0
    li      a7, 93                  /* SYS_exit */
    ecall
1:  ret
