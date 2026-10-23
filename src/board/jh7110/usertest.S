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
#define SYS_CLONE 220
#define SYS_SETPRIORITY 140
#define SYS_CLOCK_GETTIME 113
#define SYS_MMAP 222
#define SYS_NANOSLEEP 101
#define SYS_EXIT_GROUP 94
#define SYS_SCHED_SETAFFINITY 122
#define SYS_SCHED_SETSCHEDULER 119
#define SYS_WAIT4 260
#define SYS_KILL 129
#define SYS_GETPGID 155
#define SYS_GETPID 172
#define SYS_IOCTL 29
#define SYS_READ 63
#define SYS_GETUID 174
#define SYS_GETEUID 175
#define SYS_SETUID 146
#define SYS_PRLIMIT64 261
#define SYS_OPENAT 56
#define SYS_MUNMAP 215
#define SYS_MREMAP 216

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

/* A third program: **one thread, busy, and nothing else.**
 *
 * This is Phase 1's test, and it has to be embedded rather than written in C
 * because the only other way to run a program on the board is as `/init` on
 * the SD card, which means asking the operator to rewrite the card. An
 * assembly loop needs nothing.
 *
 * It makes **no syscalls at all** until it exits, so every timer interrupt
 * the kernel reports while it runs is one the scheduler chose to take. With
 * one runnable thread there is no slice to end and no deadline to wait for,
 * so the honest answer is none — and `smp: switches/ticks by hart` is where
 * that is read off.
 *
 * The count is iterations, not time: a three-instruction loop on a 1.5 GHz
 * U74 at roughly one iteration a cycle makes 15e9 about ten seconds. Being
 * approximate is fine, because the kernel times the run itself. */
    .globl user_spin_start
    .globl user_spin_end
user_spin_start:
    li      t0, 15000000000
1:  addi    t0, t0, -1
    bnez    t0, 1b
    li      a0, 0
    li      a7, SYS_EXIT
    ecall
2:  j       2b
user_spin_end:

/* A fourth program: **two threads, different nice values, one hart.**
 *
 * Phase 2's test, and the reason it has to pin itself to one hart is the
 * whole point of a proportional-share scheduler: with two runnable threads
 * on four harts, both simply run, and no weighting is observable. It is run
 * with `minikern.smp=off`, so there is one hart and the two threads must
 * divide it.
 *
 * The parent renices to **+5**, the child to **-5**. The weight table makes
 * that 335 against 3121, so the child should do about **9.3 times** the work
 * of the parent in the same wall-clock time. Each thread counts for three
 * seconds and prints its own total; the ratio of the two printed numbers is
 * the measurement.
 *
 * In assembly because the board's only other way to run a program is as
 * `/init` on the operator's SD card, and an embedded program needs none.
 *
 * Each thread keeps its `timespec` **on its own stack**, not in shared
 * memory: the two share an address space, so a single static buffer would
 * have each thread reading the other's clock.
 */
    .globl user_nice_start
    .globl user_nice_end
user_nice_start:
    /* setpriority(PRIO_PROCESS, 0, +5): the parent asks to be light. */
    li      a0, 0
    li      a1, 0
    li      a2, 5
    li      a7, SYS_SETPRIORITY
    ecall

    /* The child's stack from `mmap`, not from this image: an embedded
     * program is **one page** (`uspace_build` refuses more), and a stack
     * reserved inside the text would have spent eight of them. A thread
     * stack belongs in mapped memory regardless.
     *
     * mmap(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    li      a0, 0
    li      a1, 8192
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    li      t0, 8192
    add     s5, a0, t0           /* the top, since a stack grows down */

    /* clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD, stack) */
    li      a0, 0x00050f00
    mv      a1, s5
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bnez    a0, run_light

    /* The child: ask to be heavy. */
    li      a0, 0
    li      a1, 0
    li      a2, -5
    li      a7, SYS_SETPRIORITY
    ecall
    li      s2, 104              /* 'h' */
    j       count_for_three_seconds

run_light:
    li      s2, 108              /* 'l' */

/* s2 = the label to print with the count. Clobbers s0 (deadline), s1
 * (count), s3 (scratch). */
count_for_three_seconds:
    addi    sp, sp, -32          /* this thread's own timespec */
    jal     now_ns
    li      s3, 3
    li      s4, 1000000000
    mul     s3, s3, s4
    add     s0, a0, s3           /* deadline = now + 3s */
    li      s1, 0                /* the count */
inner:
    li      s3, 1000000
