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
#define SYS_EXECVE 221
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
#define SYS_SETPGID 154
#define SYS_GETPID 172
#define SYS_IOCTL 29
#define SYS_READ 63
#define SYS_CLOSE 57
#define SYS_GETUID 174
#define SYS_GETEUID 175
#define SYS_SETUID 146
#define SYS_PRLIMIT64 261
#define SYS_OPENAT 56
#define SYS_MUNMAP 215
#define SYS_MREMAP 216
#define SYS_MADVISE 233

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
    /* 8: the calls added since — `getresuid` writing its three ids into
     * the kernel, and `clock_nanosleep` reading its request from it, at
     * the address the kernel now actually runs at (its top-half text).
     * They share bit 128: the status is eight bits wide. */
    li      a0, 0xffffffc000200000
    li      a1, 0xffffffc000200000
    li      a2, 0xffffffc000200000
    li      a7, 148              /* getresuid */
    ecall
    bgez    a0, 81f
    li      a0, 1                /* CLOCK_MONOTONIC */
    li      a1, 0
    li      a2, 0xffffffc000200000
    li      a3, 0
    li      a7, 115              /* clock_nanosleep */
    ecall
    bgez    a0, 81f
    j       8f
81: ori     s1, s1, 128
8:
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
    /* A fresh process leads its own group: `getpgid(0)` and `getpid()` agree.
     *
     * This is also the check that caught `getpid` returning the constant 1 —
     * it only ever ran as the first program in a boot, where the real pid
     * *is* 1, so two wrong answers agreed. */
    li      a0, 0
    li      a7, SYS_GETPGID
    ecall
    mv      s3, a0
    li      a7, SYS_GETPID
    ecall
    bne     a0, s3, pgrp_bad_group

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
    mv      s4, a0

    /* The child: spin briefly and exit 7. The spin is short — the parent
     * waits far longer than it takes — so a child that was never stopped
     * finishes during the parent's sleep, and the parent's `WNOHANG` probe
     * sees it finished. That probe is the proof: it distinguishes "stopped"
     * from "ran through", which the exit status alone cannot. */
    li      t0, 20000000
pgrp_child_spin:
    addi    t0, t0, -1
    bnez    t0, pgrp_child_spin
    li      a0, 7
    li      a7, SYS_EXIT_GROUP
    ecall

pgrp_parent:
    mv      s4, a0               /* the child's pid */
    /* **The child into a group of its own**, which is what a shell does to
     * a job — and the reason this test has to: stopping *our* group would
     * stop this process too, since it is in it. The old version did exactly
     * that and passed only because `SIGSTOP` could not actually stop a
     * thread that was running; once it could, the test deadlocked itself.
     * The parent sets it rather than the child, so there is no window in
     * which the signal could arrive before the move. */
    mv      a0, s4
    mv      a1, s4
    li      a7, SYS_SETPGID
    ecall
    bltz    a0, pgrp_kill_failed

    /* SIGSTOP to that group, by negative pid. */
    sub     a0, zero, s4
    li      a1, 19               /* SIGSTOP */
    li      a7, SYS_KILL
    ecall
    bltz    a0, pgrp_kill_failed

    /* Long enough that a child which ignored the stop would have finished. */
    addi    sp, sp, -16
    sd      zero, 0(sp)
    li      t0, 500000000
    sd      t0, 8(sp)            /* 500 ms */
    mv      a0, sp
    li      a1, 0
    li      a7, SYS_NANOSLEEP
    ecall

    /* **The proof**: the child has not finished, because it is stopped. */
    addi    sp, sp, -16
    sw      zero, 0(sp)
    mv      a0, s4
    mv      a1, sp
    li      a2, 1                /* WNOHANG */
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bnez    a0, pgrp_not_stopped

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
pgrp_not_stopped:
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
 * writes. The second check used to be `MAP_HUGETLB` being refused for
 * exactly that reason, and is now the same flag being honoured.
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

    /* 2: MAP_HUGETLB is **accepted** — it was refused here until there were
     * 2 MiB pages to give, which is the right place for a flag to wait, and
     * this check moved from "is it refused?" to "is it honoured?" with the
     * same number. A 4 MiB length, because a flag asking for 2 MiB pages
     * with 4 KiB of length asks for nothing. What it actually gets is the
     * huge test's business (`minikern.huge=1`); here it is one of the three
     * answers a flag can have. */
mf_huge:
    li      a0, 0
    li      a1, 4
    slli    a1, a1, 20
    li      a2, 3
    li      a3, 0x40022          /* MAP_HUGETLB | MAP_PRIVATE | MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mf_grow
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

/* Roadmap §0: a thread that will not leave its hart on its own.
 *
 * The defect was that `sched_end_siblings` marked a process's other threads
 * and the caller carried straight on to replace or free the address space —
 * so a sibling still executing user code on another hart ran against page
 * tables being taken apart underneath it. It "had not been seen to fail",
 * which says nothing about whether it does.
 *
 * This program builds the case deliberately, which is the only way to see
 * it: a child process with a **spinning thread that makes no syscalls**, so
 * under dynamic ticks there is nothing scheduled to interrupt it, and then
 * the child's main thread calls `exit_group`. The kernel has to get that
 * thread off its hart before it frees the address space, and its own
 * `waited for` line says whether it had to.
 *
 * The parent reaps the child and exits with its status: **5** means the
 * whole sequence worked. A hang would be the wait never being satisfied,
 * and the kernel's line would then name the threads that would not leave.
 */
    .globl user_endsib_start
    .globl user_endsib_end
user_endsib_start:
    /* fork: a child with an address space of its own, which is what makes
     * its exit free one rather than stop the machine. */
    li      a0, 17               /* SIGCHLD */
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, es_failed
    bnez    a0, es_parent

    /* The child. A stack for the thread it is about to make, out of mapped
     * memory: an embedded program is one page. */
    li      a0, 0
    li      a1, 8192
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, es_child_failed
    li      t0, 8192
    add     s5, a0, t0

    /* clone(CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD) — a
     * thread of the child, sharing its address space. */
    li      a0, 0x00050f00
    mv      a1, s5
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, es_child_failed
    bnez    a0, es_child_main

    /* The thread: spin for ever, with **no syscall of any kind**. Nothing
     * here will ever trap voluntarily, which is the whole point — only an
     * interrupt aimed at this hart can stop it. */
es_spin:
    j       es_spin

es_child_main:
    /* Long enough that the thread is certainly on a hart of its own, and
     * past any slice it might have been given. */
    addi    sp, sp, -16
    sd      zero, 0(sp)
    li      t0, 50000000         /* 50 ms */
    sd      t0, 8(sp)
    mv      a0, sp
    li      a1, 0
    li      a7, SYS_NANOSLEEP
    ecall
    /* The whole process, while a thread of it is mid-instruction elsewhere. */
    li      a0, 5
    li      a7, SYS_EXIT_GROUP
    ecall

es_parent:
    addi    sp, sp, -16
    sw      zero, 0(sp)
    li      a0, -1
    mv      a1, sp
    li      a2, 0
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bltz    a0, es_failed
    lw      t0, 0(sp)
    srli    t0, t0, 8
    andi    a0, t0, 0xff
    li      a7, SYS_EXIT_GROUP
    ecall

es_failed:
    li      a0, 161
    li      a7, SYS_EXIT_GROUP
    ecall
es_child_failed:
    li      a0, 162
    li      a7, SYS_EXIT_GROUP
    ecall
7:  j       7b
user_endsib_end:

