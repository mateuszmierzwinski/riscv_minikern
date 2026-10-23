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