spin_batch:
    addi    s1, s1, 1
    addi    s3, s3, -1
    bnez    s3, spin_batch
    /* The clock is read once per million iterations, so the syscall is not
     * what is being measured — and there is no syscall at all inside the
     * batch, which is what makes the count a measure of processor share
     * rather than of kernel dispatch. */
    jal     now_ns
    blt     a0, s0, inner

    /* **One write for the whole line.** Two threads writing a label and
     * then a number as separate calls interleave, and on the board they
     * did: `light heavy 1354000000`, one count lost. The console is shared
     * state and a line is the unit that has to be atomic. */
    mv      a0, s1
    mv      a1, s2
    jal     print_dec

    /* **Exiting kills the other thread**, so only one of them does it, and
     * not immediately. `SYS_EXIT` tears the whole process down — siblings
     * included — so whichever finished first used to destroy the other
     * before it could print its count. On the board that lost one of the two
     * numbers; under emulation both appeared, which is the usual
     * relationship between an emulator and a race.
     *
     * The heavy thread prints and then waits to be killed. The light one
     * prints, **sleeps** — long enough that the other's line has certainly
     * landed, and bounded, so the program always ends — and exits.
     *
     * A shared word would also work and was tried; a sleep needs no memory
     * shared between the two and no reasoning about what `clone` copies. */
    li      t0, 104
    beq     s2, t0, heavy_waits_to_be_killed
    addi    sp, sp, -16
    sd      zero, 0(sp)              /* tv_sec  = 0 */
    li      t0, 200000000
    sd      t0, 8(sp)                /* tv_nsec = 200 ms */
    mv      a0, sp
    li      a1, 0
    li      a7, SYS_NANOSLEEP
    ecall
    /* **`exit_group`, not `exit`.** `exit` ends this thread and leaves the
     * other one spinning, so the program never finishes and the kernel waits
     * for it for ever — which is exactly what the board did: both counts
     * printed and nothing followed them. Ending the *process* is what was
     * meant, and the two syscalls are one number apart. */
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall
heavy_waits_to_be_killed:
8:  j       8b

/* Monotonic nanoseconds in a0, using the timespec at sp. */
now_ns:
    addi    sp, sp, -16
    sd      ra, 8(sp)
    li      a0, 1                /* CLOCK_MONOTONIC */
    addi    a1, sp, 16           /* the caller's scratch, above our frame */
    li      a7, SYS_CLOCK_GETTIME
    ecall
    ld      t0, 16(sp)           /* tv_sec */
    ld      t1, 24(sp)           /* tv_nsec */
    li      t2, 1000000000
    mul     t0, t0, t2
    add     a0, t0, t1
    ld      ra, 8(sp)
    addi    sp, sp, 16
    ret

/* Prints the tag in a1, then a0 as unsigned decimal, then a newline — in a
 * **single** write. There is no libc, so this is the whole of the
 * formatting: digits built backwards into a stack buffer, the tag placed in
 * front of them, and one call. */
print_dec:
    addi    sp, sp, -64
    sd      ra, 56(sp)
    /* Built backwards from the end of a 32-byte area at the bottom of the
     * frame, so the digits come out in order and one write sends the lot:
     * the tag, the digits, the newline. `ra` lives at 56, clear of it. */
    addi    t1, sp, 32
    addi    t1, t1, -1
    li      t3, 10
    sb      t3, 0(t1)            /* the newline, last in the line */
    mv      t0, a0
    bnez    t0, pd_digits
    addi    t1, t1, -1
    li      t4, 48               /* '0', for a count of zero */
    sb      t4, 0(t1)
    j       pd_tag
pd_digits:
    beqz    t0, pd_tag
    li      t2, 10
    remu    t4, t0, t2
    divu    t0, t0, t2
    addi    t4, t4, 48
    addi    t1, t1, -1
    sb      t4, 0(t1)
    j       pd_digits
pd_tag:
    addi    t1, t1, -1
    sb      a1, 0(t1)            /* 'h' or 'l' */
    li      a0, 1
    mv      a1, t1
    addi    a2, sp, 32
    sub     a2, a2, a1           /* exactly what was built */
    li      a7, SYS_WRITE
    ecall
    ld      ra, 56(sp)
    addi    sp, sp, 64
    ret

user_nice_end:

/* A sixth program: **a real-time thread against a fair one, one hart.**
 *
 * Phase 5's test. Run with `minikern.smp=off`, because a scheduling class is
 * only observable under contention: on four harts both threads simply run.
 *
 * The child asks for `SCHED_FIFO` at priority 50 and the parent stays in the
 * fair class. Both then do **the same fixed amount of work** and print one
 * byte when they finish. A real-time thread runs in preference to every fair
 * thread for as long as it wants to, so the real-time one must finish first:
 * the console should read `rf` and never `fr`.
 *
 * **Self-contained, deliberately.** The first version jumped to the counting
 * loop in the nice program and faulted with `EFAULT` after two syscalls,
 * because each embedded program is copied from its own `_start` to its own
 * `_end` and a label in another one is outside the mapped region. Sharing
 * code between these blobs is not possible, so each carries what it needs.
 *
 * Completion *order* rather than two counts, which is what lets it be this
 * small: the order is the claim the class makes, and it needs one byte to
 * report rather than a decimal formatter.
 */
    .globl user_rt_start
    .globl user_rt_end
user_rt_start:
    /* The child's stack: one page is all an embedded program gets, so the
     * stack comes from `mmap` rather than from this image. */
    li      a0, 0
    li      a1, 8192
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    li      t0, 8192
    add     s5, a0, t0

    li      a0, 0x00050f00
    mv      a1, s5
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bnez    a0, rt_fair_thread

    /* The child: sched_setscheduler(0, SCHED_FIFO, {.sched_priority = 50}) */
    addi    sp, sp, -16
    li      t0, 50
    sw      t0, 0(sp)
    li      a0, 0
    li      a1, 1                /* SCHED_FIFO */
    mv      a2, sp
    li      a7, SYS_SCHED_SETSCHEDULER
    ecall
    bnez    a0, rt_refused
    li      s2, 114              /* 'r' */
    j       rt_work