/* Roadmap §2: huge pages, asked for with `MAP_HUGETLB`.
 *
 * Seven checks, one bit each. What a program can see is
 * only that the memory works and that the mapping is 2 MiB aligned — it
 * cannot see how many table entries are behind it, which is the whole
 * reason to ask. That half is the kernel's own counter, printed after this
 * runs: `uspace_huge_pages` is the number of 2 MiB entries handed out, and
 * a kernel quietly using 4 KiB pages would look identical here.
 *
 *   1. the mapping is accepted
 *   2. it is 2 MiB aligned, which it has to be to be mapped as one entry
 *   3. the first byte of each of its two blocks is writable and reads back
 *   4. so is the last byte of the second block, 4 MiB in
 *   5. a partial `munmap` of it is **refused**, because a 2 MiB page cannot
 *      be unmapped in quarters and splitting it silently would make the
 *      flag a hint again
 *   6. a forked child sees the markers
 *   7. and the child's write does not reach the parent's copy, which is
 *      what distinguishes a 2 MiB page *copied* into the child from the
 *      same entry shared with it
 *
 * **127 is a pass**, not 31.
 */
    .globl user_huge_start
    .globl user_huge_end
user_huge_start:
    li      s2, 0

    /* mmap(NULL, 4 MiB, RW, MAP_HUGETLB|MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
    li      a0, 0
    li      a1, 4
    slli    a1, a1, 20           /* 4 MiB */
    li      a2, 3
    li      a3, 0x40022
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, hp_done
    mv      s3, a0
    ori     s2, s2, 1

    /* Aligned to 2 MiB: anything else could not be a 2 MiB page. */
    li      t0, 2
    slli    t0, t0, 20
    addi    t0, t0, -1
    and     t1, s3, t0
    bnez    t1, hp_blocks
    ori     s2, s2, 2

    /* One marker in each block. */
hp_blocks:
    li      t0, 0x1111
    sd      t0, 0(s3)
    li      t2, 2
    slli    t2, t2, 20
    add     t3, s3, t2           /* the second block */
    li      t0, 0x2222
    sd      t0, 0(t3)
    ld      t4, 0(s3)
    li      t0, 0x1111
    bne     t4, t0, hp_tail
    ld      t4, 0(t3)
    li      t0, 0x2222
    bne     t4, t0, hp_tail
    ori     s2, s2, 4

    /* The last word of the whole mapping, 4 MiB in. */
hp_tail:
    li      t2, 4
    slli    t2, t2, 20
    add     t3, s3, t2
    addi    t3, t3, -8
    li      t0, 0x3333
    sd      t0, 0(t3)
    ld      t4, 0(t3)
    bne     t4, t0, hp_partial
    ori     s2, s2, 8

    /* A partial unmap must be refused. */
hp_partial:
    mv      a0, s3
    li      a1, 4096
    li      a7, SYS_MUNMAP
    ecall
    bgez    a0, hp_fork
    ori     s2, s2, 16

    /* 6 and 7: **fork**, which is where a 2 MiB page is most likely to be
     * got wrong. The child must see the markers and must have its *own*
     * copy: a megapage given by sharing the one table entry would let the
     * child's write change the parent's memory, which is the bug this pair
     * of checks exists to catch. */
hp_fork:
    li      a0, 17               /* SIGCHLD */
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, hp_done
    bnez    a0, hp_parent

    /* The child: both markers, then overwrite the first. */
    ld      t4, 0(s3)
    li      t0, 0x1111
    bne     t4, t0, hp_child_bad
    li      t2, 2
    slli    t2, t2, 20
    add     t3, s3, t2
    ld      t4, 0(t3)
    li      t0, 0x2222
    bne     t4, t0, hp_child_bad
    li      t0, 0x9999
    sd      t0, 0(s3)
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall
hp_child_bad:
    li      a0, 1
    li      a7, SYS_EXIT_GROUP
    ecall

hp_parent:
    addi    sp, sp, -16
    sw      zero, 0(sp)
    li      a0, -1
    mv      a1, sp
    li      a2, 0
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bltz    a0, hp_done
    lw      t0, 0(sp)
    srli    t0, t0, 8
    andi    t0, t0, 0xff
    bnez    t0, hp_mine
    ori     s2, s2, 32
    /* And the parent's own copy is untouched. */
hp_mine:
    ld      t4, 0(s3)
    li      t0, 0x1111
    bne     t4, t0, hp_done
    ori     s2, s2, 64

hp_done:
    mv      a0, s2
    li      a7, SYS_EXIT_GROUP
    ecall
8:  j       8b
user_huge_end:

/* Roadmap §2: a mapping backed by a file.
 *
 * Six checks, one bit each; **63 is a pass.** It maps `/init` — the only
 * file this kernel is sure to find, and one whose first four bytes are
 * known: an ELF magic number, which is a better marker than anything this
 * program could write because it is *the file's* and not its own.
 *
 *   1. `openat` of a regular file succeeds, which it could not before
 *   2. `read` of it gives the ELF magic
 *   3. the mapping gives the same four bytes
 *   4. a page **not yet touched** gives the file's bytes after the
 *      descriptor is closed — the word at offset 4096, read through the
 *      descriptor beforehand and compared against the mapping afterwards.
 *      Re-reading the *first* page would prove nothing: it was faulted in
 *      before the close, so a kernel that had freed the file's bytes would
 *      still answer correctly. This is the check that the mapping's own
 *      reference to the file is real
 *   5. an unaligned offset is refused rather than rounded
 *   6. a mapping reaching past the file's last page is refused rather than
 *      being handed invented zeroes
 */
    .globl user_filemap_start
    .globl user_filemap_end
user_filemap_start:
    li      s2, 0
    addi    sp, sp, -32

    /* openat(AT_FDCWD, "/init", O_RDONLY) */
    li      a0, -100
    lla     a1, fm_path
    li      a2, 0
    li      a3, 0
    li      a7, SYS_OPENAT
    ecall
    bltz    a0, fm_done
    mv      s3, a0
    ori     s2, s2, 1

    /* read(fd, sp, 4) must give the ELF magic. */
    mv      a0, s3
    mv      a1, sp
    li      a2, 4
    li      a7, SYS_READ
    ecall
    li      t0, 4
    bne     a0, t0, fm_map
    lw      t1, 0(sp)
    lla     t2, fm_magic
    lw      t3, 0(t2)
    bne     t1, t3, fm_map
    ori     s2, s2, 2

    /* mmap(NULL, 8192, PROT_READ, MAP_PRIVATE, fd, 0) */
fm_map:
    li      a0, 0
    li      a1, 8192
    li      a2, 1                /* PROT_READ */
    li      a3, 0x02             /* MAP_PRIVATE, and no MAP_ANONYMOUS */
    mv      a4, s3
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, fm_unaligned
    mv      s4, a0
    lw      t1, 0(s4)
    lla     t2, fm_magic
    lw      t3, 0(t2)
    bne     t1, t3, fm_closed
    ori     s2, s2, 4

    /* **The page that proves it.** Closing the descriptor and re-reading
     * the *first* page proves nothing: that page was faulted in before the
     * close, so its bytes were already copied and a kernel that had freed
     * the file would still answer correctly. So the word at offset 4096 is
     * read through the descriptor first, kept in a register, and then
     * compared against the same offset reached **through the mapping after
     * the close** — a page that has to be faulted in from a file the
     * program can no longer name. If the mapping did not hold its own
     * reference, that fault reads freed memory.
     *
     * Getting there needs the descriptor's offset at exactly 4096: four
     * bytes have been read already, so 4092 more. */
fm_closed:
    li      t0, 4096             /* past `addi`'s reach, so through a register */
    sub     sp, sp, t0
    mv      a0, s3
    mv      a1, sp
    li      a2, 4092
    li      a7, SYS_READ
    ecall
    li      t0, 4092
    bne     a0, t0, fm_unaligned
    mv      a0, s3
    mv      a1, sp
    li      a2, 4
    li      a7, SYS_READ
    ecall
    li      t0, 4
    bne     a0, t0, fm_unaligned
    lw      s5, 0(sp)            /* the file's word at offset 4096 */
    li      t0, 4096
    add     sp, sp, t0

    mv      a0, s3
    li      a7, SYS_CLOSE
    ecall
    li      t0, 4096
    add     t1, s4, t0
    lw      t2, 0(t1)
    bne     t2, s5, fm_unaligned
    ori     s2, s2, 8

    /* An unaligned offset is refused. The descriptor is closed, so this
     * also has to be refused for that reason — it is reopened first. */
