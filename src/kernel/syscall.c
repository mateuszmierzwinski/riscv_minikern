#include "kernel/syscall.h"

#include <stddef.h>

#include "core/console.h"
#include "core/elf.h"
#include "core/errno.h"
#include "kernel/random.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "arch/riscv/timer.h"
#include "kernel/uspace.h"

static uint64_t calls;
static uint32_t trace_budget;

void syscall_set_trace(uint32_t budget) {
    trace_budget = budget;
}

/* The three calls a runtime sleeps in, logged with the duration they asked
 * for when tracing. The arguments are pointers, so the ordinary trace shows
 * an address where the interesting number is a length of time — and a sleep
 * that takes twenty times longer than asked is invisible without it. Its own
 * switch rather than the general trace's budget, because a sleep happens
 * after hundreds of other calls and a budget is long gone by then. */
static bool tracing_sleeps;

void syscall_trace_sleeps(bool on) {
    tracing_sleeps = on;
}

static void trace_sleep(const char *what, uint64_t ns) {
    if (!tracing_sleeps) {
        return;
    }
    kprintf("sleep: %s for %llu us\n", what, (unsigned long long)(ns / 1000u));
}

static uint64_t refused;

/* Which numbers have already been complained about, so that a runtime
 * polling an unimplemented call does not bury the log. 512 bits covers the
 * whole generic table. */
static uint64_t complained[8];

/* The flags `mmap` is called with. Only the ones that change what this
 * kernel does; the rest are accepted and ignored, because a runtime passes
 * flags that are advisory on Linux too. */
#define MAP_PRIVATE 0x02u
#define MAP_FIXED 0x10u
#define MAP_ANONYMOUS 0x20u

/* The clocks `clock_gettime` knows. Both come from the same tick here: this
 * kernel has no wall clock, and inventing one would be worse than a
 * monotonic count that is honest about what it is. */
#define CLOCK_REALTIME 0u
#define CLOCK_MONOTONIC 1u

/* A `struct timespec` as user space sees it: two 64-bit words, which is one
 * shape here where the 32-bit architecture had a `timespec` and a
 * `__kernel_timespec` and syscalls taking each. */
struct user_timespec {
    int64_t sec;
    int64_t nsec;
};

/* One entry of an `iovec` array, for `writev`. */
struct user_iovec {
    uint64_t base;
    uint64_t len;
};

static int64_t sys_exit(struct trapframe *tf) {
    uspace_exit((int64_t)(int32_t)sysarg(tf, 0));
    return 0; /* not reached */
}

static int64_t sys_brk(struct trapframe *tf) {
    return (int64_t)uspace_brk(uspace_current(), sysarg(tf, 0));
}

static int64_t sys_mmap(struct trapframe *tf) {
    uint64_t flags = sysarg(tf, 3);
    virt_t out = 0;
    int rc;
    if ((flags & MAP_ANONYMOUS) == 0) {
        return KERR_NODEV; /* nothing is mappable from a file yet */
    }
    rc = uspace_map_anon(uspace_current(), sysarg(tf, 0), sysarg(tf, 1), (uint32_t)sysarg(tf, 2),
                         (flags & MAP_FIXED) != 0, &out);
    return rc == KERR_OK ? (int64_t)out : rc;
}

static int64_t sys_munmap(struct trapframe *tf) {
    return uspace_unmap(uspace_current(), sysarg(tf, 0), sysarg(tf, 1));
}

static int64_t sys_mprotect(struct trapframe *tf) {
    return uspace_protect(uspace_current(), sysarg(tf, 0), sysarg(tf, 1), (uint32_t)sysarg(tf, 2));
}