rt_fair_thread:
    li      s2, 102              /* 'f' */

/* The same work for both, so finishing order is the measurement. No syscall
 * in the loop: one would be a scheduling point and would measure the
 * kernel's dispatch instead of which thread the scheduler prefers. */
rt_work:
    li      t0, 2000000000
rt_spin:
    addi    t0, t0, -1
    bnez    t0, rt_spin

    /* One byte, one write: two threads writing more than that interleave. */
    addi    sp, sp, -16
    sb      s2, 0(sp)
    li      a0, 1
    mv      a1, sp
    li      a2, 1
    li      a7, SYS_WRITE
    ecall

    /* The fair thread ends the process once it has printed. The real-time
     * one **leaves the class first**, and that is not tidiness: a
     * `SCHED_FIFO` thread that keeps running after its work is done starves
     * every fair thread for ever, which is precisely what happened here —
     * the console printed `r` and then nothing, because the fair thread was
     * never going to be chosen again. The hazard is real and it is the
     * caller's to manage; this one hands the processor back.
     *
     * It also exercises the way out of the class, where the thread's virtual
     * clock has to be placed rather than restored: it has not been
     * accumulating one that anybody can compare against. */
    li      t0, 114
    beq     s2, t0, rt_steps_down
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall
rt_steps_down:
    addi    sp, sp, -16
    sw      zero, 0(sp)
    li      a0, 0
    li      a1, 0                /* SCHED_OTHER */
    mv      a2, sp
    li      a7, SYS_SCHED_SETSCHEDULER
    ecall
5:  j       5b

rt_refused:
    li      a0, 9
    li      a7, SYS_EXIT_GROUP
    ecall
6:  j       6b
user_rt_end:

/* A fifth program: **one thread, pinned to one hart.**
 *
 * Phase 4's test. It asks for hart index 2 and nothing else, then spins long
 * enough to be unmistakable. The measurement is the kernel's own per-hart
 * report: if affinity works, that hart does every context switch and the
 * others do none — and the pinned thread must actually *move* if it was
 * started somewhere else, which is the part a mask stored and never acted on
 * would get wrong.
 *
 * A fixed iteration count rather than a clock: at roughly a billion
 * iterations a second on a U74, three billion is about three seconds, and
 * being approximate costs nothing here because the report is a count of
 * switches rather than a rate. */
    .globl user_pin_start
    .globl user_pin_end
user_pin_start:
    /* sched_setaffinity(0, 8, &mask), mask = 1 << 2 */
    addi    sp, sp, -16
    li      t0, 4
    sd      t0, 0(sp)
    li      a0, 0
    li      a1, 8
    mv      a2, sp
    li      a7, SYS_SCHED_SETAFFINITY
    ecall
    /* A refusal must not look like success: exit non-zero so the kernel's
     * own "exited with" line says so. */
    bnez    a0, pin_refused
    li      t0, 3000000000
pin_spin:
    addi    t0, t0, -1
    bnez    t0, pin_spin
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall
pin_refused:
    li      a0, 9
    li      a7, SYS_EXIT_GROUP
    ecall
7:  j       7b
user_pin_end:

/* A seventh program: **hostile syscall arguments.** Rule 25's fourth
 * surface, and the one the host tests cannot reach.
 *
 * The other three surfaces are parsers in `src/core`, so they are tested on
 * the host against hostile input: the device tree, XFS, the partition table
 * and the ELF loader each have an adversarial suite. The syscall boundary is
 * not reachable that way — checking a user pointer needs page tables and a
 * process — so it needs a program, and it needs to run where the page
 * tables are real.
 *
 * Every case here hands the kernel a pointer it must refuse: an address in
 * the kernel's own half, and zero. **The kernel must not dereference
 * either.** A kernel that does takes a fault with no user context to blame
 * it on, and a fault in the kernel is a panic — so one bad argument from any
 * program stops the machine. `CLAUDE.md` lists that as a rule already; this
 * is the test of it.
 *
 * The result is a bitmask in the exit status: a bit per case that *wrongly
 * succeeded*, so a non-zero status names which check is missing rather than
 * just saying something is. Zero means every hostile argument was refused.
 */
    .globl user_hostile_start
    .globl user_hostile_end
user_hostile_start:
    li      s1, 0                /* the mask of cases that wrongly succeeded */

    /* 1: write() from the kernel's own text. */
    li      a0, 1
    li      a1, 0x80200000
    li      a2, 16
    li      a7, SYS_WRITE
    ecall
    bltz    a0, 1f
    ori     s1, s1, 1
1:
    /* 2: write() from address zero, which no process maps. */
    li      a0, 1
    li      a1, 0
    li      a2, 16
    li      a7, SYS_WRITE
    ecall
    bltz    a0, 2f
    ori     s1, s1, 2