fm_unaligned:
    li      a0, -100
    lla     a1, fm_path
    li      a2, 0
    li      a3, 0
    li      a7, SYS_OPENAT
    ecall
    bltz    a0, fm_done
    mv      s3, a0
    li      a0, 0
    li      a1, 4096
    li      a2, 1
    li      a3, 0x02
    mv      a4, s3
    li      a5, 100              /* not a multiple of the page size */
    li      a7, SYS_MMAP
    ecall
    bgez    a0, fm_toolong
    ori     s2, s2, 16

    /* A mapping past the end of the file is refused. */
fm_toolong:
    li      a0, 0
    li      a1, 64
    slli    a1, a1, 20           /* 64 MiB, far past /init */
    li      a2, 1
    li      a3, 0x02
    mv      a4, s3
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bgez    a0, fm_done
    ori     s2, s2, 32

fm_done:
    mv      a0, s2
    li      a7, SYS_EXIT_GROUP
    ecall
fm_path:
    .asciz  "/init"
    .balign 4
fm_magic:
    .byte   0x7f
    .ascii  "ELF"
    .balign 4
9:  j       9b
user_filemap_end:

/* Roadmap §1: `^Z` from the keyboard to a job, which is the last thing in
 * that section.
 *
 * Everything on this path existed and nothing had ever walked it: the
 * terminal raises `SIGTSTP` on the **foreground group**, `TIOCSPGRP` sets
 * which group that is, and `SIGSTOP`/`SIGCONT` act on a process rather than
 * a handler. What was missing was a program willing to be the shell for
 * long enough to prove it.
 *
 * So this one is: it puts a child in a group of its own, makes that group
 * the terminal's foreground, prints a marker and waits. The harness types
 * `^Z` when it sees the marker (`runtype.py`), and the kernel has to turn
 * that keystroke into a signal that stops **the child and not this
 * process** — which is why the parent stays in its own group, and why a
 * kernel that raised the signal at the wrong group would hang here instead
 * of passing.
 *
 * The proof is the same shape as the process-group test's: after a wait far
 * longer than the child's own work, the child must **not** have finished.
 * Then `SIGCONT`, and it does. **7 is a pass**; 171..175 name the step.
 */
    .globl user_tstp_start
    .globl user_tstp_end
user_tstp_start:
    /* fork */
    li      a0, 17               /* SIGCHLD */
    li      a1, 0
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    bltz    a0, ts_clone_failed
    bnez    a0, ts_parent

    /* The child: its own group, then a little work, then 7. */
    li      a0, 0
    li      a1, 0
    li      a7, SYS_SETPGID
    ecall
    /* **Long enough to still be running when the key is typed**, which the
     * harness does about a second after the marker. The two constraints
     * pull apart: the child must outlast the keystroke, and the parent must
     * outwait the child. So the child works for roughly three seconds and
     * the parent waits six — with a five-millisecond child, which is what
     * this was first, the key arrived after it had finished and the test
     * reported 174 for a reason that had nothing to do with the kernel. */
    li      t0, 1500000000
ts_child_spin:
    addi    t0, t0, -1
    bnez    t0, ts_child_spin
    li      a0, 1
    lla     a1, ts_child_msg
    li      a2, ts_child_msg_end - ts_child_msg
    li      a7, SYS_WRITE
    ecall
    li      a0, 7
    li      a7, SYS_EXIT_GROUP
    ecall

ts_parent:
    mv      s4, a0               /* the child's pid, and its group */
    /* The parent sets the child's group too, so there is no window in
     * which the keystroke could arrive before the move. */
    mv      a0, s4
    mv      a1, s4
    li      a7, SYS_SETPGID
    ecall
    bltz    a0, ts_setpgid_failed

    /* ioctl(0, TIOCSPGRP, &pgid): the child's group owns the keyboard. */
    addi    sp, sp, -16
    sw      s4, 0(sp)
    li      a0, 0
    li      a1, 0x5410           /* TIOCSPGRP */
    mv      a2, sp
    li      a7, SYS_IOCTL
    ecall
    bltz    a0, ts_ioctl_failed

    /* The marker the harness waits for before typing. */
    li      a0, 1
    lla     a1, ts_ready
    li      a2, ts_ready_end - ts_ready
    li      a7, SYS_WRITE
    ecall

    /* Long enough that a child which was never stopped has finished. */
    /* Two seconds, in `tv_sec`. **Not in `tv_nsec`**, which is where this
     * first put it: a `timespec` with 2,000,000,000 nanoseconds is not a
     * long sleep, it is an invalid argument — the kernel answers `EINVAL`
     * and the call returns at once. The test then passed without ever
     * waiting, in both the keystroke case and the control, which is how it
     * was caught. */
    addi    sp, sp, -16
    li      t0, 6
    sd      t0, 0(sp)
    sd      zero, 8(sp)
    mv      a0, sp
    li      a1, 0
    li      a7, SYS_NANOSLEEP
    ecall
    addi    sp, sp, 16

    /* It must still be there: stopped, not finished. */
    addi    sp, sp, -16
    sw      zero, 0(sp)
    mv      a0, s4
    mv      a1, sp
    li      a2, 1                /* WNOHANG */
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bnez    a0, ts_not_stopped

    /* SIGCONT to the group, then reap it. */
    sub     a0, zero, s4
    li      a1, 18               /* SIGCONT */
    li      a7, SYS_KILL
    ecall
    bltz    a0, ts_setpgid_failed
    addi    sp, sp, -16
    sw      zero, 0(sp)
    li      a0, -1
    mv      a1, sp
    li      a2, 0
    li      a3, 0
    li      a7, SYS_WAIT4
    ecall
    bltz    a0, ts_wait_failed
    lw      t0, 0(sp)
    srli    t0, t0, 8
    andi    a0, t0, 0xff
    li      a7, SYS_EXIT_GROUP
    ecall

ts_clone_failed:
    li      a0, 171
    li      a7, SYS_EXIT_GROUP
    ecall
ts_setpgid_failed:
    li      a0, 172
    li      a7, SYS_EXIT_GROUP
    ecall
ts_ioctl_failed:
    li      a0, 173
    li      a7, SYS_EXIT_GROUP
    ecall
ts_not_stopped:
    li      a0, 174
    li      a7, SYS_EXIT_GROUP
    ecall
ts_wait_failed:
    li      a0, 175
    li      a7, SYS_EXIT_GROUP
    ecall
ts_ready:
    .ascii  "tstp: the keyboard is the child's\n"
ts_ready_end:
ts_child_msg:
    .ascii  "tstp: the child finished its work\n"
ts_child_msg_end:
    .balign 4
10: j       10b
user_tstp_end:

/* Roadmap §2: a page that goes out to swap and comes back.
 *
 * The program cannot see swap and must not be able to: that is the whole
 * promise. So it writes a pattern across several pages, asks the kernel —
 * through a syscall it is allowed to make — to take them away, and reads
 * the pattern back. If the bytes are right, the pages went somewhere and
 * came back; the kernel's own counters say where.
 *
 * `madvise(addr, len, MADV_PAGEOUT)` is the request. Linux has exactly this
 * advice for exactly this purpose, so the program is asking an ordinary
 * question rather than using a private interface — and a kernel that
 * ignores it (as this one did until now) simply answers 0 and the pattern
 * is still right, which is why the kernel's counters are what prove it.
 *
 * **0 is a pass**; 181..183 name the step.
 */
    .globl user_swap_start
    .globl user_swap_end
user_swap_start:
    /* mmap(NULL, 64 KiB, RW, PRIVATE|ANON) */
    li      a0, 0
    li      a1, 65536
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, sw_no_memory
    mv      s3, a0

    /* A word per page, each the page's own index plus one — so a page that
     * comes back as zeroes, or as another page, is caught. */
    li      t3, 4096
    mv      t1, s3
    li      t2, 1
    li      t4, 17