static int64_t sys_clock_gettime(struct trapframe *tf) {
    struct user_timespec *out = (struct user_timespec *)(uintptr_t)sysarg(tf, 1);
    uint64_t ns = timer_ns();
    if (sysarg(tf, 0) != CLOCK_REALTIME && sysarg(tf, 0) != CLOCK_MONOTONIC) {
        return KERR_INVAL;
    }
    if (out == NULL || (uint64_t)(uintptr_t)out >= USER_TOP) {
        return KERR_FAULT;
    }
    out->sec = (int64_t)(ns / 1000000000ull);
    out->nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

static int64_t sys_getrandom(struct trapframe *tf) {
    uint8_t *to = (uint8_t *)(uintptr_t)sysarg(tf, 0);
    uint64_t n = sysarg(tf, 1);
    if (to == NULL || (uint64_t)(uintptr_t)to >= USER_TOP) {
        return KERR_FAULT;
    }
    n = n > 4096u ? 4096u : n;
    random_fill(to, (uint32_t)n);
    return (int64_t)n;
}

/* The calls whose honest answer is "yes, fine".
 *
 * A runtime asks a lot of questions whose answers do not matter to a kernel
 * with one process and no signals: block this signal, set that alternate
 * stack, advise me about this memory, name this thread. Returning success
 * is correct — the thing asked for either happened trivially or has no
 * effect that can be observed yet. Returning an error instead is what makes
 * a runtime abort during initialisation. */
static int64_t sys_ok(struct trapframe *tf) {
    (void)tf;
    return 0;
}

/* The one process. A real `getpid` needs a process table; until there is
 * one, 1 is both true and useful — it is what an init process is. */
static int64_t sys_getpid(struct trapframe *tf) {
    (void)tf;
    return 1;
}

/* Which CPUs this thread may run on.
 *
 * **Every hart that is in the scheduler**, as a bitmask with one bit per
 * hart. A runtime sizes its worker pool from this — it is where `NumCPU`
 * comes from, and `GOMAXPROCS` after it — so under-reporting here is a
 * machine that runs on one core while three sit idle, with nothing to
 * suggest anything is wrong.
 *
 * The bits are **positions, not hart ids**: a mask is a set of CPU numbers
 * 0..n-1, and this machine's hart ids are not that (the JH7110's are an S7
 * at 0 and four U74s at 1-4). Reporting hart 4 as bit 4 would describe a
 * five-CPU machine with a hole in it. */
static int64_t sys_sched_getaffinity(struct trapframe *tf) {
    uint8_t *mask = (uint8_t *)(uintptr_t)sysarg(tf, 2);
    uint64_t bytes = sysarg(tf, 1);
    uint32_t cpus = sched_cpus();
    if (mask == NULL || (uint64_t)(uintptr_t)mask >= USER_TOP || bytes == 0) {
        return KERR_FAULT;
    }
    bytes = bytes > 128u ? 128u : bytes;
    for (uint64_t i = 0; i < bytes; i++) {
        mask[i] = 0u;
    }
    for (uint32_t cpu = 0; cpu < cpus && cpu / 8u < bytes; cpu++) {
        mask[cpu / 8u] |= (uint8_t)(1u << (cpu % 8u));
    }
    return (int64_t)(bytes < 8u ? bytes : 8u);
}

/* ── descriptors ───────────────────────────────────────────────────────────
 *
 * Everything goes through the file layer now. It used to be a `switch` on
 * the descriptor number, which was honest while only the console and the
 * netpoller's two objects could be opened and stopped being honest the
 * moment a socket existed: a socket is a descriptor whose readiness changes,
 * and a number in a `switch` cannot be waited on.
 */

/* A runtime **checks descriptors 0, 1 and 2 before it does anything else** —
 * `runtime.checkfds` asks `fcntl(F_GETFD)` of each — and anything other than
 * success or EBADF makes it abort with "cannot open standard fds" before
 * `main` is reached. That was the first thing a real Go binary did here. */
#define F_DUPFD 0u
#define F_GETFD 1u
#define F_SETFD 2u
#define F_GETFL 3u
#define F_SETFL 4u

static int64_t sys_fcntl(struct trapframe *tf) {
    struct file *f = fd_get((int32_t)sysarg(tf, 0));
    if (f == NULL) {
        return KERR_BADF;
    }
    switch (sysarg(tf, 1)) {
    case F_DUPFD:
        return fd_dup((int32_t)sysarg(tf, 0));
    case F_GETFD:
        return (f->flags & O_CLOEXEC) != 0 ? 1 : 0;
    case F_SETFD:
        f->flags = (f->flags & ~O_CLOEXEC) | ((sysarg(tf, 2) & 1u) != 0 ? O_CLOEXEC : 0u);
        return 0;
    case F_GETFL:
        return (int64_t)(f->flags & (O_NONBLOCK | O_CLOEXEC)) | 2; /* O_RDWR */
    case F_SETFL:
        /* Only the status flags a caller may change. A runtime sets
         * O_NONBLOCK on every socket it opens, so this is not optional. */
        f->flags = (f->flags & ~O_NONBLOCK) | (sysarg(tf, 2) & O_NONBLOCK);
        return 0;
    default:
        return KERR_INVAL;
    }
}

static int64_t sys_read(struct trapframe *tf) {
    struct file *f = fd_get((int32_t)sysarg(tf, 0));
    void *to = (void *)(uintptr_t)sysarg(tf, 1);
    uint64_t n = sysarg(tf, 2);
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops->read == NULL) {
        return KERR_INVAL;
    }
    if (n != 0 && ((uint64_t)(uintptr_t)to == 0 || (uint64_t)(uintptr_t)to >= USER_TOP)) {
        return KERR_FAULT;
    }
    return f->ops->read(f, to, (uint32_t)(n > (1u << 20) ? (1u << 20) : n));
}