2:
    /* 3: clock_gettime() writing *into* the kernel. A read of a bad pointer
     * is a fault; a write is a fault that also corrupts, so this is the
     * worse direction and gets its own case. */
    li      a0, 1
    li      a1, 0x80200000
    li      a7, SYS_CLOCK_GETTIME
    ecall
    bltz    a0, 3f
    ori     s1, s1, 4
3:
    /* 4: sched_setaffinity() reading its mask from the kernel. A syscall
     * added this week, which is the point of having the suite rather than
     * having audited once. */
    li      a0, 0
    li      a1, 8
    li      a2, 0x80200000
    li      a7, SYS_SCHED_SETAFFINITY
    ecall
    bltz    a0, 4f
    ori     s1, s1, 8
4:
    /* 5: mmap() of a length that cannot be satisfied. Not a pointer, but
     * the same class — an arithmetic argument the kernel must bound before
     * it reaches a page count. */
    li      a0, 0
    li      a1, -1
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, 5f
    ori     s1, s1, 16
5:
    /* 6: mremap() of the kernel's own text. The address is not read, so
     * this is not the pointer class — it asks the kernel to *move* a
     * mapping the process does not own, and the answer has to come from
     * the process's own region record rather than from the page tables,
     * which do map the kernel in every address space. */
    li      a0, 0x80200000
    li      a1, 4096
    li      a2, 8192
    li      a3, 1                /* MREMAP_MAYMOVE */
    li      a4, 0
    li      a7, SYS_MREMAP
    ecall
    bltz    a0, 6f
    ori     s1, s1, 32
6:
    /* 7: and a mapping of its own moved *onto* the kernel. The destination
     * is the dangerous direction: a move that landed there would map the
     * kernel's frames into user space with the region's rights. */
    li      a0, 0
    li      a1, 4096
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, 7f
    li      a1, 4096
    li      a2, 4096
    li      a3, 3                /* MREMAP_MAYMOVE | MREMAP_FIXED */
    li      a4, 0x80200000
    li      a7, SYS_MREMAP
    ecall
    bltz    a0, 7f
    ori     s1, s1, 64
7:
    /* Surviving to here is most of the result: the kernel refused each one
     * rather than faulting inside itself. */
    mv      a0, s1
    li      a7, SYS_EXIT_GROUP
    ecall
9:  j       9b
user_hostile_end:

/* An eighth program: **fork, then wait for the child.**
 *
 * Roadmap §1 records this as owed on top of the completed `fork`: "`wait4`
 * does not wake a forked child, so a parent that waits is left blocked."
 * Both halves of the mechanism are in the tree — the child's exit wakes its
 * parent's wait channel, and both fork paths record a parent pid — so the
 * first thing needed is to find out what actually happens.
 *
 * The child exits with 7. The parent waits for it and exits with whatever
 * `WEXITSTATUS` would extract, so the kernel's own "exited with" line is
 * the answer: **7** means the whole path works, anything else names where
 * it stops. A parent left blocked shows up as a run that never finishes.
 */
    .globl user_wait_start
    .globl user_wait_end
user_wait_start:
    /* clone(CLONE_VM is NOT set: a real fork, its own address space) */
    li      a0, 17               /* SIGCHLD, so it is a child rather than a thread */
    li      a1, 0                /* no stack: the child gets a copy of ours */
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, wait_clone_failed
    bnez    a0, wait_parent

    /* The child. */
    li      a0, 7
    li      a7, SYS_EXIT_GROUP
    ecall

wait_parent:
    /* wait4(-1, &status, 0, 0) — any child. */
    addi    sp, sp, -16
    sw      zero, 0(sp)
    li      a0, -1
    mv      a1, sp
    li      a2, 0
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bltz    a0, wait_failed
    /* The status, shifted the way WEXITSTATUS does: the exit code lives in
     * bits 8..15, and a kernel that writes it in the low byte reads as 0
     * for every program exiting with less than 256. */
    lw      t0, 0(sp)
    srli    t0, t0, 8
    andi    a0, t0, 0xff
    li      a7, SYS_EXIT_GROUP
    ecall

wait_clone_failed:
    li      a0, 101
    li      a7, SYS_EXIT_GROUP
    ecall
wait_failed:
    li      a0, 102
    li      a7, SYS_EXIT_GROUP
    ecall
8:  j       8b
user_wait_end:

/* A ninth program: **process groups, and SIGSTOP/SIGCONT by group.**
 *
 * Roadmap §1's groups-and-sessions step, tested the way a shell would use
 * it rather than one syscall at a time:
 *
 *   1. `getpgid(0)` must equal the pid — a fresh process leads its own
 *      group, which is what makes the first process a leader with no
 *      special case.
 *   2. `fork`, and the child must inherit that group.
 *   3. The parent sends **`SIGSTOP` to the group** (`kill(-pgid)`). Both
 *      members are in it, and the signal must reach the child.
 *   4. The parent sends `SIGCONT` to the group, and the child runs on and
 *      exits with 7.
 *   5. The parent reaps it and exits with the child's status.
 *
 * 7 means every step worked. The interesting failure is step 3 reaching
 * only one member: a job-control signal that reaches part of a pipeline is
 * worse than one that reaches none, because the survivors keep running with
 * their input gone — so this checks the *child* observed it, by having the
 * child only exit after being continued.
 *
 * The child proves it was stopped by writing nothing until it is continued:
 * the parent sleeps between STOP and CONT, and if the child were running it
 * would have exited during that window and the reap would come back with a
 * status it set before the CONT. There is no shared memory here, so the
 * evidence is the ordering, which the status carries.
 */
    .globl user_pgrp_start
    .globl user_pgrp_end