sw_fill:
    sd      t2, 0(t1)
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, sw_fill

    /* madvise(addr, 64 KiB, MADV_PAGEOUT=21) */
    mv      a0, s3
    li      a1, 65536
    li      a2, 21
    li      a7, SYS_MADVISE
    ecall
    bltz    a0, sw_advice_refused

    /* And read every one of them back. */
    li      t3, 4096
    mv      t1, s3
    li      t2, 1
    li      t4, 17
sw_check:
    ld      t5, 0(t1)
    bne     t5, t2, sw_came_back_wrong
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, sw_check

    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

sw_no_memory:
    li      a0, 181
    li      a7, SYS_EXIT_GROUP
    ecall
sw_advice_refused:
    li      a0, 182
    li      a7, SYS_EXIT_GROUP
    ecall
sw_came_back_wrong:
    li      a0, 183
    li      a7, SYS_EXIT_GROUP
    ecall
11: j       11b
user_swap_end:

/* Roadmap §0: does a preempted thread get all of its registers back?
 *
 * **The question the race hunt came down to, asked directly.** Everything
 * else about §0 was measured through a Go program, which can only say that
 * *something* went wrong somewhere; this asks the kernel the narrow
 * question and gets a register number for an answer.
 *
 * Every register the ABI lets a program keep is loaded with a value only
 * this program would choose — its own number, in the high half so that a
 * small arithmetic slip cannot look like a match — and then the program
 * spins long enough to be preempted many times over. The kernel's tick is
 * 100 Hz and a slice is milliseconds, so a loop of a few hundred million
 * iterations is preempted hundreds of times. Then every register is
 * compared against what it should still hold.
 *
 * `t0`..`t2` and `a0`..`a7` are left out: they are the loop's counter and
 * the exit syscall's arguments, so they cannot hold a canary across both.
 * That is not a gap worth closing here — a kernel that restored only the
 * temporaries correctly and lost a saved register is the case this is
 * looking for, and the saved registers are where a compiler keeps a live
 * pointer across a call.
 *
 * **0 is a pass.** Any other value is the number of the first register that
 * came back wrong, which is the whole point: a count would say the kernel
 * is broken and a number says where.
 */
    .globl user_regs_start
    .globl user_regs_end
user_regs_start:
    /* **Company first.** A single thread is never preempted, because the
     * kernel has nothing to put on the hart instead — so the first version
     * of this test passed without ever being interrupted, which is the
     * oldest mistake in this file wearing new clothes. Three spinners are
     * cloned, and only then does the canary thread start timing itself. */
    li      s7, 3
regs_spawn:
    li      a0, 0
    li      a1, 8192
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, regs_no_memory
    li      t0, 8192
    add     a1, a0, t0              /* a stack grows down, so its top */
    li      a0, 0x00050f00          /* VM|FS|FILES|SIGHAND|THREAD */
    li      a2, 0
    li      a3, 0
    li      a4, 0
    li      a7, SYS_CLONE
    ecall
    beqz    a0, regs_spin_forever   /* the child: just keep the hart busy */
    addi    s7, s7, -1
    bnez    s7, regs_spawn

    /* The canaries. Every register the ABI lets this program keep across an
     * interruption, **including the caller-saved ones**: `t0`..`t2` are the
     * only ones left out, because checking needs scratch, and `sp`, which
     * is the stack. A runtime keeps a live pointer in a temporary across a
     * preemption as readily as in a saved register, so leaving the
     * temporaries out — which the first version of this did — leaves out
     * the half that matters. The value is its own register number with a
     * distinctive high half, so a small arithmetic slip cannot pass. */
    li      t0, 0x5EC0
    slli    t0, t0, 48
    ori     ra, t0, 1
    ori     gp, t0, 3
    ori     tp, t0, 4
    ori     s0, t0, 8
    ori     s1, t0, 9
    ori     a0, t0, 10
    ori     a1, t0, 11
    ori     a2, t0, 12
    ori     a3, t0, 13
    ori     a4, t0, 14
    ori     a5, t0, 15
    ori     a6, t0, 16
    ori     a7, t0, 17
    ori     s2, t0, 18
    ori     s3, t0, 19
    ori     s4, t0, 20
    ori     s5, t0, 21
    ori     s6, t0, 22
    ori     s7, t0, 23
    ori     s8, t0, 24
    ori     s9, t0, 25
    ori     s10, t0, 26
    ori     s11, t0, 27
    ori     t3, t0, 28
    ori     t4, t0, 29
    ori     t5, t0, 30
    ori     t6, t0, 31

    /* Spin, touching only `t1`. Long enough that a 100 Hz tick with three
     * other runnable threads cannot miss it. */
    li      t1, 60000000
regs_spin:
    addi    t1, t1, -1
    bnez    t1, regs_spin

    /* Check. `t2` carries the first register that came back wrong, and 0
     * means every one of them survived. */
    li      t0, 0x5EC0
    slli    t0, t0, 48
    li      t2, 0

    ori     t1, t0, 1
    bne     ra, t1, regs_bad_1
    ori     t1, t0, 3
    bne     gp, t1, regs_bad_3
    ori     t1, t0, 4
    bne     tp, t1, regs_bad_4
    ori     t1, t0, 8
    bne     s0, t1, regs_bad_8
    ori     t1, t0, 9
    bne     s1, t1, regs_bad_9
    ori     t1, t0, 10
    bne     a0, t1, regs_bad_10
    ori     t1, t0, 11
    bne     a1, t1, regs_bad_11
    ori     t1, t0, 12
    bne     a2, t1, regs_bad_12
    ori     t1, t0, 13
    bne     a3, t1, regs_bad_13
    ori     t1, t0, 14
    bne     a4, t1, regs_bad_14
    ori     t1, t0, 15
    bne     a5, t1, regs_bad_15
    ori     t1, t0, 16
    bne     a6, t1, regs_bad_16
    ori     t1, t0, 17
    bne     a7, t1, regs_bad_17
    ori     t1, t0, 18
    bne     s2, t1, regs_bad_18
    ori     t1, t0, 19
    bne     s3, t1, regs_bad_19
    ori     t1, t0, 20
    bne     s4, t1, regs_bad_20
    ori     t1, t0, 21
    bne     s5, t1, regs_bad_21
    ori     t1, t0, 22
    bne     s6, t1, regs_bad_22
    ori     t1, t0, 23
    bne     s7, t1, regs_bad_23
    ori     t1, t0, 24
    bne     s8, t1, regs_bad_24
    ori     t1, t0, 25
    bne     s9, t1, regs_bad_25
    ori     t1, t0, 26
    bne     s10, t1, regs_bad_26
    ori     t1, t0, 27
    bne     s11, t1, regs_bad_27
    ori     t1, t0, 28
    bne     t3, t1, regs_bad_28
    ori     t1, t0, 29
    bne     t4, t1, regs_bad_29
    ori     t1, t0, 30
    bne     t5, t1, regs_bad_30
    ori     t1, t0, 31
    bne     t6, t1, regs_bad_31

    /* **The second half: a switch the thread asked for.**
     *
     * Everything above is about being interrupted. A thread that *blocks*
     * takes a different path out of the kernel and back — it switches away
     * from inside a syscall, on its own kernel stack, and is resumed by
     * whoever wakes it — and the two have nothing in common but the frame.
     * `minikern.cooperative` showed that the §0 corruption needs only this
     * kind of switch, so this is the one that had to be tested.
     *
     * The saved registers are reloaded first, because a syscall is entitled
     * to the argument and return registers: `a0`..`a7` and `t0`..`t2` carry
     * the call, so they cannot also carry a canary across it. Everything
     * the ABI says a *call* preserves must still be here afterwards.
     *
     * 1 ms, fifty times, with three spinners competing: fifty blocks, fifty
     * wake-ups and fifty resumptions. */
    li      s0, 50