static int64_t sys_write(struct trapframe *tf) {
    struct file *f = fd_get((int32_t)sysarg(tf, 0));
    const void *from = (const void *)(uintptr_t)sysarg(tf, 1);
    uint64_t n = sysarg(tf, 2);
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops->write == NULL) {
        return KERR_INVAL;
    }
    if (n != 0 && ((uint64_t)(uintptr_t)from == 0 || (uint64_t)(uintptr_t)from >= USER_TOP)) {
        return KERR_FAULT;
    }
    return f->ops->write(f, from, (uint32_t)(n > (1u << 20) ? (1u << 20) : n));
}

/* The scattered form, which a Go runtime uses for everything it prints. */
static int64_t sys_writev(struct trapframe *tf) {
    const struct user_iovec *iov = (const struct user_iovec *)(uintptr_t)sysarg(tf, 1);
    struct file *f = fd_get((int32_t)sysarg(tf, 0));
    uint64_t count = sysarg(tf, 2);
    int64_t total = 0;
    if (f == NULL) {
        return KERR_BADF;
    }
    if (iov == NULL || (uint64_t)(uintptr_t)iov >= USER_TOP || count > 1024u) {
        return KERR_INVAL;
    }
    if (f->ops->write == NULL) {
        return KERR_INVAL;
    }
    for (uint64_t i = 0; i < count; i++) {
        int32_t wrote;
        if (iov[i].len == 0) {
            continue;
        }
        if (iov[i].base == 0 || iov[i].base >= USER_TOP) {
            return total > 0 ? total : KERR_FAULT;
        }
        wrote = f->ops->write(f, (const void *)(uintptr_t)iov[i].base, (uint32_t)iov[i].len);
        if (wrote < 0) {
            return total > 0 ? total : wrote;
        }
        total += wrote;
    }
    return total;
}

static int64_t sys_close(struct trapframe *tf) {
    return fd_close((int32_t)sysarg(tf, 0));
}

static int64_t sys_dup(struct trapframe *tf) {
    return fd_dup((int32_t)sysarg(tf, 0));
}

/* There is no path namespace in a process's view of the world: the kernel
 * reads its program off the disk, but a process has no filesystem. So every
 * path is absent, which is what a runtime probing for
 * `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size` or `/proc/self/maps`
 * expects to be told when the file is not there. */
static int64_t sys_openat(struct trapframe *tf) {
    (void)tf;
    return KERR_NOENT;
}

static int64_t sys_eventfd2(struct trapframe *tf) {
    return fdobjects_open_eventfd(sysarg(tf, 0), (uint32_t)sysarg(tf, 1) & O_NONBLOCK);
}

static int64_t sys_epoll_create1(struct trapframe *tf) {
    return fdobjects_open_epoll((uint32_t)sysarg(tf, 0) & O_CLOEXEC);
}