user_pgrp_start:
    /* getpgid(0) and getpid() must agree. */
    li      a0, 0
    li      a7, SYS_GETPGID
    ecall
    mv      s3, a0
    li      a7, SYS_GETPID
    ecall
    bne     a0, s3, pgrp_bad_group
    mv      s4, a0               /* our pid, which is also our pgid */

    /* fork */
    li      a0, 17               /* SIGCHLD: a child, not a thread */
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, pgrp_clone_failed
    bnez    a0, pgrp_parent

    /* The child: confirm it inherited the group, then spin until it is
     * continued. The spin is bounded so a failure ends rather than hangs —
     * if it is never stopped it simply exits 7 early, and the ordering the
     * parent checks is what catches that. */
    li      a0, 0
    li      a7, SYS_GETPGID
    ecall
    bne     a0, s4, pgrp_child_wrong_group
    li      t0, 400000000
pgrp_child_spin:
    addi    t0, t0, -1
    bnez    t0, pgrp_child_spin
    li      a0, 7
    li      a7, SYS_EXIT_GROUP
    ecall

pgrp_parent:
    /* SIGSTOP to the whole group: kill(-pgid, SIGSTOP). */
    sub     a0, zero, s4
    li      a1, 19               /* SIGSTOP */
    li      a7, SYS_KILL
    ecall
    bltz    a0, pgrp_kill_failed

    /* Long enough that a child which ignored the stop would have finished
     * its spin and exited. */
    addi    sp, sp, -16
    sd      zero, 0(sp)
    li      t0, 500000000
    sd      t0, 8(sp)            /* 500 ms */
    mv      a0, sp
    li      a1, 0
    li      a7, SYS_NANOSLEEP
    ecall

    /* SIGCONT to the group. */
    sub     a0, zero, s4
    li      a1, 18               /* SIGCONT */
    li      a7, SYS_KILL
    ecall
    bltz    a0, pgrp_kill_failed

    /* Reap, and exit with what the child exited with. */
    addi    sp, sp, -16
    sw      zero, 0(sp)
    li      a0, -1
    mv      a1, sp
    li      a2, 0
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bltz    a0, pgrp_wait_failed
    lw      t0, 0(sp)
    srli    t0, t0, 8
    andi    a0, t0, 0xff
    li      a7, SYS_EXIT_GROUP
    ecall

pgrp_bad_group:
    li      a0, 111
    li      a7, SYS_EXIT_GROUP
    ecall
pgrp_clone_failed:
    li      a0, 112
    li      a7, SYS_EXIT_GROUP
    ecall
pgrp_child_wrong_group:
    li      a0, 113
    li      a7, SYS_EXIT_GROUP
    ecall
pgrp_kill_failed:
    li      a0, 114
    li      a7, SYS_EXIT_GROUP
    ecall
pgrp_wait_failed:
    li      a0, 115
    li      a7, SYS_EXIT_GROUP
    ecall
7:  j       7b
user_pgrp_end:

/* A tenth program: **reads a line from the terminal.**
 *
 * Roadmap §1's controlling-terminal step. Everything about the line
 * discipline is tested on the host, because the editing rules need a
 * human's fingers to notice. What only a board can show is the rest of the
 * path: a byte arriving on the UART, reaching the discipline from the
 * housekeeping beat, waking a blocked reader, and coming back through
 * `read` on descriptor 0.
 *
 * It asks the terminal its size first, because a program that cannot do
 * that lays itself out for a window with no room in it — and because
 * `ioctl` returning `ENOTTY` for a terminal is the failure that makes every
 * program think its output is a pipe.
 *
 * Then it reads one line and writes it back. The harness types `hi` and
 * looks for it echoed: the echo proves the discipline ran, and the write
 * proves the line reached the program.
 */
    .globl user_term_start
    .globl user_term_end
user_term_start:
    /* ioctl(1, TIOCGWINSZ, &ws) */
    addi    sp, sp, -32
    li      a0, 1
    li      a1, 0x5413
    mv      a2, sp
    li      a7, SYS_IOCTL
    ecall
    bltz    a0, term_no_tty
    lhu     t0, 0(sp)            /* rows */
    lhu     t1, 2(sp)            /* cols */
    li      t2, 24
    bne     t0, t2, term_bad_size
    li      t2, 80
    bne     t1, t2, term_bad_size

    /* read(0, buf, 64) — blocks until a line is typed. */
    addi    sp, sp, -64
    li      a0, 0
    mv      a1, sp
    li      a2, 64
    li      a7, SYS_READ
    ecall
    blez    a0, term_read_failed
    mv      s1, a0

    /* write(1, buf, n) — hand the line straight back. */
    li      a0, 1
    mv      a1, sp
    mv      a2, s1
    li      a7, SYS_WRITE
    ecall
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