regs_block:
    li      t0, 0x5EC0
    slli    t0, t0, 48
    ori     gp, t0, 3
    ori     tp, t0, 4
    ori     s1, t0, 9
    ori     s2, t0, 18
    ori     s3, t0, 19
    ori     s4, t0, 20
    ori     s5, t0, 21
    ori     s6, t0, 22
    ori     s7, t0, 23
    ori     s8, t0, 24
    ori     s9, t0, 25
    ori     s10, t0, 26
    ori     s11, t0, 27
    ori     t3, t0, 28
    ori     t4, t0, 29
    ori     t5, t0, 30
    ori     t6, t0, 31

    addi    sp, sp, -16
    li      t1, 0
    sd      t1, 0(sp)               /* tv_sec  = 0 */
    li      t1, 1000000
    sd      t1, 8(sp)               /* tv_nsec = 1 ms */
    mv      a0, sp
    li      a1, 0
    li      a7, SYS_NANOSLEEP
    ecall
    addi    sp, sp, 16

    li      t0, 0x5EC0
    slli    t0, t0, 48
    ori     t1, t0, 3
    bne     gp, t1, regs_bad_3
    ori     t1, t0, 4
    bne     tp, t1, regs_bad_4
    ori     t1, t0, 9
    bne     s1, t1, regs_bad_9
    ori     t1, t0, 18
    bne     s2, t1, regs_bad_18
    ori     t1, t0, 19
    bne     s3, t1, regs_bad_19
    ori     t1, t0, 20
    bne     s4, t1, regs_bad_20
    ori     t1, t0, 21
    bne     s5, t1, regs_bad_21
    ori     t1, t0, 22
    bne     s6, t1, regs_bad_22
    ori     t1, t0, 23
    bne     s7, t1, regs_bad_23
    ori     t1, t0, 24
    bne     s8, t1, regs_bad_24
    ori     t1, t0, 25
    bne     s9, t1, regs_bad_25
    ori     t1, t0, 26
    bne     s10, t1, regs_bad_26
    ori     t1, t0, 27
    bne     s11, t1, regs_bad_27
    ori     t1, t0, 28
    bne     t3, t1, regs_bad_28
    ori     t1, t0, 29
    bne     t4, t1, regs_bad_29
    ori     t1, t0, 30
    bne     t5, t1, regs_bad_30
    ori     t1, t0, 31
    bne     t6, t1, regs_bad_31
    addi    s0, s0, -1
    bnez    s0, regs_block

    j       regs_exit

regs_spin_forever:
    j       regs_spin_forever

regs_no_memory:
    li      t2, 180
    j       regs_exit

regs_bad_1:  li t2, 1
    j regs_exit
regs_bad_3:  li t2, 3
    j regs_exit
regs_bad_4:  li t2, 4
    j regs_exit
regs_bad_8:  li t2, 8
    j regs_exit
regs_bad_9:  li t2, 9
    j regs_exit
regs_bad_10: li t2, 10
    j regs_exit
regs_bad_11: li t2, 11
    j regs_exit
regs_bad_12: li t2, 12
    j regs_exit
regs_bad_13: li t2, 13
    j regs_exit
regs_bad_14: li t2, 14
    j regs_exit
regs_bad_15: li t2, 15
    j regs_exit
regs_bad_16: li t2, 16
    j regs_exit
regs_bad_17: li t2, 17
    j regs_exit
regs_bad_18: li t2, 18
    j regs_exit
regs_bad_19: li t2, 19
    j regs_exit
regs_bad_20: li t2, 20
    j regs_exit
regs_bad_21: li t2, 21
    j regs_exit
regs_bad_22: li t2, 22
    j regs_exit
regs_bad_23: li t2, 23
    j regs_exit
regs_bad_24: li t2, 24
    j regs_exit
regs_bad_25: li t2, 25
    j regs_exit
regs_bad_26: li t2, 26
    j regs_exit
regs_bad_27: li t2, 27
    j regs_exit
regs_bad_28: li t2, 28
    j regs_exit
regs_bad_29: li t2, 29
    j regs_exit
regs_bad_30: li t2, 30
    j regs_exit
regs_bad_31: li t2, 31
regs_exit:
    /* **`exit_group`, not `exit`.** The spinners never stop by themselves,
     * and `exit` ends only the thread that calls it — so the first version
     * of this ended the one thread that had an answer and left the process
     * alive with three threads spinning, which from outside is a hang. */
    mv      a0, t2
    li      a7, SYS_EXIT_GROUP
    ecall
user_regs_end:

/* Roadmap §0: `MADV_DONTNEED` means the pages are gone and read as zero.
 *
 * **The guarantee §0's corruption came from breaking.** Its name says
 * advice, and for private memory Linux's contract does not: the pages are
 * discarded and the next read of those addresses returns zero. A runtime
 * builds on that — Go's allocator returns memory this way, records the
 * range as zeroed, and skips zeroing it when it hands the range out again.
 * A kernel that keeps the pages has not declined optional advice; it has
 * told the program its memory is zero while it still holds the previous
 * owner's bytes.
 *
 * So the test is behavioural and it is the whole point: write a pattern
 * that is **not** zero, prove it is really there, discard it, and require
 * zero. Searching the kernel for the constant proves nothing; this proves
 * what a program would see.
 *
 * **0 is a pass**; 190..193 name the step.
 */
    .globl user_dontneed_start
    .globl user_dontneed_end
user_dontneed_start:
    /* mmap(NULL, 64 KiB, RW, PRIVATE|ANON) */
    li      a0, 0
    li      a1, 65536
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, dn_no_memory
    mv      s3, a0

    /* A word per page, each its own index plus one: non-zero everywhere,
     * and distinct so that a page swapped for another is caught too. */
    li      t3, 4096
    mv      t1, s3
    li      t2, 1
    li      t4, 17
dn_fill:
    sd      t2, 0(t1)
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, dn_fill

    /* It really is there. Without this the test could pass on a kernel
     * whose pages were never written at all. */
    mv      t1, s3
    li      t2, 1
dn_verify_written:
    ld      t5, 0(t1)
    bne     t5, t2, dn_not_written
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, dn_verify_written

    /* madvise(addr, 64 KiB, MADV_DONTNEED=4) */
    mv      a0, s3
    li      a1, 65536
    li      a2, 4
    li      a7, SYS_MADVISE
    ecall
    bnez    a0, dn_refused

    /* Every word must now be zero — **sixteen of them, not seventeen.**
     *
     * The fill loop counts 1..16 because the value it writes is the page's
     * index plus one, so its limit is 17; this loop counts 0..15 and its
     * limit is 16. Sharing the 17 read one page *past* the mapping, which
     * the emulator happened to answer with zero and the board did not —
     * so the first board run of this test failed at index 16, an index the
     * mapping does not have. A loop bound copied from a loop that starts
     * somewhere else is the oldest off-by-one there is. */
    mv      t1, s3
    li      t2, 0
    li      t4, 16
dn_verify_zero:
    ld      t5, 0(t1)
    bnez    t5, dn_not_zero
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, dn_verify_zero

    /* And the addresses still work: writing and reading them again must
     * succeed, because `MADV_DONTNEED` takes the pages and not the
     * mapping. */
    li      t5, 0x1234
    sd      t5, 0(s3)
    ld      t6, 0(s3)
    bne     t5, t6, dn_not_usable

    li      a0, 0
    j       dn_exit
dn_no_memory:
    li      a0, 190
    j       dn_exit
dn_not_written:
    li      a0, 191
    j       dn_exit
dn_refused:
    li      a0, 192
    j       dn_exit
dn_not_zero:
    /* **Which page, not just that one failed.** The first run of this on
     * real hardware exited 193 where the emulator exits 0, and "a page did
     * not come back zero" is not enough to act on: the first page and the
     * sixteenth fail for different reasons. `t2` is the index. */
    li      a0, 200
    add     a0, a0, t2
    j       dn_exit
dn_not_usable:
    li      a0, 194
dn_exit:
    li      a7, SYS_EXIT_GROUP
    ecall