static int64_t sys_epoll_ctl(struct trapframe *tf) {
    const struct user_epoll_event *event =
        (const struct user_epoll_event *)(uintptr_t)sysarg(tf, 3);
    if (event != NULL && (uint64_t)(uintptr_t)event >= USER_TOP) {
        return KERR_FAULT;
    }
    return fdobjects_epoll_ctl((int32_t)sysarg(tf, 0), (int32_t)sysarg(tf, 1),
                               (int32_t)sysarg(tf, 2), event);
}

/* Waiting for readiness, properly: what is ready is reported, and when
 * nothing is the caller sleeps until the timeout **or until something
 * changes**. Both halves matter. Reporting nothing instantly spins a core at
 * the speed of the syscall; sleeping through a change makes every event as
 * late as the timeout. */
static int64_t sys_epoll_pwait(struct trapframe *tf) {
    struct user_epoll_event *out = (struct user_epoll_event *)(uintptr_t)sysarg(tf, 1);
    uint32_t max = (uint32_t)sysarg(tf, 2);
    int64_t timeout_ms = (int64_t)(int32_t)sysarg(tf, 3);
    int32_t epfd = (int32_t)sysarg(tf, 0);
    uint64_t deadline;
    int found;
    if (out == NULL || (uint64_t)(uintptr_t)out >= USER_TOP || max == 0) {
        return KERR_FAULT;
    }
    max = max > 64u ? 64u : max;
    found = fdobjects_epoll_collect(epfd, out, max);
    if (found != 0 || timeout_ms == 0) {
        return found;
    }
    /* A negative timeout means "for ever", which here is bounded: with
     * nothing able to arrive from outside, sleeping for ever would be a
     * deadlock this kernel caused. */
    trace_sleep(timeout_ms < 0 ? "epoll_pwait (indefinite)" : "epoll_pwait",
                timeout_ms < 0 ? 10000000ull : (uint64_t)timeout_ms * 1000000ull);
    deadline = timer_ns() + (timeout_ms < 0 ? 10000000ull : (uint64_t)timeout_ms * 1000000ull);
    /* Waiting on a **key**, so an eventfd write — a runtime interrupting its
     * own poller — ends it early. */
    (void)sched_futex_wait(SCHED_POLLER_KEY, deadline);
    return fdobjects_epoll_collect(epfd, out, max);
}

/* `riscv_hwprobe` — the architecture's own feature query, and something the
 * ARM project had no counterpart for.
 *
 * A runtime asks which extensions the hart has, so that it can pick between
 * code paths. Answering **zero for every key** is the conservative truth
 * here: it says "none of the optional extensions", and a runtime then uses
 * its baseline paths, which is what this kernel wants anyway. Claiming an
 * extension that is not there would make it emit instructions that trap.
 *
 * A value array is pairs of {key, value}; the key is echoed and the value
 * zeroed, which is the "unknown key" convention the interface defines. */
#define HWPROBE_KEY_MVENDORID 0u
#define HWPROBE_KEY_MARCHID 1u
#define HWPROBE_KEY_MIMPID 2u

struct user_hwprobe {
    int64_t key;
    uint64_t value;
};

static int64_t sys_riscv_hwprobe(struct trapframe *tf) {
    struct user_hwprobe *pairs = (struct user_hwprobe *)(uintptr_t)sysarg(tf, 0);
    uint64_t count = sysarg(tf, 1);
    if (pairs == NULL || (uint64_t)(uintptr_t)pairs >= USER_TOP || count > 64u) {
        return KERR_FAULT;
    }
    for (uint64_t i = 0; i < count; i++) {
        /* A negative key is how the interface says "I do not know this
         * one", and it is distinguishable from a key whose value is 0. */
        pairs[i].key = pairs[i].key <= (int64_t)HWPROBE_KEY_MIMPID ? pairs[i].key : -1;
        pairs[i].value = 0;
    }
    return 0;
}

/* Sleeping, without a scheduler to sleep against.
 *
 * This **spins on the timer** until the deadline, which is honest about what
 * it is: there is no other thread to run yet, so a yielding sleep and a
 * spinning one are the same thing from outside. It becomes a real sleep when
 * the scheduler arrives, and this is the call that will prove it.
 *
 * It is bounded at a second regardless of what was asked, because a runtime
 * that sleeps forever on a kernel with no way to wake it is a hang with no
 * diagnosis, and a bounded sleep that returns early is something a caller
 * must already handle — `nanosleep` returns `EINTR` with time remaining. */
static int64_t sys_nanosleep(struct trapframe *tf) {
    const struct user_timespec *req = (const struct user_timespec *)(uintptr_t)sysarg(tf, 0);
    uint64_t want, deadline;
    if (req == NULL || (uint64_t)(uintptr_t)req >= USER_TOP) {
        return KERR_FAULT;
    }
    if (req->sec < 0 || req->nsec < 0 || req->nsec >= 1000000000) {
        return KERR_INVAL;
    }
    want = (uint64_t)req->sec * 1000000000ull + (uint64_t)req->nsec;
    trace_sleep("nanosleep", want);
    want = want > 1000000000ull ? 1000000000ull : want;
    deadline = timer_ns() + want;
    if (!sched_sleep_until(deadline)) {
        /* No thread to block — before the scheduler is running. Then the
         * clock is the only thing to wait on. */
        while (timer_ns() < deadline) {
            __asm__ volatile("wfi");
        }
    }
    return 0;
}

/* `clone`'s flags. Only what distinguishes a thread from a process is
 * checked: this kernel makes threads, and a request for anything else is
 * refused rather than quietly given a thread instead — a caller that asked
 * for a separate address space and got a shared one would corrupt memory
 * long after the call succeeded. */
#define CLONE_VM 0x00000100u
#define CLONE_THREAD 0x00010000u

static int64_t sys_clone(struct trapframe *tf) {
    uint64_t flags = sysarg(tf, 0), stack = sysarg(tf, 1);
    uint64_t tid = 0;
    struct trapframe child = *tf;
    int rc;
    if ((flags & CLONE_VM) == 0 || (flags & CLONE_THREAD) == 0) {
        return KERR_NOSYS; /* fork is not implemented; a thread is */
    }
    if (stack == 0 || stack >= USER_TOP) {
        return KERR_INVAL;
    }
    /* The child resumes **after** the `ecall`, which the dispatcher has not
     * stepped over yet — so the child's program counter is computed here
     * rather than inherited. Getting this wrong makes the child re-execute
     * the clone, which forks threads until the table is full. */
    child.sepc = tf->sepc + 4u;
    rc = sched_spawn(&child, stack, &tid);
    return rc == KERR_OK ? (int64_t)tid : rc;
}

/* `futex` — the operation a Go runtime blocks its threads on.
 *
 * Only WAIT and WAKE, with or without the private flag, because those are
 * what a runtime uses to implement everything else. The value check is here
 * rather than in the scheduler: the whole correctness of a futex is that
 * the test and the sleep happen with no window between them, and on one
 * hart inside a syscall there is none. */
#define FUTEX_WAIT 0u
#define FUTEX_WAKE 1u
#define FUTEX_PRIVATE_FLAG 128u
#define FUTEX_CLOCK_REALTIME 256u

static int64_t sys_futex(struct trapframe *tf) {
    const volatile uint32_t *word = (const volatile uint32_t *)(uintptr_t)sysarg(tf, 0);
    uint64_t op = sysarg(tf, 1) & ~(uint64_t)(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    const struct user_timespec *ts = (const struct user_timespec *)(uintptr_t)sysarg(tf, 3);
    uint64_t deadline = 0;
    if (word == NULL || (uint64_t)(uintptr_t)word >= USER_TOP) {
        return KERR_FAULT;
    }
    if (op == FUTEX_WAKE) {
        /* The count is the **third** argument — the same slot that WAIT
         * uses for the value it expects. That overlap is the whole trap: a
         * wake read from the wrong slot finds 0 there, wakes nobody, and
         * reports success, so a runtime's `notewakeup` returns happily
         * while the thread it meant to start sleeps for ever. It cost a
         * deadlock to find, and only a trace showing a wake and a wait on
         * the same address with nothing in between could show it. */
        return sched_futex_wake(sysarg(tf, 0), (uint32_t)sysarg(tf, 2));
    }
    if (op != FUTEX_WAIT) {
        return KERR_NOSYS;
    }
    /* The value changed before the sleep, so there is nothing to wait for.
     * This is not an optimisation: a futex that slept here would miss the
     * wake that already happened. */
    if (*word != (uint32_t)sysarg(tf, 2)) {
        return KERR_AGAIN;
    }
    if (ts != NULL && (uint64_t)(uintptr_t)ts < USER_TOP) {
        uint64_t ns = (uint64_t)ts->sec * 1000000000ull + (uint64_t)ts->nsec;
        trace_sleep("futex", ns);
        deadline = timer_ns() + ns;
    }
    return sched_futex_wait(sysarg(tf, 0), deadline) ? 0 : KERR_AGAIN;
}

/* One thread ending, as against the whole program. A Go runtime's worker
 * threads end with `exit`; `exit_group` is what `main` returning does, and
 * conflating the two kills the program the first time a worker finishes. */
static int64_t sys_exit_thread(struct trapframe *tf) {
    sched_exit_thread((int64_t)(int32_t)sysarg(tf, 0));
    return 0; /* not reached */
}

static int64_t sys_sched_yield(struct trapframe *tf) {
    (void)tf;
    sched_yield_now();
    return 0;
}

/* Signalling a thread. A runtime uses it to preempt a goroutine that is not
 * cooperating, by sending SIGURG to the thread running it. */
/* Whether a runtime may signal its own threads (`minikern.nopreempt` turns
 * it off). Async preemption is the one signal that arrives at an arbitrary
 * instruction in ordinary code, so when something corrupts user data under
 * load it is the first thing worth subtracting. */
static bool tgkill_allowed = true;

void syscall_allow_tgkill(bool on) {
    tgkill_allowed = on;
}

static int64_t sys_tgkill(struct trapframe *tf) {
    uint64_t tid = sysarg(tf, 1);
    uint32_t signo = (uint32_t)sysarg(tf, 2);
    struct thread_signals *target = sched_signals_of(tid);
    if (!tgkill_allowed) {
        return 0; /* accepted and dropped: a missed preemption, not an error */
    }
    if (signo == 0) {
        return target != NULL ? 0 : KERR_SRCH; /* the "does it exist?" probe */
    }
    if (target == NULL) {
        return KERR_SRCH;
    }
    /* **The named thread**, not the process. A runtime preempting a
     * goroutine has chosen which thread to interrupt, and delivering to a
     * different one preempts the wrong goroutine. */
    signal_raise_on(target, signo, 0, 0);
    return 0;
}

/* Resource limits. A runtime reads the stack limit during initialisation to
 * decide how much of its own stack it may use, so an error here is not
 * ignorable — and a *small* answer would make it refuse to start threads.
 *
 * The honest numbers are this kernel's own: a thread's stack is what it was
 * given, and nothing here enforces a limit on anything, so every limit is
 * reported as unlimited except the stack, whose real value is known. */
#define RLIMIT_STACK 3u
#define RLIM_INFINITY 0xFFFFFFFFFFFFFFFFull

struct user_rlimit {
    uint64_t soft;
    uint64_t hard;
};

static int64_t sys_prlimit64(struct trapframe *tf) {
    struct user_rlimit *out = (struct user_rlimit *)(uintptr_t)sysarg(tf, 3);
    if (sysarg(tf, 2) != 0) {
        return KERR_PERM; /* setting a limit is refused, not pretended */
    }
    if (out == NULL || (uint64_t)(uintptr_t)out >= USER_TOP) {
        return KERR_FAULT;
    }
    out->soft = sysarg(tf, 1) == RLIMIT_STACK ? USER_STACK_PAGES * PAGE_SIZE : RLIM_INFINITY;
    out->hard = out->soft;
    return 0;
}

/* ── signals ───────────────────────────────────────────────────────────────
 *
 * These were answered with "yes, fine" until now, which was the right
 * answer while nothing could be delivered: a runtime that is refused
 * `rt_sigaction` aborts during initialisation. Now that delivery works they
 * do what they say, and the one thing that must not be got wrong is the
 * **size argument**: `rt_sigaction` and `rt_sigprocmask` both take the size
 * of a signal set, and a caller passing 8 for a 64-bit set is agreeing about
 * the layout. Anything else is a disagreement, not a detail. */
#define SIGSET_BYTES 8u

static int64_t sys_rt_sigaction(struct trapframe *tf) {
    const struct user_sigaction *in = (const struct user_sigaction *)(uintptr_t)sysarg(tf, 1);
    struct user_sigaction *out = (struct user_sigaction *)(uintptr_t)sysarg(tf, 2);
    if (sysarg(tf, 3) != SIGSET_BYTES) {
        return KERR_INVAL;
    }
    if ((uint64_t)(uintptr_t)in >= USER_TOP || (uint64_t)(uintptr_t)out >= USER_TOP) {
        return KERR_FAULT;
    }
    return signal_action((uint32_t)sysarg(tf, 0), in, out);
}

static int64_t sys_rt_sigprocmask(struct trapframe *tf) {
    const uint64_t *set = (const uint64_t *)(uintptr_t)sysarg(tf, 1);
    uint64_t *old = (uint64_t *)(uintptr_t)sysarg(tf, 2);
    if (sysarg(tf, 3) != SIGSET_BYTES) {
        return KERR_INVAL;
    }
    if ((uint64_t)(uintptr_t)set >= USER_TOP || (uint64_t)(uintptr_t)old >= USER_TOP) {
        return KERR_FAULT;
    }
    return signal_mask((uint32_t)sysarg(tf, 0), set, old);
}

static int64_t sys_sigaltstack(struct trapframe *tf) {
    const struct user_sigaltstack *in =
        (const struct user_sigaltstack *)(uintptr_t)sysarg(tf, 0);
    struct user_sigaltstack *out = (struct user_sigaltstack *)(uintptr_t)sysarg(tf, 1);
    if ((uint64_t)(uintptr_t)in >= USER_TOP || (uint64_t)(uintptr_t)out >= USER_TOP) {
        return KERR_FAULT;
    }
    return signal_altstack_at(tf->x[2], in, out);
}

/* Returning from a handler. It restores the whole register set, so there is
 * no result to report — and the dispatcher must **not** step `sepc` past the
 * `ecall` afterwards, because `sepc` is now the interrupted instruction's,
 * not this call's. That is what `SYSCALL_NO_STEP` is for. */
static int64_t sys_rt_sigreturn(struct trapframe *tf) {
    (void)signal_return(tf);
    return (int64_t)tf->x[10]; /* a0 as restored: the interrupted value */
}

static const struct syscall_entry table[] = {
    {SYS_WRITE, "write", sys_write},
    {SYS_WRITEV, "writev", sys_writev},
    {SYS_EXIT, "exit", sys_exit_thread},
    {SYS_EXIT_GROUP, "exit_group", sys_exit},
    {SYS_BRK, "brk", sys_brk},
    {SYS_MMAP, "mmap", sys_mmap},
    {SYS_MUNMAP, "munmap", sys_munmap},
    {SYS_MPROTECT, "mprotect", sys_mprotect},
    {SYS_MADVISE, "madvise", sys_ok},
    {SYS_CLOCK_GETTIME, "clock_gettime", sys_clock_gettime},
    {SYS_GETRANDOM, "getrandom", sys_getrandom},
    {SYS_GETPID, "getpid", sys_getpid},
    {SYS_GETTID, "gettid", sys_getpid},
    {SYS_SET_TID_ADDRESS, "set_tid_address", sys_getpid},
    {SYS_SET_ROBUST_LIST, "set_robust_list", sys_ok},
    {SYS_SIGALTSTACK, "sigaltstack", sys_sigaltstack},
    {SYS_RT_SIGACTION, "rt_sigaction", sys_rt_sigaction},
    {SYS_RT_SIGPROCMASK, "rt_sigprocmask", sys_rt_sigprocmask},
    {SYS_RT_SIGRETURN, "rt_sigreturn", sys_rt_sigreturn},
    {SYS_SCHED_GETAFFINITY, "sched_getaffinity", sys_sched_getaffinity},
    {SYS_SCHED_YIELD, "sched_yield", sys_sched_yield},
    {SYS_CLONE, "clone", sys_clone},
    {SYS_FUTEX, "futex", sys_futex},
    {SYS_TGKILL, "tgkill", sys_tgkill},
    {SYS_PRLIMIT64, "prlimit64", sys_prlimit64},
    {SYS_EPOLL_CREATE1, "epoll_create1", sys_epoll_create1},
    {SYS_EPOLL_CTL, "epoll_ctl", sys_epoll_ctl},
    {SYS_EPOLL_PWAIT, "epoll_pwait", sys_epoll_pwait},
    {SYS_EVENTFD2, "eventfd2", sys_eventfd2},
    {SYS_PRCTL, "prctl", sys_ok},
    {SYS_FCNTL, "fcntl", sys_fcntl},
    {SYS_READ, "read", sys_read},
    {SYS_CLOSE, "close", sys_close},
    {SYS_DUP, "dup", sys_dup},
    {SYS_OPENAT, "openat", sys_openat},
    {SYS_RISCV_HWPROBE, "riscv_hwprobe", sys_riscv_hwprobe},
    {SYS_NANOSLEEP, "nanosleep", sys_nanosleep},
    {SYS_RSEQ, "rseq", sys_ok},
};

/* Complains about a number once. The log line is the point of the whole
 * table: it names what to implement next, which is how the ARM kernel was
 * brought up to running Go one line at a time. */
static void complain_once(uint64_t nr) {
    uint64_t word = nr / 64u, bit = 1ull << (nr % 64u);
    refused++;
    if (nr >= 512u || (complained[word] & bit) != 0) {
        return;
    }
    complained[word] |= bit;
    kprintf("syscall: %llu is not implemented\n", (unsigned long long)nr);
}

/* Every domain's table, searched in turn. The core one first because it
 * holds what every program uses. */
/* The entry for a number, or NULL. */
static const struct syscall_entry *entry_of(uint64_t nr) {
    for (uint32_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (table[i].nr == nr) {
            return &table[i];
        }
    }
    for (const struct syscall_entry *d = sys_socket_table; d->call != NULL; d++) {
        if (d->nr == nr) {
            return d;
        }
    }
    return NULL;
}

/* The name of a number, for the trace and for the complaint. */
static const char *name_of(uint64_t nr) {
    const struct syscall_entry *e = entry_of(nr);
    return e == NULL ? "?" : e->name;
}

/* Whether the dispatcher should step `sepc` past the `ecall` afterwards.
 *
 * Everything does, except `rt_sigreturn`: it has replaced `sepc` with the
 * interrupted instruction's address, and adding 4 to that would skip an
 * instruction of the program every time a handler returned — a corruption
 * that only shows up as the program behaving slightly wrongly later. */
bool syscall_steps_over_ecall(const struct trapframe *tf) {
    return tf->x[17] != SYS_RT_SIGRETURN;
}

bool syscall_dispatch(struct trapframe *tf) {
    uint64_t nr = tf->x[17]; /* a7 */
    bool tracing = trace_budget > 0;
    calls++;
    if (tracing) {
        trace_budget--;
        kprintf("call: tid %llu %s(%llu) %llx %llx %llx\n",
                (unsigned long long)sched_current_tid(), name_of(nr), (unsigned long long)nr,
                (unsigned long long)sysarg(tf, 0), (unsigned long long)sysarg(tf, 1),
                (unsigned long long)sysarg(tf, 2));
    }
    {
        const struct syscall_entry *e = entry_of(nr);
        if (e != NULL) {
            tf->x[10] = (uint64_t)e->call(tf);
            if (tracing) {
                kprintf("call:   -> %lld\n", (long long)(int64_t)tf->x[10]);
            }
            return true;
        }
    }
    complain_once(nr);
    tf->x[10] = (uint64_t)(int64_t)KERR_NOSYS;
    return true;
}

uint64_t syscall_count(void) {
    return calls;
}

uint64_t syscall_unimplemented(void) {
    return refused;
}