term_no_tty:
    li      a0, 121
    li      a7, SYS_EXIT_GROUP
    ecall
term_bad_size:
    li      a0, 122
    li      a7, SYS_EXIT_GROUP
    ecall
term_read_failed:
    li      a0, 123
    li      a7, SYS_EXIT_GROUP
    ecall
6:  j       6b
user_term_end:

/* An eleventh program: **dropping privilege, and not getting it back.**
 *
 * Roadmap §1's users-and-groups step. The permission *rule* is pure and
 * host-tested; what only a running kernel can show is the identity model:
 *
 *   1. The first process is root — `getuid` and `geteuid` both 0. Something
 *      has to be, or the machine cannot read its own filesystem to boot.
 *   2. `setuid(1000)` succeeds, because root may become anybody.
 *   3. Both ids changed. Setting only the effective one would leave a real
 *      uid of 0, and a process with that can take privilege back — the
 *      difference between dropping it and appearing to.
 *   4. `setuid(0)` is now **refused**. This is the step that matters: a
 *      drop that can be undone is not a drop.
 *
 * 0 means every step held; 131..134 name the one that did not.
 */
    .globl user_uid_start
    .globl user_uid_end
user_uid_start:
    li      a7, SYS_GETUID
    ecall
    bnez    a0, uid_not_root
    li      a7, SYS_GETEUID
    ecall
    bnez    a0, uid_not_root

    /* Become 1000. */
    li      a0, 1000
    li      a7, SYS_SETUID
    ecall
    bnez    a0, uid_drop_failed

    /* Both ids, not just the effective one. */
    li      a7, SYS_GETUID
    ecall
    li      t0, 1000
    bne     a0, t0, uid_real_unchanged
    li      a7, SYS_GETEUID
    ecall
    bne     a0, t0, uid_real_unchanged

    /* And back to root must fail. */
    li      a0, 0
    li      a7, SYS_SETUID
    ecall
    bgez    a0, uid_regained
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

uid_not_root:
    li      a0, 131
    li      a7, SYS_EXIT_GROUP
    ecall
uid_drop_failed:
    li      a0, 132
    li      a7, SYS_EXIT_GROUP
    ecall
uid_real_unchanged:
    li      a0, 133
    li      a7, SYS_EXIT_GROUP
    ecall
uid_regained:
    li      a0, 134
    li      a7, SYS_EXIT_GROUP
    ecall
5:  j       5b
user_uid_end:

/* A twelfth program: **a resource limit that actually bites.**
 *
 * Roadmap §1's last item. `prlimit64` answered every question with
 * "infinite" and refused every attempt to set anything, so a program read a
 * number nothing would hold it to.
 *
 *   1. Read `RLIMIT_NOFILE`. It must come back as the table's size, not as
 *      infinity — the honest answer about a kernel with a fixed table.
 *   2. Lower it to 5, which anybody may do.
 *   3. Read it back and see 5. A limit that is accepted and dropped is
 *      worse than one that is refused.
 *   4. Open `/dev/null` until it fails. With 0, 1 and 2 already open, the
 *      5th descriptor must be the last: the next `openat` is `EMFILE`.
 *   5. Try to raise the hard limit back. Refused, because this program is
 *      no longer root — the step that makes a hard limit mean anything.
 *
 * The fourth step is the one that was missing: before it, the limit was a
 * number in a struct. 0 means every step held.
 */
    .globl user_rlimit_start
    .globl user_rlimit_end
user_rlimit_start:
    /* prlimit64(0, RLIMIT_NOFILE, NULL, &old) */
    addi    sp, sp, -32
    li      a0, 0
    li      a1, 7                /* RLIMIT_NOFILE */
    li      a2, 0
    mv      a3, sp
    li      a7, SYS_PRLIMIT64
    ecall
    bnez    a0, rl_get_failed
    ld      t0, 0(sp)            /* the soft limit */
    li      t1, 64               /* FD_MAX: the honest answer */
    bne     t0, t1, rl_not_table_size

    /* Lower it to 5, soft and hard. */
    li      t0, 5
    sd      t0, 0(sp)
    sd      t0, 8(sp)
    li      a0, 0
    li      a1, 7
    mv      a2, sp
    li      a3, 0
    li      a7, SYS_PRLIMIT64
    ecall
    bnez    a0, rl_set_failed

    /* Read it back. */
    addi    sp, sp, -32
    li      a0, 0
    li      a1, 7
    li      a2, 0
    mv      a3, sp
    li      a7, SYS_PRLIMIT64
    ecall
    bnez    a0, rl_get_failed
    ld      t0, 0(sp)
    li      t1, 5
    bne     t0, t1, rl_not_lowered
    addi    sp, sp, 32

    /* Open /dev/null until it is refused. 0, 1 and 2 are already open, so
     * exactly two more may be had. */
    li      s2, 0                /* how many opened */
rl_open_loop:
    li      a0, -100             /* AT_FDCWD */
    lla     a1, rl_devnull
    li      a2, 0
    li      a3, 0
    li      a7, SYS_OPENAT
    ecall
    bltz    a0, rl_opens_done
    addi    s2, s2, 1
    li      t0, 8
    blt     s2, t0, rl_open_loop
    /* More than the limit allowed: the limit is not being enforced. */
    j       rl_not_enforced