user_dontneed_end:

/* Roadmap §0: can two separate mappings ever be the same memory?
 *
 * **The §0 question, asked without Go and without the card.** The fast
 * reproducer is a Go binary, so it needs a filesystem to be loaded from and
 * has only ever run under the emulator — and the emulator is not the
 * arbiter here. What it reports is that a fresh allocation holds a previous
 * one's bytes while the two ranges provably do not overlap, which is either
 * two virtual ranges over one frame or bytes arriving after a range was
 * cleared. A program that does nothing but map, stamp and check can ask
 * that on real silicon, in the kernel's own image, with no runtime in the
 * way to be suspected.
 *
 * Eight regions of 64 KiB are kept at once, each stamped with a word made
 * of its own slot number so that a byte from the wrong region names the
 * region it came from. Every iteration throws one away, maps a new one in
 * its place, stamps it, and then **re-checks all eight** — so a frame
 * handed out while still mapped elsewhere is caught on the next pass rather
 * than whenever something happens to read it. Four hundred iterations is
 * four hundred `mmap`s and nearly as many `munmap`s, which is the churn
 * that makes an allocator or a stale translation show itself.
 *
 * One word per page is checked rather than every byte: `mmap` hands out
 * whole pages, so an overlap is page-granular and the first word of a page
 * is as good a witness as all 4,096 — and it keeps four hundred passes over
 * half a megabyte quick enough to run on every board boot.
 *
 * **0 is a pass.** 170 is out of memory, 171 a mapping refused, and
 * 100 + slot names the region whose stamp came back wrong.
 */
#define MC_REGION 65536
#define MC_SLOTS 8
#define MC_ROUNDS 400

    .globl user_mapchurn_start
    .globl user_mapchurn_end
user_mapchurn_start:
    /* A page to hold the eight bases, because a slot is an index and
     * registers cannot be indexed. */
    li      a0, 0
    li      a1, 4096
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mc_no_memory
    mv      s3, a0
    li      s2, 0                   /* the iteration */

mc_loop:
    andi    t0, s2, (MC_SLOTS - 1)  /* the slot this round replaces */
    slli    t1, t0, 3
    add     s4, s3, t1              /* &base[slot], kept across the calls */

    /* Throw the old one away, if there is one. */
    ld      t2, 0(s4)
    beqz    t2, mc_fresh
    mv      a0, t2
    li      a1, MC_REGION
    li      a7, SYS_MUNMAP
    ecall

mc_fresh:
    li      a0, 0
    li      a1, MC_REGION
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, mc_refused
    sd      a0, 0(s4)

    /* Stamp it: the slot's number in every byte of every page's first word,
     * so a word read from the wrong region says which region it was. */
    andi    t0, s2, (MC_SLOTS - 1)
    addi    t3, t0, 1
    li      t4, 0x0101
    slli    t5, t4, 16
    or      t4, t4, t5
    slli    t5, t4, 32
    or      t4, t4, t5              /* 0x0101010101010101 */
    mul     t3, t3, t4              /* the slot's stamp word */
    mv      t1, a0
    li      t5, MC_REGION
    add     t5, t1, t5              /* the end */
    li      t6, 4096
mc_stamp:
    sd      t3, 0(t1)
    add     t1, t1, t6
    blt     t1, t5, mc_stamp

    /* Now every slot must still hold its own stamp. */
    li      t0, 0
mc_check_slot:
    slli    t1, t0, 3
    add     t1, s3, t1
    ld      t2, 0(t1)               /* this slot's base */
    beqz    t2, mc_next_slot
    addi    t3, t0, 1
    li      t4, 0x0101
    slli    t5, t4, 16
    or      t4, t4, t5
    slli    t5, t4, 32
    or      t4, t4, t5
    mul     t3, t3, t4              /* what it should hold */
    li      t6, MC_REGION
    add     t6, t2, t6
    li      a1, 4096
mc_check_page:
    ld      a2, 0(t2)
    bne     a2, t3, mc_wrong
    add     t2, t2, a1
    blt     t2, t6, mc_check_page
mc_next_slot:
    addi    t0, t0, 1
    li      t1, MC_SLOTS
    blt     t0, t1, mc_check_slot

    addi    s2, s2, 1
    li      t0, MC_ROUNDS
    blt     s2, t0, mc_loop

    li      a0, 0
    j       mc_exit
mc_no_memory:
    li      a0, 170
    j       mc_exit
mc_refused:
    li      a0, 171
    j       mc_exit
mc_wrong:
    /* 100 + the slot whose stamp was wrong. The value that was there is in
     * `a2` and the kernel prints the exit status only, so the slot is what
     * there is room to say — and it is the half that matters: it names the
     * region whose memory went somewhere else. */
    li      a0, 100
    add     a0, a0, t0
mc_exit:
    li      a7, SYS_EXIT_GROUP
    ecall
user_mapchurn_end:

/* Roadmap §2: pages that go out because the machine is short of memory,
 * not because the program asked.
 *
 * **It asks for nothing**, which is the entire point. `user_swap` calls
 * `madvise(MADV_PAGEOUT)` and so proves the mechanism; this proves the
 * *policy*: a program that allocates and uses memory while the kernel is
 * below its watermark must have its pages taken and given back without
 * noticing, and the kernel's `pages_out_unasked` counter is what says they
 * went anywhere.
 *
 * A megabyte, touched twice. The second pass is what matters: by then the
 * kernel has had the chance to take pages the first pass wrote, so reading
 * the pattern back is reading through the fault handler and out of the
 * swapfile. One pass would prove only that writing memory works.
 *
 * **0 is a pass**; 191..193 name the step.
 */
    .globl user_pressure_start
    .globl user_pressure_end
user_pressure_start:
    /* mmap(NULL, 1 MiB, RW, PRIVATE|ANON) */
    li      a0, 0
    li      a1, 1048576
    li      a2, 3
    li      a3, 0x22
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP
    ecall
    bltz    a0, pr_no_memory
    mv      s3, a0

    /* A word per page: the page's index plus one, so a page that comes back
     * as zeroes or as a different page is caught rather than plausible. */
    li      t3, 4096
    mv      t1, s3
    li      t2, 1
    li      t4, 257
pr_fill:
    sd      t2, 0(t1)
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, pr_fill

    /* And again, reading each one before writing it: every page the kernel
     * took in the meantime faults back in here. */
    li      t3, 4096
    mv      t1, s3
    li      t2, 1
    li      t4, 257
pr_check:
    ld      t5, 0(t1)
    bne     t5, t2, pr_came_back_wrong
    sd      t2, 0(t1)
    add     t1, t1, t3
    addi    t2, t2, 1
    blt     t2, t4, pr_check

    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

pr_no_memory:
    li      a0, 191
    li      a7, SYS_EXIT_GROUP
    ecall
pr_came_back_wrong:
    /* 193 plus nothing: the index is not reported because the interesting
     * fact is that any page came back wrong at all, and the exit status is
     * one byte. */
    li      a0, 193
    li      a7, SYS_EXIT_GROUP
    ecall
    .balign 4
10: j       10b
user_pressure_end:

/* **Capabilities and `seccomp`, which only a running process can show.**
 *
 * The rules are pure and host-tested — `caps_test` covers every refusal
 * `cap_change_allowed` makes, and `bpf_test` covers every program the
 * verifier rejects. What those cannot show is that the kernel *consults*
 * them: a capability model whose bits are maintained perfectly and never
 * asked is privilege separation on paper only, and that is exactly the
 * failure this project has shipped twice in other guises (a declared and
 * never-read property, and a control that was vacuous).
 *
 * So each step here is about the consulting:
 *
 *   1. The first process is root and holds `CAP_SYS_ADMIN`. `capget` says
 *      so, which also proves the ABI's two-word split is right way round.
 *   2. `swapon` with a nonsense path fails — but **not** with `EPERM`,
 *      because the capability is held. This is the control: without it,
 *      step 4's refusal would prove nothing, since every `swapon` here
 *      fails for some reason.
 *   3. `PR_CAPBSET_DROP` of `CAP_SYS_ADMIN` succeeds, and `PR_CAPBSET_READ`
 *      then says it is gone.
 *   4. `swapon` now fails with **`EPERM`**, from a process that is still
 *      uid 0. That is the whole claim of a capability model: privilege is
 *      what you hold, not who you are.
 *   5. `PR_SET_NO_NEW_PRIVS` succeeds, which an unprivileged filter needs.
 *   6. A filter that returns `ERRNO 42` for `getpid` and allows everything
 *      else is installed, and `getpid` then returns **-42** while `write`
 *      still works. Both halves matter: a filter that broke everything
 *      would pass a test that only checked the refusal.
 *
 * 0 means every step held; 151..157 name the one that did not.
 */
