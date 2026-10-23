/* Four threads, a shared counter, and nothing else.
 *
 * A Go runtime is a hard thing to debug through: when its concurrency checks
 * fail, the evidence is a corrupted slice or a channel yielding values nobody
 * sent, and between the kernel and that symptom are a scheduler, a garbage
 * collector, a memory allocator and a signal handler. This program removes
 * all of it. It makes threads with `clone`, shares one page of memory, and
 * counts — so a wrong total points at the kernel and at almost nothing else.
 *
 * Two phases, because they fail for different reasons:
 *
 *   1. **Atomics only.** Each thread adds to a shared counter with
 *      `amoadd`. No syscalls in the loop at all. A wrong total here means
 *      the threads are not seeing the same memory — a stale translation, a
 *      page handed out twice — or that the hardware's atomics are not
 *      working, which on real silicon they are.
 *   2. **A futex mutex.** The same count, but each increment is a plain
 *      load-add-store inside a lock built on `futex`. A wrong total here,
 *      with phase 1 correct, means mutual exclusion failed: the lock's
 *      compare-and-swap, or the kernel's wait and wake.
 *
 * Freestanding and static, with the syscalls written out: this is the
 * interface under test, so it is not hidden behind anything.
 */
#include <stdbool.h>
#include <stdint.h>

#define SYS_FUTEX 98
#define SYS_CLONE 220
#define SYS_EXIT 93
#define SYS_WRITE 64
#define SYS_NANOSLEEP 101
#define SYS_MMAP 222
#define SYS_MUNMAP 215

#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20

#define SYS_RT_SIGACTION 134
#define SYS_SIGALTSTACK 132
#define SYS_TGKILL 131
#define SYS_GETPID 172
#define SYS_GETTID 178

#define SIGURG 23u
#define SA_ONSTACK 0x08000000u
#define SA_RESTART 0x10000000u
#define ALTSTACK_BYTES 32768u

#define CLONE_VM 0x00000100
#define CLONE_FS 0x00000200
#define CLONE_FILES 0x00000400
#define CLONE_SIGHAND 0x00000800
#define CLONE_THREAD 0x00010000

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

#define THREADS 12u
#define EACH 20000u
#define STACK_BYTES 65536u

static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static long syscall3(long nr, long a, long b, long c) {
    return syscall4(nr, a, b, c, 0);
}

static long syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
                     : "memory");
    return a0;
}

static void write_all(const char *s, uint32_t n) {
    (void)syscall3(SYS_WRITE, 1, (long)(uintptr_t)s, (long)n);
}

static void say(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    write_all(s, n);
}

static void say_number(uint64_t v) {
    char buf[24];
    uint32_t at = sizeof buf;
    buf[--at] = '\n';
    if (v == 0) {
        buf[--at] = '0';
    }
    while (v != 0 && at > 0) {
        buf[--at] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    write_all(buf + at, sizeof buf - at);
}

static void *map_anon(uint64_t bytes);

/* How a Go runtime preempts a goroutine: a signal to a specific thread,
 * delivered on that thread's alternate stack, whose handler does nothing but
 * return. It is constant and it lands at arbitrary instructions — including
 * inside a lock's compare-and-swap loop — which is the one thing this
 * program did not do when it passed with no signals at all.
 *
 * The handler has to exist and has to be harmless: if signal delivery and
 * return are correct, a program cannot tell how many it received. */
static volatile uint64_t signals_seen;

static void preempt_handler(int signo) {
    (void)signo;
    (void)__atomic_add_fetch(&signals_seen, 1u, __ATOMIC_RELAXED);
}

/* Each thread runs its handlers on a stack of its own, as a runtime does —
 * and for the reason a runtime does it: the stack that overflowed cannot
 * carry the frame that would report the overflow. */
static void take_an_altstack(void) {
    struct {
        uint64_t ss_sp;
        int32_t ss_flags;
        int32_t pad;
        uint64_t ss_size;
    } ss;
    void *mem = map_anon(ALTSTACK_BYTES);
    if (mem == (void *)0) {
        return;
    }
    ss.ss_sp = (uint64_t)(uintptr_t)mem;
    ss.ss_flags = 0;
    ss.pad = 0;
    ss.ss_size = ALTSTACK_BYTES;
    (void)syscall3(SYS_SIGALTSTACK, (long)(uintptr_t)&ss, 0, 0);
}

static void install_handler(void) {
    struct {
        uint64_t sa_handler;
        uint64_t sa_flags;
        uint64_t sa_mask;
    } act;
    act.sa_handler = (uint64_t)(uintptr_t)preempt_handler;
    act.sa_flags = SA_ONSTACK | SA_RESTART;
    act.sa_mask = 0;
    (void)syscall4(SYS_RT_SIGACTION, SIGURG, (long)(uintptr_t)&act, 0, 8);
}

/* The shared state. One page, so every thread is reading and writing the
 * same translations — which is the point. */
static volatile uint64_t atomic_counter;
static volatile uint64_t guarded_counter;
static volatile uint32_t lock_word;
static volatile uint32_t finished;
static volatile uint32_t tids[THREADS];
static volatile uint32_t running_count;
static volatile uint32_t stop_preempting;
static volatile uint32_t ephemeral_done;

/* Stacks for the children.
 *
 * **Mapped, not static.** They began as static arrays so that a bug in
 * `mmap` could not be what this program was measuring — and with static
 * stacks it passed, which is exactly why they are mapped now: a Go runtime
 * takes its thread stacks from `mmap`, and the difference between the two
 * programs is the search space for what is left. */
static void *stack_tops[THREADS];

static void *map_anon(uint64_t bytes) {
    long rc = syscall6(SYS_MMAP, 0, (long)bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return rc < 0 && rc > -4096 ? (void *)0 : (void *)(uintptr_t)rc;
}

/* A mutex on one word: 0 free, 1 held, 2 held and someone is waiting. The
 * shape glibc and Go both use, because it is the one a futex is for. */
static void lock_take(void) {
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&lock_word, &expected, 1u, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
        (void)__atomic_exchange_n(&lock_word, 2u, __ATOMIC_ACQUIRE);
        (void)syscall4(SYS_FUTEX, (long)(uintptr_t)&lock_word, FUTEX_WAIT, 2, 0);
        expected = 0;
        if (__atomic_compare_exchange_n(&lock_word, &expected, 2u, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
            return;
        }
    }
}

static void lock_drop(void) {
    if (__atomic_exchange_n(&lock_word, 0u, __ATOMIC_RELEASE) == 2u) {
        (void)syscall4(SYS_FUTEX, (long)(uintptr_t)&lock_word, FUTEX_WAKE, 1, 0);
    }
}

/* Maps and unmaps while the others count.
 *
 * A garbage collector does this continuously, and the counting threads do
 * not: if the address space changing underneath them is what breaks them,
 * this is what shows it. Each mapping is touched, so the pages are real and
 * not just reserved. */
static void churn(uint32_t rounds) {
    for (uint32_t i = 0; i < rounds; i++) {
        uint64_t bytes = (uint64_t)((i % 16u) + 1u) * 4096ull;
        uint8_t *p = map_anon(bytes);
        if (p == (uint8_t *)0) {
            continue;
        }
        for (uint64_t at = 0; at < bytes; at += 4096ull) {
            p[at] = (uint8_t)i;
        }
        (void)syscall3(SYS_MUNMAP, (long)(uintptr_t)p, (long)bytes, 0);
    }
}

/* A thread that does almost nothing and exits.
 *
 * A Go runtime starts and retires Ms continuously, and this program's four
 * threads ran from start to finish — so the path where a thread *ends* while
 * others are running was never exercised under load. That path has already
 * had one bug in it: a slot marked free while the thread was still standing
 * on its stack, which another hart could then hand to a new thread.
 *
 * Each one takes an alternate stack first, because that is what a real
 * thread does before it does anything else, and it is more state to get
 * wrong on the way out. */
static void ephemeral(void) {
    take_an_altstack();
    (void)__atomic_add_fetch(&ephemeral_done, 1u, __ATOMIC_RELAXED);
    (void)syscall3(SYS_EXIT, 0, 0, 0);
    for (;;) {
    }
}

/* What each child does: the two phases, then it reports itself finished. */
static void child_work(void) {
    uint32_t slot = __atomic_fetch_add(&running_count, 1u, __ATOMIC_SEQ_CST);
    /* An alternate stack and a published thread id, so the preemptor can
     * find this thread and the handler has somewhere to run — both of which
     * a Go runtime does in `minit` before the thread does any work. */
    take_an_altstack();
    if (slot < THREADS) {
        tids[slot] = (uint32_t)syscall3(SYS_GETTID, 0, 0, 0);
    }
    for (uint32_t i = 0; i < EACH; i++) {
        (void)__atomic_add_fetch(&atomic_counter, 1u, __ATOMIC_RELAXED);
    }
    churn(200u);
    for (uint32_t i = 0; i < EACH; i++) {
        lock_take();
        /* Deliberately **not** atomic: the lock is what makes it safe, so
         * the total is a statement about the lock. */
        guarded_counter = guarded_counter + 1u;
        lock_drop();
    }
    (void)__atomic_add_fetch(&finished, 1u, __ATOMIC_SEQ_CST);
    (void)syscall3(SYS_EXIT, 0, 0, 0);
}

/* In clone.S: the child enters `entry` on its own stack, never returning
 * into this function's frame. */
long clone_thread(long flags, void *stack_top, void (*entry)(void));

static void start_one(uint32_t i) {
    long flags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    void *base = map_anon(STACK_BYTES);
    if (base == (void *)0) {
        say("race: could not map a stack\n");
        (void)syscall3(SYS_EXIT, 1, 0, 0);
    }
    stack_tops[i] = base;
    (void)clone_thread(flags, (void *)((uintptr_t)base + STACK_BYTES), child_work);
}

void user_main(void *start_state) {
    uint32_t waited = 0;
    (void)start_state;
    {
        /* Written out here rather than through `say`, as the first thing the
         * program does: if this does not appear, nothing about the program's
         * own helpers is worth investigating yet. */
        static const char hello[] = "race: start\n";
        register long a0 __asm__("a0") = 1;
        register long a1 __asm__("a1") = (long)(uintptr_t)hello;
        register long a2 __asm__("a2") = (long)(sizeof hello - 1u);
        register long a7 __asm__("a7") = SYS_WRITE;
        __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    }
    struct {
        int64_t sec;
        int64_t nsec;
    } pause = {0, 2000000};
    for (uint32_t i = 0; i < THREADS; i++) {
        start_one(i);
    }
    say("race: spawned\n");
    /* The main thread does the preempting, continuously, while the others
     * count — which is what `sysmon` does in a Go runtime. */
    install_handler();
    {
        long pid = syscall3(SYS_GETPID, 0, 0, 0);
        /* Until they are done, not for a fixed number of rounds. A counted
         * loop finished long before the children had even published their
         * thread ids, so nothing was ever signalled and the test quietly
         * measured nothing — `signals delivered 0` is what gave it away. */
        uint32_t rounds = 0;
        while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) < THREADS &&
               rounds < 2000000u && stop_preempting == 0) {
            for (uint32_t i = 0; i < THREADS; i++) {
                uint32_t t = tids[i];
                if (t != 0) {
                    (void)syscall3(SYS_TGKILL, pid, (long)t, SIGURG);
                }
            }
            /* And a thread that comes and goes, every round. */
            {
                void *base = map_anon(STACK_BYTES);
                if (base != (void *)0) {
                    (void)clone_thread(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                                           CLONE_THREAD,
                                       (void *)((uintptr_t)base + STACK_BYTES), ephemeral);
                }
            }
            rounds++;
        }
    }
    /* Bounded, because a reproducer that hangs tells you nothing: a wait
     * that gives up and reports what it saw is worth more than one that is
     * still correct at the moment it is killed. */
    while (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) < THREADS && waited < 20000u) {
        (void)syscall3(SYS_NANOSLEEP, (long)(uintptr_t)&pause, 0, 0);
        waited++;
    }
    if (__atomic_load_n(&finished, __ATOMIC_SEQ_CST) < THREADS) {
        say("race: gave up waiting, finished ");
        say_number(finished);
        say("race: atomic so far ");
        say_number(atomic_counter);
        say("race: guarded so far ");
        say_number(guarded_counter);
        (void)syscall3(SYS_EXIT, 1, 0, 0);
    }
    say("race: threads ");
    say_number(THREADS);
    say("race: atomic counter ");
    say_number(atomic_counter);
    say("race: guarded counter ");
    say_number(guarded_counter);
    say("race: expected ");
    say_number((uint64_t)THREADS * EACH);
    say("race: signals delivered ");
    say_number(signals_seen);
    say("race: short-lived threads ");
    say_number(ephemeral_done);
    say(atomic_counter == (uint64_t)THREADS * EACH &&
                guarded_counter == (uint64_t)THREADS * EACH
            ? "race: PASS\n"
            : "race: FAIL\n");
    (void)syscall3(SYS_EXIT, 0, 0, 0);
}
