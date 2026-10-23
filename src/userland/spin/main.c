/* One thread, busy, for a fixed wall-clock time — and nothing else.
 *
 * This exists to measure the preemption model rather than to compute
 * anything. The claim a dynamic-tick scheduler makes is that **a thread with
 * no competition is not interrupted**, and the only way to check it is a
 * process that does no I/O, makes no syscalls in its loop, and keeps a hart
 * busy long enough for a periodic clock to show up as a number.
 *
 * So: spin on arithmetic the compiler cannot discard, checking the clock
 * only once every few million iterations, and exit. Whatever timer
 * interrupts the kernel reports afterwards were not asked for by this
 * program — with one runnable thread there is no slice to end and no
 * deadline to wait for, so the honest answer is zero.
 *
 * Freestanding and static: a libc would make syscalls of its own.
 */
#include <stdbool.h>
#include <stdint.h>

#define SYS_CLOCK_GETTIME 113
#define SYS_WRITE 64
#define SYS_EXIT 93

#define CLOCK_MONOTONIC 1
#define CHECK_EVERY 2000000u /* iterations per batch */
#define BATCHES 1200u        /* calibrated to roughly ten seconds under QEMU */

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static int64_t syscall3(int64_t n, int64_t a, int64_t b, int64_t c) {
    register int64_t a7 __asm__("a7") = n;
    register int64_t a0 __asm__("a0") = a;
    register int64_t a1 __asm__("a1") = b;
    register int64_t a2 __asm__("a2") = c;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

/* Monotonic nanoseconds, or 0 if the clock is unavailable. */
static uint64_t now_ns(void) {
    struct timespec ts = {0, 0};
    if (syscall3(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (int64_t)(uintptr_t)&ts, 0) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void say(const char *s) {
    uint64_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    (void)syscall3(SYS_WRITE, 1, (int64_t)(uintptr_t)s, (int64_t)n);
}

/* The work itself: a sum the compiler must actually compute, because it is
 * returned. Volatile would make every iteration a memory access and measure
 * the cache instead. */
static uint64_t churn(uint64_t seed, uint32_t rounds) {
    uint64_t x = seed;
    for (uint32_t i = 0; i < rounds; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        x ^= x >> 29;
    }
    return x;
}

/* Does a **fixed** amount of work and says how long it took.
 *
 * Fixed rather than time-bounded, because the question this answers is what
 * a preemption costs: the same work run under a periodic tick and under
 * dynamic ticks differs by the preemptions the tick inflicted, and dividing
 * that difference by their number is a measurement of the whole path —
 * trap entry, the frame, the floating-point state, the kernel lock, the
 * switch, the scheduler's arming and `sret` — rather than of the bare
 * register swap a kernel-side probe can see. */
static uint64_t work_for(uint32_t batches, uint64_t *elapsed_ns) {
    uint64_t started = now_ns();
    uint64_t x = 1;
    for (uint32_t i = 0; i < batches; i++) {
        x = churn(x, CHECK_EVERY);
    }
    *elapsed_ns = now_ns() - started;
    return x;
}

/* Prints an unsigned decimal, because there is no libc here. */
static void say_num(const char *label, uint64_t n) {
    char buf[24];
    uint32_t i = sizeof buf;
    buf[--i] = '\n';
    do {
        buf[--i] = (char)('0' + (n % 10u));
        n /= 10u;
    } while (n != 0 && i > 0);
    say(label);
    (void)syscall3(SYS_WRITE, 1, (int64_t)(uintptr_t)&buf[i], (int64_t)(sizeof buf - i));
}

void user_main(void *start_state) {
    uint64_t x, elapsed = 0;
    (void)start_state;
    say("spin: one thread, fixed work, no syscalls in the loop\n");
    x = work_for(BATCHES, &elapsed);
    say_num("spin: elapsed ms ", elapsed / 1000000u);
    say(x == 0 ? "spin: done (zero)\n" : "spin: done\n");
    (void)syscall3(SYS_EXIT, 0, 0, 0);
}