#define SYS_CAPGET_N 90
#define SYS_PRCTL_N 167
#define SYS_SECCOMP_N 277
#define SYS_SWAPON_N 224
#define PR_CAPBSET_READ_N 23
#define PR_CAPBSET_DROP_N 24
#define PR_SET_NO_NEW_PRIVS_N 38
#define CAP_SYS_ADMIN_N 21
#define EPERM_N 1

    .globl user_privsep_start
    .globl user_privsep_end
    .balign 4
user_privsep_start:
    /* A stack to put the structures on: the program is loaded without one
     * of its own beyond what the kernel gave, and `sp` is valid here. */
    addi    sp, sp, -256
    /* 1. capget(&hdr, &data) — hdr at sp, data at sp+16. */
    li      t0, 0x20080522          /* _LINUX_CAPABILITY_VERSION_3 */
    sw      t0, 0(sp)
    sw      zero, 4(sp)             /* pid 0 means "me" */
    addi    a0, sp, 0
    addi    a1, sp, 16
    li      a7, SYS_CAPGET_N
    ecall
    bnez    a0, ps_capget_failed
    /* data[0].effective is the low word; CAP_SYS_ADMIN is bit 21. */
    lw      t0, 16(sp)
    li      t1, 1
    slli    t1, t1, CAP_SYS_ADMIN_N
    and     t2, t0, t1
    beqz    t2, ps_not_admin

    /* 2. The control: swapon fails, but not for want of privilege. */
    addi    a0, sp, 240             /* an empty path: 240 is zeroed below */
    sw      zero, 240(sp)
    li      a1, 0
    li      a7, SYS_SWAPON_N
    ecall
    li      t0, -EPERM_N
    beq     a0, t0, ps_perm_too_early

    /* 3. Drop CAP_SYS_ADMIN from the bounding set, for good. */
    li      a0, PR_CAPBSET_DROP_N
    li      a1, CAP_SYS_ADMIN_N
    li      a7, SYS_PRCTL_N
    ecall
    bnez    a0, ps_drop_failed
    li      a0, PR_CAPBSET_READ_N
    li      a1, CAP_SYS_ADMIN_N
    li      a7, SYS_PRCTL_N
    ecall
    bnez    a0, ps_still_bounded

    /* 4. And now the same call is refused — still uid 0. */
    addi    a0, sp, 240
    li      a1, 0
    li      a7, SYS_SWAPON_N
    ecall
    li      t0, -EPERM_N
    bne     a0, t0, ps_not_refused

    /* 5. no_new_privs, which an unprivileged filter requires. */
    li      a0, PR_SET_NO_NEW_PRIVS_N
    li      a1, 1
    li      a7, SYS_PRCTL_N
    ecall
    bnez    a0, ps_nnp_failed

    /* 6. The filter, built on the stack at sp+32:
     *      ld   [nr]
     *      jeq  SYS_GETPID ? +0 : +1
     *      ret  ERRNO|42
     *      ret  ALLOW
     * Each instruction is {code:u16, jt:u8, jf:u8, k:u32} = 8 bytes. */
    li      t0, 0x0020              /* BPF_LD|BPF_W|BPF_ABS */
    sh      t0, 32(sp)
    sb      zero, 34(sp)
    sb      zero, 35(sp)
    sw      zero, 36(sp)            /* k = offsetof(nr) = 0 */

    li      t0, 0x0015              /* BPF_JMP|BPF_JEQ|BPF_K */
    sh      t0, 40(sp)
    li      t1, 0
    sb      t1, 42(sp)              /* jt = 0 -> the ERRNO return */
    li      t1, 1
    sb      t1, 43(sp)              /* jf = 1 -> the ALLOW return */
    li      t0, SYS_GETPID
    sw      t0, 44(sp)

    li      t0, 0x0006              /* BPF_RET|BPF_K */
    sh      t0, 48(sp)
    sb      zero, 50(sp)
    sb      zero, 51(sp)
    li      t0, 0x0005002a          /* SECCOMP_RET_ERRNO | 42 */
    sw      t0, 52(sp)

    li      t0, 0x0006
    sh      t0, 56(sp)
    sb      zero, 58(sp)
    sb      zero, 59(sp)
    li      t0, 0x7fff0000          /* SECCOMP_RET_ALLOW */
    sw      t0, 60(sp)

    /* struct sock_fprog { u16 len; u64 filter; } at sp+64, with the
     * pointer at +8 because of the alignment the ABI gives it. */
    li      t0, 4
    sh      t0, 64(sp)
    addi    t0, sp, 32
    sd      t0, 72(sp)

    li      a0, 1                   /* SECCOMP_SET_MODE_FILTER */
    li      a1, 0
    addi    a2, sp, 64
    li      a7, SYS_SECCOMP_N
    ecall
    bnez    a0, ps_filter_refused

    /* getpid must now be refused with exactly that errno... */
    li      a7, SYS_GETPID
    ecall
    li      t0, -42
    bne     a0, t0, ps_not_filtered

    /* ...and write must still work, or the filter proves nothing. */
    li      a0, 1
    addi    a1, sp, 240
    li      a2, 1
    li      a7, SYS_WRITE
    ecall
    blez    a0, ps_write_broken

    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

ps_capget_failed:
    li      a0, 151
    li      a7, SYS_EXIT_GROUP
    ecall
ps_not_admin:
    li      a0, 152
    li      a7, SYS_EXIT_GROUP
    ecall
ps_perm_too_early:
    li      a0, 153
    li      a7, SYS_EXIT_GROUP
    ecall
ps_drop_failed:
    li      a0, 154
    li      a7, SYS_EXIT_GROUP
    ecall
ps_still_bounded:
    li      a0, 155
    li      a7, SYS_EXIT_GROUP
    ecall
ps_not_refused:
    li      a0, 156
    li      a7, SYS_EXIT_GROUP
    ecall
ps_nnp_failed:
    li      a0, 157
    li      a7, SYS_EXIT_GROUP
    ecall
ps_filter_refused:
    li      a0, 158
    li      a7, SYS_EXIT_GROUP
    ecall
ps_not_filtered:
    li      a0, 159
    li      a7, SYS_EXIT_GROUP
    ecall
ps_write_broken:
    li      a0, 160
    li      a7, SYS_EXIT_GROUP
    ecall
11: j       11b
user_privsep_end:

/* **Address randomisation, reported so that two runs can be compared.**
 *
 * The program cannot test randomness by itself — one sample is one sample,
 * and a program that "checked" its own addresses were random would be
 * asserting something unfalsifiable from the inside. So it prints three
 * addresses and the harness runs it twice: the claim is that they differ
 * between runs, and that is a claim only the harness can make.
 *
 * The three are chosen because they are the three the roadmap named:
 *   - the stack, read from `sp`;
 *   - the heap, from `brk(0)`;
 *   - a mapping, from `mmap(NULL, 4096, ...)`.
 *
 * Printed as raw hex through `write`, because there is no `printf` here.
 */
#define SYS_BRK_N 214
#define SYS_MMAP_N 222

    .globl user_aslr_start
    .globl user_aslr_end
    .balign 4