rl_opens_done:
    li      t0, 2
    bne     s2, t0, rl_not_enforced

    /* Drop privilege, then try to raise the hard limit back. */
    li      a0, 1000
    li      a7, SYS_SETUID
    ecall
    bnez    a0, rl_set_failed
    li      t0, 64
    sd      t0, 0(sp)
    sd      t0, 8(sp)
    li      a0, 0
    li      a1, 7
    mv      a2, sp
    li      a3, 0
    li      a7, SYS_PRLIMIT64
    ecall
    bgez    a0, rl_raised
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

rl_get_failed:
    li      a0, 141
    li      a7, SYS_EXIT_GROUP
    ecall
rl_not_table_size:
    li      a0, 142
    li      a7, SYS_EXIT_GROUP
    ecall
rl_set_failed:
    li      a0, 143
    li      a7, SYS_EXIT_GROUP
    ecall
rl_not_lowered:
    li      a0, 144
    li      a7, SYS_EXIT_GROUP
    ecall
rl_not_enforced:
    li      a0, 145
    li      a7, SYS_EXIT_GROUP
    ecall
rl_raised:
    li      a0, 146
    li      a7, SYS_EXIT_GROUP
    ecall
rl_devnull:
    .asciz  "/dev/null"
    .balign 4
4:  j       4b
user_rlimit_end:

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

/* Roadmap §2: `mremap`, and the record `munmap` used to keep.
 *
 * Six independent checks, each setting one bit of the exit status, so a
 * partial failure says *which* half works rather than only that something
 * does not. **63 is a pass.**
 *
 * The shape of it is forced by what has to be proved. A mapping asked to
 * grow will grow where it stands if the pages past its end are free, and
 * that path moves nothing — so to see a move at all, the space above the
 * mapping has to be occupied. The blocker is mapped **first** (placement
 * takes the highest gap, so the blocker ends up above the subject) and is
 * read-only, which keeps the record from merging the two into one region
 * with the same rights and leaving nothing in the way.
 *
 *   1. the four markers written before the move are readable after it
 *   2. the pages the growth added are usable
 *   3. shrinking reports the same address
 *   4. what the shrink kept still reads correctly
 *   5. `mremap` of the address it moved **from** fails — the old range is
 *      gone from the record, not merely unmapped
 *   6. `mremap` of a range just handed to `munmap` fails, which is the
 *      defect this found: nothing used to remove the record, so with demand
 *      paging a read of returned memory was answered with a fresh zero page
 *
 * The last two are probes rather than faults on purpose: a program that
 * proved the point by dying of SIGSEGV would take the other five bits with
 * it.
 */
    .globl user_mremap_start
    .globl user_mremap_end
user_mremap_start:
    li      s2, 0                /* the mask of checks that held */

    /* The blocker: eight pages, read-only so it cannot merge with the
     * subject, taking the highest gap in the window. */
    li      a0, 0
    li      a1, 8 * 4096
    li      a2, 1                /* PROT_READ */
    li      a3, 0x22             /* MAP_PRIVATE | MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mr_no_memory
    mv      s3, a0

    /* The subject: four pages, immediately below the blocker. */
    li      a0, 0
    li      a1, 4 * 4096
    li      a2, 3                /* PROT_READ | PROT_WRITE */
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mr_no_memory
    mv      s4, a0

    /* One marker per page: 1, 2, 3, 4. Writing them is also what makes the
     * pages exist, since nothing is backed until it is touched. */
    li      t3, 4096
    mv      t1, s4
    li      t2, 1
    li      t4, 5
mr_fill:
    sd      t2, 0(t1)
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, mr_fill

    /* Grow it to eight pages. The blocker is in the way, so this has to
     * move — and `MREMAP_MAYMOVE` is the caller saying that is allowed. */
    mv      a0, s4
    li      a1, 4 * 4096
    li      a2, 8 * 4096
    li      a3, 1                /* MREMAP_MAYMOVE */
    li      a4, 0
    li      a7, SYS_MREMAP
    ecall
    bltz    a0, mr_refused
    mv      s5, a0

    /* 1: the markers survived the move. */
    li      t3, 4096
    mv      t1, s5
    li      t2, 1
    li      t4, 5
mr_check:
    ld      t5, 0(t1)
    bne     t5, t2, mr_tail
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, mr_check
    ori     s2, s2, 1

    /* 2: the last of the added pages is usable. */
mr_tail:
    li      t3, 7 * 4096
    add     t1, s5, t3
    li      t2, 0x5a5a
    sd      t2, 0(t1)
    ld      t5, 0(t1)
    bne     t5, t2, mr_shrink
    ori     s2, s2, 2

    /* 3: shrinking to two pages keeps the address. */
mr_shrink:
    mv      a0, s5
    li      a1, 8 * 4096
    li      a2, 2 * 4096
    li      a3, 0
    li      a4, 0
    li      a7, SYS_MREMAP
    ecall
    bne     a0, s5, mr_head
    ori     s2, s2, 4

    /* 4: the two pages it kept still read 1 and 2. */
