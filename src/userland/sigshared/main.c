/* A signal sent to the process is taken by a thread that can take it.
 *
 * `kill` names a process, not a thread. Linux keeps such a signal in the
 * process's pending set until a thread with it unblocked takes it; this
 * kernel used to put it on the process's first thread, so a program whose
 * main thread blocks a signal — Go's runtime, a server with a signal thread
 * — never saw it delivered at all.
 *
 * The main thread blocks SIGUSR1 throughout; a second thread unblocks it.
 * The exit status is a bitmask of the checks that **passed**:
 *   1   a kill to the process ran the handler
 *   2   on the second thread, the one that had it unblocked
 *   4   sent while every thread blocked it, it ran once the second thread
 *       unblocked it — the process held it meanwhile
 *   8   the main thread never received a copy of its own
 */
#include <stdint.h>

#define SYS_EXIT 93
#define SYS_EXIT_GROUP 94
#define SYS_SCHED_YIELD 124
#define SYS_KILL 129
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_GETPID 172
#define SYS_GETTID 178
#define SIGUSR1 10
#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define CLONE_THREAD_FLAGS 0x00010f00 /* VM|FS|FILES|SIGHAND|THREAD */
#define PATIENCE 100000u /* yields before a check gives up: seconds, not minutes */

long clone_thread(long flags, void *stack_top, void (*entry)(void));

struct sigaction_raw {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

static volatile uint32_t handled, phase;
static volatile long handler_tid, child_tid;
static char child_stack[16384] __attribute__((aligned(16)));

static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static void on_usr1(int signo) {
    (void)signo;
    handler_tid = syscall4(SYS_GETTID, 0, 0, 0, 0);
    handled++;
}

static void mask(long how) {
    uint64_t set = 1ull << (SIGUSR1 - 1);
    (void)syscall4(SYS_RT_SIGPROCMASK, how, (long)&set, 0, 8);
}

/* Waits, yielding, until `*what` reaches `want`; false if it never does. */
static int wait_for(volatile uint32_t *what, uint32_t want) {
    for (uint32_t i = 0; i < PATIENCE; i++) {
        if (*what >= want) {
            return 1;
        }
        (void)syscall4(SYS_SCHED_YIELD, 0, 0, 0, 0);
    }
    return 0;
}

/* The second thread: takes the signal, then blocks it, then — once the
 * main thread has sent it again — unblocks it and takes it again. */
static void child(void) {
    child_tid = syscall4(SYS_GETTID, 0, 0, 0, 0);
    mask(SIG_UNBLOCK);
    phase = 1;
    (void)wait_for(&handled, 1);
    mask(SIG_BLOCK);
    phase = 2;
    (void)wait_for(&phase, 3);
    mask(SIG_UNBLOCK);
    (void)wait_for(&handled, 2);
    phase = 4;
}

void user_main(void) {
    struct sigaction_raw act = {.handler = (uint64_t)(uintptr_t)on_usr1, .flags = 0, .mask = 0};
    long pid = syscall4(SYS_GETPID, 0, 0, 0, 0);
    uint32_t passed = 0;
    (void)syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&act, 0, 8);
    mask(SIG_BLOCK);
    (void)clone_thread(CLONE_THREAD_FLAGS, child_stack + sizeof child_stack, child);
    (void)wait_for(&phase, 1);
    (void)syscall4(SYS_KILL, pid, SIGUSR1, 0, 0);
    passed |= wait_for(&handled, 1) ? 1u : 0u;
    passed |= handled >= 1 && handler_tid == child_tid ? 2u : 0u;
    (void)wait_for(&phase, 2);
    (void)syscall4(SYS_KILL, pid, SIGUSR1, 0, 0);
    phase = 3;
    passed |= wait_for(&phase, 4) && handled == 2 && handler_tid == child_tid ? 4u : 0u;
    mask(SIG_UNBLOCK);
    (void)syscall4(SYS_SCHED_YIELD, 0, 0, 0, 0);
    passed |= handled == 2 ? 8u : 0u;
    (void)syscall4(SYS_EXIT_GROUP, passed, 0, 0, 0);
}