user_aslr_start:
    addi    sp, sp, -64
    mv      s0, sp                  /* keep the buffer, sp changes below */

    /* "sp=" then the stack pointer. */
    mv      s2, sp
    li      t0, 's'
    sb      t0, 0(s0)
    li      t0, 'p'
    sb      t0, 1(s0)
    li      t0, '='
    sb      t0, 2(s0)
    li      a0, 1
    mv      a1, s0
    li      a2, 3
    li      a7, SYS_WRITE
    ecall
    mv      a0, s2
    jal     aslr_put_hex

    /* "brk=" then brk(0). */
    li      t0, 'b'
    sb      t0, 0(s0)
    li      t0, 'r'
    sb      t0, 1(s0)
    li      t0, 'k'
    sb      t0, 2(s0)
    li      t0, '='
    sb      t0, 3(s0)
    li      a0, 1
    mv      a1, s0
    li      a2, 4
    li      a7, SYS_WRITE
    ecall
    li      a0, 0
    li      a7, SYS_BRK_N
    ecall
    jal     aslr_put_hex

    /* "map=" then an anonymous mapping's address. */
    li      t0, 'm'
    sb      t0, 0(s0)
    li      t0, 'a'
    sb      t0, 1(s0)
    li      t0, 'p'
    sb      t0, 2(s0)
    li      t0, '='
    sb      t0, 3(s0)
    li      a0, 1
    mv      a1, s0
    li      a2, 4
    li      a7, SYS_WRITE
    ecall
    li      a0, 0
    li      a1, 4096
    li      a2, 3                   /* PROT_READ|PROT_WRITE */
    li      a3, 0x22                /* MAP_PRIVATE|MAP_ANONYMOUS */
    li      a4, -1
    li      a5, 0
    li      a7, SYS_MMAP_N
    ecall
    jal     aslr_put_hex

    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall

/* Writes a0 as sixteen hex digits and a newline. Clobbers t0..t4, a0..a2. */
aslr_put_hex:
    mv      s1, ra
    mv      t3, a0
    li      t4, 60                  /* the shift for the top nibble */
aslr_hex_loop:
    srl     t0, t3, t4
    andi    t0, t0, 15
    li      t1, 10
    blt     t0, t1, aslr_hex_digit
    addi    t0, t0, 'a' - 10
    j       aslr_hex_emit
aslr_hex_digit:
    addi    t0, t0, '0'
aslr_hex_emit:
    sb      t0, 16(s0)
    li      a0, 1
    addi    a1, s0, 16
    li      a2, 1
    li      a7, SYS_WRITE
    ecall
    addi    t4, t4, -4
    bgez    t4, aslr_hex_loop
    li      t0, '\n'
    sb      t0, 16(s0)
    li      a0, 1
    addi    a1, s0, 16
    li      a2, 1
    li      a7, SYS_WRITE
    ecall
    mv      ra, s1
    ret
12: j       12b
user_aslr_end:

/* **`/dev/virtual/kernel_logger`, read the way a program reads it.**
 *
 * The kernel cannot test this for itself: a descriptor needs a process's
 * table, and the first attempt at this check — from the boot path — was
 * refused with `EBADF`, which was the fd layer telling the truth.
 *
 * It opens the device, reads it to the end, and counts newlines. A log tail
 * that opened and returned nothing would pass a test that only checked the
 * open, which is the usual way a device file comes to be "working".
 *
 * 0 means it opened and gave at least one whole line; 171 that it would not
 * open, 172 that it opened and gave nothing.
 */
#define SYS_OPENAT_N 56
#define SYS_READ_N 63
#define SYS_CLOSE_N 57
#define AT_FDCWD_N -100

    .globl user_klogdev_start
    .globl user_klogdev_end
    .balign 4
user_klogdev_start:
    addi    sp, sp, -512
    /* The path, built on the stack: "/dev/virtual/kernel_logger" */
    la      t0, klogdev_path
    mv      t1, sp
1:  lb      t2, 0(t0)
    sb      t2, 0(t1)
    addi    t0, t0, 1
    addi    t1, t1, 1
    bnez    t2, 1b

    li      a0, AT_FDCWD_N
    mv      a1, sp
    li      a2, 0                   /* O_RDONLY */
    li      a3, 0
    li      a7, SYS_OPENAT_N
    ecall
    bltz    a0, kd_no_open
    mv      s3, a0                  /* the descriptor */
    li      s2, 0                   /* newlines seen */

kd_loop:
    mv      a0, s3
    addi    a1, sp, 64
    li      a2, 256
    li      a7, SYS_READ_N
    ecall
    blez    a0, kd_done
    mv      t3, a0
    addi    t4, sp, 64
2:  lb      t5, 0(t4)
    li      t6, 10
    bne     t5, t6, 3f
    addi    s2, s2, 1
3:  addi    t4, t4, 1
    addi    t3, t3, -1
    bnez    t3, 2b
    j       kd_loop

kd_done:
    mv      a0, s3
    li      a7, SYS_CLOSE_N
    ecall
    beqz    s2, kd_empty
    li      a0, 0
    li      a7, SYS_EXIT_GROUP
    ecall
kd_no_open:
    li      a0, 171
    li      a7, SYS_EXIT_GROUP
    ecall
kd_empty:
    li      a0, 172
    li      a7, SYS_EXIT_GROUP
    ecall
    .balign 8
klogdev_path:
    .asciz "/dev/virtual/kernel_logger"
    .balign 4
13: j       13b
user_klogdev_end:

/* ── a single-threaded execve, to settle the exec'd-Go fault ──────────────
 *
 * **The one comparison left.** A Go program runs when the kernel loads it
 * and faults when another Go program execs it, with the image, the stack,
 * the loader, the handlers and the tables all measured and identical. What
 * has not been separated is "execve is broken" from "execve *after Go's
 * multi-threaded fork* is broken" — because every exec on this board so far
 * has come from a Go runtime with half a dozen threads alive, where
 * `sched_end_siblings` and `sched_await_siblings` have work to do.
 *
 * This program has exactly one thread, by construction. It execs
 * `/bin/qservice` with a real argv and nothing else going on. If the service
 * then starts, exec is sound and the fault belongs to the multi-threaded
 * case; if it faults identically, exec itself is wrong and every Go detail
 * above was a distraction.
 */
    .globl user_execone_start
    .globl user_execone_end
user_execone_start:
    li      a0, 1
    lla     a1, execone_says
    li      a2, execone_says_end - execone_says
    li      a7, SYS_WRITE
    ecall
    /* **The argv array is built on the stack, not in the blob.**
     *
     * A `.dword execone_path` table holds the *link-time* address, and this
     * program is copied to wherever the loader puts it — so the pointers
     * were wrong at runtime and execve refused before it read anything.
     * `lla` is pc-relative and correct; the pointers it produces go on the
     * stack, which is where a real argv lives anyway. */
    addi    sp, sp, -32
    lla     t0, execone_path
    sd      t0, 0(sp)
    lla     t0, execone_arg1
    sd      t0, 8(sp)
    lla     t0, execone_arg2
    sd      t0, 16(sp)
    sd      zero, 24(sp)
    lla     a0, execone_path
    mv      a1, sp
    addi    a2, sp, 24          /* envp: the NULL already on the stack */
    li      a7, SYS_EXECVE
    ecall
    /* Only reached when execve refused: say so with its own error code. */
    li      a0, 1
    lla     a1, execone_failed
    li      a2, execone_failed_end - execone_failed
    li      a7, SYS_WRITE
    ecall
    li      a0, 9
    li      a7, SYS_EXIT
    ecall
2:  j       2b

    .balign 8
execone_path:
    .asciz  "/bin/tiny"
execone_arg1:
    .asciz  "-name"
execone_arg2:
    .asciz  "solo"
execone_says:
    .ascii  "execone: one thread, about to execve /bin/tiny\n"

execone_says_end:
execone_failed:
    .ascii  "execone: execve REFUSED\n"
execone_failed_end:
user_execone_end:
