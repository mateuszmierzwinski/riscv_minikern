/* A program's first instruction.
 *
 * The kernel leaves the stack pointer at `argc`, with everything else above
 * it — so a program's entry point is not a function with arguments, it is a
 * place where the stack already means something. All this does is hand that
 * pointer to C, which can then read the structure.
 *
 * No frame is set up and nothing is saved: there is nothing to return to.
 */
    .section .text.start, "ax"
    .globl _start
_start:
    mv      a0, sp          /* the stack as the kernel built it */
    call    user_main
    /* user_main calls exit; if it ever returned, stop rather than running
     * into whatever follows. */
1:  j       1b