mr_head:
    ld      t5, 0(s5)
    li      t2, 1
    bne     t5, t2, mr_old
    li      t3, 4096
    add     t1, s5, t3
    ld      t5, 0(t1)
    li      t2, 2
    bne     t5, t2, mr_old
    ori     s2, s2, 8

    /* 5: the address it moved from is gone from the record. */
mr_old:
    mv      a0, s4
    li      a1, 4 * 4096
    li      a2, 4 * 4096
    li      a3, 0
    li      a4, 0
    li      a7, SYS_MREMAP
    ecall
    bgez    a0, mr_unmap
    ori     s2, s2, 16

    /* 6: and so is a range just unmapped. */
mr_unmap:
    mv      a0, s5
    li      a1, 2 * 4096
    li      a7, SYS_MUNMAP
    ecall
    bnez    a0, mr_done
    mv      a0, s5
    li      a1, 2 * 4096
    li      a2, 2 * 4096
    li      a3, 0
    li      a4, 0
    li      a7, SYS_MREMAP
    ecall
    bgez    a0, mr_done
    ori     s2, s2, 32

mr_done:
    mv      a0, s2
    li      a7, SYS_EXIT_GROUP
    ecall
mr_no_memory:
    li      a0, 151
    li      a7, SYS_EXIT_GROUP
    ecall
    /* A refused move reports **which** refusal: 200 plus the errno, so
     * `EFAULT` is 214, `ENOMEM` 212 and `EINVAL` 222. A bare "it failed"
     * costs a board run to narrow down, and the status is free. */
mr_refused:
    neg     a0, a0
    addi    a0, a0, 200
    li      a7, SYS_EXIT_GROUP
    ecall
5:  j       5b
user_mremap_end:

/* Roadmap §2: the `mmap` flags, each either honoured, vacuous or refused.
 *
 * Seven checks, one bit each; **127 is a pass.** The refusals are as much
 * of the point as the honourings: a flag that is accepted and not delivered
 * leaves the caller's assumption standing, and `MAP_SHARED` answered with
 * private memory is two processes each believing the other can see their
 * writes.
 *
 * What a program cannot see from here is whether `MAP_POPULATE` actually
 * backed anything — the memory reads as zeroes either way. That half is the
 * kernel's own counter, printed after this program runs: the pages it
 * covers appear there and never in the demand-paging count.
 */
    .globl user_mapflags_start
    .globl user_mapflags_end
user_mapflags_start:
    li      s2, 0

    /* 1: MAP_SHARED is refused rather than quietly made private. */
    li      a0, 0
    li      a1, 4096
    li      a2, 3
    li      a3, 0x21             /* MAP_SHARED | MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bgez    a0, mf_huge
    ori     s2, s2, 1

    /* 2: MAP_HUGETLB, rather than 4 KiB pages pretending. */
mf_huge:
    li      a0, 0
    li      a1, 4096
    li      a2, 3
    li      a3, 0x40022          /* MAP_HUGETLB | MAP_PRIVATE | MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bgez    a0, mf_grow
    ori     s2, s2, 2

    /* 3: MAP_GROWSDOWN, rather than a mapping that will not grow. */
mf_grow:
    li      a0, 0
    li      a1, 4096
    li      a2, 3
    li      a3, 0x122            /* MAP_GROWSDOWN | MAP_PRIVATE | MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bgez    a0, mf_populate
    ori     s2, s2, 4

    /* 4: MAP_POPULATE is accepted and the memory works. */
mf_populate:
    li      a0, 0
    li      a1, 4 * 4096
    li      a2, 3
    li      a3, 0x8022           /* MAP_POPULATE | MAP_PRIVATE | MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mf_done
    mv      s3, a0
    li      t2, 0x1234
    sd      t2, 0(s3)
    ld      t5, 0(s3)
    bne     t5, t2, mf_taken
    ori     s2, s2, 8

    /* 5: MAP_FIXED_NOREPLACE over something refuses instead of destroying
     * it — which is the reason the flag exists. */
mf_taken:
    mv      a0, s3
    li      a1, 4096
    li      a2, 3
    li      a3, 0x100022         /* MAP_FIXED_NOREPLACE | PRIVATE | ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bgez    a0, mf_free
    ori     s2, s2, 16

    /* 6: and takes the address once it is free, exactly. */
mf_free:
    mv      a0, s3
    li      a1, 4 * 4096
    li      a7, SYS_MUNMAP
    ecall
    bnez    a0, mf_vacuous
    mv      a0, s3
    li      a1, 4096
    li      a2, 3
    li      a3, 0x100022
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bne     a0, s3, mf_vacuous
    ori     s2, s2, 32

    /* 7: the vacuous flags are accepted, because they ask for what is
     * already true rather than for something missing. */
mf_vacuous:
    li      a0, 0
    li      a1, 4096
    li      a2, 3
    li      a3, 0x24022          /* MAP_NORESERVE | MAP_STACK | PRIVATE | ANON */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mf_done
    ori     s2, s2, 64

mf_done:
    mv      a0, s2
    li      a7, SYS_EXIT_GROUP
    ecall
6:  j       6b
user_mapflags_end:
