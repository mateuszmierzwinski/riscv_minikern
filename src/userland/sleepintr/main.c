/* `nanosleep` cut short by a signal: EINTR, and the time it had left.
 *
 * It used to sleep through: a signal woke the thread, the handler ran, and
 * the call returned 0 as though the whole interval had passed — so a
 * program that sleeps in a loop, recomputing from `rem`, slept short and
 * never knew. A second thread sends the signal; the main thread sleeps.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   a five-second sleep interrupted at about 200 ms returned EINTR
 *   2   with `rem` between four and five seconds
 *   4   the handler ran
 *   8   interrupted with `rem` NULL, it is still EINTR (and no fault)
 *   16  an uninterrupted 50 ms sleep returns 0 and leaves `rem` alone
 *   32  clock_nanosleep, relative, 50 ms: 0
 *   64  CLOCK_REALTIME is a date after 2020 (the harness passes the epoch)
 *   128 clock_nanosleep TIMER_ABSTIME on CLOCK_MONOTONIC waits until then
 */
#include <stdint.h>

#define SYS_EXIT_GROUP 94
#define SYS_NANOSLEEP 101
#define SYS_CLOCK_GETTIME 113
#define SYS_CLOCK_NANOSLEEP 115
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define TIMER_ABSTIME 1
#define SYS_KILL 129
#define SYS_RT_SIGACTION 134
#define SYS_RT_SIGPROCMASK 135
#define SYS_GETPID 172
#define SIGUSR1 10
#define SIG_BLOCK 0
#define EINTR 4
#define CLONE_THREAD_FLAGS 0x00010f00 /* VM|FS|FILES|SIGHAND|THREAD */

long clone_thread(long flags, void *stack_top, void (*entry)(void));

struct timespec_raw {
    int64_t sec;
    int64_t nsec;
};

struct sigaction_raw {
    uint64_t handler;
    uint64_t flags;
    uint64_t mask;
};

static volatile uint32_t handled;
static volatile long pid;
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
    handled++;
}

static long sleep_ms(int64_t ms, struct timespec_raw *rem) {
    struct timespec_raw req = {.sec = ms / 1000, .nsec = (ms % 1000) * 1000000};
    return syscall4(SYS_NANOSLEEP, (long)&req, (long)rem, 0, 0);
}

/* Blocks the signal (so it is the main thread's to take), then sends it
 * twice, 200 ms apart from each sleep's start. */
static void sender(void) {
    uint64_t set = 1ull << (SIGUSR1 - 1);
    (void)syscall4(SYS_RT_SIGPROCMASK, SIG_BLOCK, (long)&set, 0, 8);
    for (uint32_t i = 0; i < 2u; i++) {
        (void)sleep_ms(200, 0);
        (void)syscall4(SYS_KILL, pid, SIGUSR1, 0, 0);
        (void)sleep_ms(300, 0);
    }
}

static int64_t clock_ns(long clock) {
    struct timespec_raw t = {0, 0};
    (void)syscall4(SYS_CLOCK_GETTIME, clock, (long)&t, 0, 0);
    return t.sec * 1000000000 + t.nsec;
}

/* The clock checks: bits 32, 64 and 128. */
static uint32_t clocks(void) {
    struct timespec_raw req = {.sec = 0, .nsec = 50000000};
    int64_t start = clock_ns(CLOCK_MONOTONIC), at = start + 50000000;
    struct timespec_raw abs_req = {.sec = at / 1000000000, .nsec = at % 1000000000};
    uint32_t passed = 0;
    passed |= syscall4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, 0, (long)&req, 0) == 0 ? 32u : 0u;
    passed |= clock_ns(CLOCK_REALTIME) / 1000000000 > 1577836800 ? 64u : 0u;
    start = clock_ns(CLOCK_MONOTONIC);
    at = start + 50000000;
    abs_req = (struct timespec_raw){.sec = at / 1000000000, .nsec = at % 1000000000};
    if (syscall4(SYS_CLOCK_NANOSLEEP, CLOCK_MONOTONIC, TIMER_ABSTIME, (long)&abs_req, 0) == 0 &&
        clock_ns(CLOCK_MONOTONIC) >= at) {
        passed |= 128u;
    }
    return passed;
}

void user_main(void) {
    struct sigaction_raw act = {.handler = (uint64_t)(uintptr_t)on_usr1, .flags = 0, .mask = 0};
    struct timespec_raw rem = {.sec = -1, .nsec = -1};
    uint32_t passed = 0;
    pid = syscall4(SYS_GETPID, 0, 0, 0, 0);
    (void)syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&act, 0, 8);
    (void)clone_thread(CLONE_THREAD_FLAGS, child_stack + sizeof child_stack, sender);
    passed |= sleep_ms(5000, &rem) == -EINTR ? 1u : 0u;
    passed |= rem.sec == 4 && rem.nsec >= 0 && rem.nsec < 1000000000 ? 2u : 0u;
    passed |= handled == 1 ? 4u : 0u;
    passed |= sleep_ms(5000, 0) == -EINTR && handled == 2 ? 8u : 0u;
    rem = (struct timespec_raw){.sec = -1, .nsec = -1};
    passed |= sleep_ms(50, &rem) == 0 && rem.sec == -1 ? 16u : 0u;
    passed |= clocks();
    (void)syscall4(SYS_EXIT_GROUP, passed, 0, 0, 0);
}
