/* A user program, as instructions rather than an ELF.
 *
 * It exists to prove the mode switch, the trap path out of user mode and the
 * syscall convention — before an ELF loader is written on top of any of
 * them. Three untested things at once is how a bring-up becomes a guessing
 * game; the ARM project's equivalent was the same shape, and the first thing
 * it caught was a start state the loader had nothing to do with.
 *
 * It uses the generic Linux calling convention, which is what Go will use:
 * the number in `a7`, arguments in `a0` onwards, `ecall`, result in `a0`.
 * Written position-independently, because the kernel maps it wherever it
 * likes and a `la` of an absolute address would be a lie.
 */
    .section .user_program, "ax"
    .globl user_program_start
    .globl user_program_end

#define SYS_WRITE 64
#define SYS_EXIT 93

user_program_start:
    /* write(1, message, length) — the address comes from the program
     * counter, so it is right wherever this page was mapped. */
    li      a0, 1
    lla     a1, message
    li      a2, message_end - message
    li      a7, SYS_WRITE
    ecall

    /* A second call, so the test can prove the kernel came *back* into user
     * mode rather than merely reaching it once. A single syscall followed by
     * an exit would look identical to a kernel that never returned. */
    li      a0, 1
    lla     a1, again
    li      a2, again_end - again
    li      a7, SYS_WRITE
    ecall

    /* Something only user mode can do wrongly: reading a kernel address.
     * The page is mapped in this address space — it has to be, or the trap
     * handler could not run — but without PTE_U, so user mode cannot touch
     * it. The kernel reports the fault and ends the process, which is what
     * makes it a user fault rather than a kernel one. Commented out here
     * because the test runs it as a separate program. */

    li      a0, 7                  /* an exit status the test can check */
    li      a7, SYS_EXIT
    ecall

    /* Unreachable: exit does not return. If it ever did, stop rather than
     * running into whatever follows. */
1:  j       1b

message:
    .ascii  "hello from user mode on RISC-V\n"
message_end:
again:
    .ascii  "and again, so the kernel came back\n"
again_end:
user_program_end:

/* A second program, which faults on purpose: it reads an address that is
 * mapped for the kernel and not for user mode. */
    .globl user_faulter_start
    .globl user_faulter_end
user_faulter_start:
    li      a0, 1
    lla     a1, about_to
    li      a2, about_to_end - about_to
    li      a7, SYS_WRITE
    ecall
    /* The kernel's own text, which every address space maps and no user
     * page table marks reachable from user mode. */
    li      t0, 0x80200000
    ld      t1, 0(t0)
    /* Not reached: the load faults. */
    li      a0, 0
    li      a7, SYS_EXIT
    ecall
2:  j       2b
about_to:
    .ascii  "about to touch the kernel's memory\n"
about_to_end:
user_faulter_end:
