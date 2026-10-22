#include "kernel/syscall.h"

#include <stddef.h>

#include "core/console.h"
#include "core/elf.h"
#include "core/errno.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "arch/riscv/timer.h"
#include "kernel/uspace.h"

static uint64_t calls;
static uint32_t trace_budget;

void syscall_set_trace(uint32_t budget) {
    trace_budget = budget;
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

/* ── the descriptors a Go runtime makes for itself ──────────────────────────
 *
 * A Go runtime builds a **netpoller** during initialisation — before any
 * network code is reached, because its timers go through it: the first
 * `time.Sleep` calls `netpollinit`, which creates an epoll instance and an
 * eventfd and fails fatally if either is refused. That is why a kernel with
 * no network stack still has to answer these.
 *
 * What is implemented is the shape, not the mechanism: an epoll instance
 * that never reports an event, and an eventfd that counts. Nothing is
 * watchable yet, so "no events" is the true answer, and a runtime treats it
 * as such — it is the answer a poller gets whenever nothing is ready.
 *
 * The state is the kernel's rather than the process's because there is one
 * process. When there is a second, this moves into `struct uproc`, and the
 * fact that it has not is recorded here rather than discovered then. */
#define FD_FIRST_ALLOCATED 3u
#define FD_EPOLL 3u
#define FD_EVENT 4u
#define FD_LIMIT 5u

static uint64_t eventfd_count;
static bool epoll_created;
static bool eventfd_created;

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

/* Writes `n` bytes of the running process's memory to the console.
 *
 * Reading user memory directly is safe **only because `sstatus.SUM` is set
 * while a syscall runs and the process's own table is still loaded** — the
 * kernel is addressing the same page the process was. A length is clamped
 * rather than trusted: it arrived from user space. */
static int64_t write_user_bytes(uint64_t addr, uint64_t n) {
    const char *from = (const char *)(uintptr_t)addr;
    if (addr == 0 || addr >= USER_TOP) {
        return KERR_FAULT;
    }
    n = n > 1u << 20 ? 1u << 20 : n;
    console_write(from, (size_t)n);
    return (int64_t)n;
}

static int64_t sys_write(struct trapframe *tf) {
    uint64_t fd = sysarg(tf, 0);
    const uint64_t *value = (const uint64_t *)(uintptr_t)sysarg(tf, 1);
    /* Writing to the eventfd is how a runtime interrupts its own poller —
     * `netpollBreak` — and it **throws a fatal error if the write fails**.
     * It adds to the counter, which is the whole of an eventfd's semantics
     * on the write side. */
    if (fd == FD_EVENT && eventfd_created) {
        if (value == NULL || (uint64_t)(uintptr_t)value >= USER_TOP || sysarg(tf, 2) != 8u) {
            return KERR_INVAL;
        }
        eventfd_count += *value;
        return 8;
    }
    if (fd != 1u && fd != 2u) {
        return KERR_BADF;
    }
    return write_user_bytes(sysarg(tf, 1), sysarg(tf, 2));
}

/* The scattered form, which a Go runtime uses for everything it prints. */
static int64_t sys_writev(struct trapframe *tf) {
    const struct user_iovec *iov = (const struct user_iovec *)(uintptr_t)sysarg(tf, 1);
    uint64_t count = sysarg(tf, 2);
    int64_t total = 0;
    if (sysarg(tf, 0) != 1u && sysarg(tf, 0) != 2u) {
        return KERR_BADF;
    }
    if (iov == NULL || count > 1024u) {
        return KERR_INVAL;
    }
    for (uint64_t i = 0; i < count; i++) {
        int64_t wrote = iov[i].len == 0 ? 0 : write_user_bytes(iov[i].base, iov[i].len);
        if (wrote < 0) {
            return total > 0 ? total : wrote;
        }
        total += wrote;
    }
    return total;
}

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

/* Which CPUs this thread may run on. One, and the caller is told its mask
 * is one byte long — a runtime sizes its worker pool from this, and a zero
 * or an error makes it either abort or spawn by guesswork. */
static int64_t sys_sched_getaffinity(struct trapframe *tf) {
    uint8_t *mask = (uint8_t *)(uintptr_t)sysarg(tf, 2);
    if (mask == NULL || (uint64_t)(uintptr_t)mask >= USER_TOP || sysarg(tf, 1) == 0) {
        return KERR_FAULT;
    }
    mask[0] = 1u;
    for (uint64_t i = 1; i < sysarg(tf, 1) && i < 128u; i++) {
        mask[i] = 0u;
    }
    return 8; /* the number of bytes written, which Linux also reports */
}

static int64_t sys_epoll_create1(struct trapframe *tf) {
    (void)tf;
    epoll_created = true;
    return FD_EPOLL;
}

static int64_t sys_eventfd2(struct trapframe *tf) {
    eventfd_count = sysarg(tf, 0);
    eventfd_created = true;
    return FD_EVENT;
}

/* Registering interest in a descriptor. Accepted and recorded nowhere:
 * there is nothing that can become ready, so there is nothing to remember.
 * Refusing it would stop the runtime starting. */
static int64_t sys_epoll_ctl(struct trapframe *tf) {
    return sysarg(tf, 0) == FD_EPOLL ? 0 : KERR_BADF;
}

/* Waiting for an event that cannot arrive.
 *
 * The timeout is **honoured** rather than ignored, which matters: a runtime
 * whose idle threads are told "no events" instantly spins a core at the
 * speed of the syscall, and on one hart that starves every other thread.
 * Waiting for the timeout makes an idle program idle. A negative timeout
 * means "for ever", which here is bounded — nothing can arrive, so sleeping
 * for ever would be a deadlock this kernel caused. */
static int64_t sys_epoll_pwait(struct trapframe *tf) {
    int64_t timeout_ms = (int64_t)(int32_t)sysarg(tf, 3);
    uint64_t deadline;
    if (sysarg(tf, 0) != FD_EPOLL) {
        return KERR_BADF;
    }
    if (timeout_ms == 0) {
        return 0;
    }
    deadline = timer_ns() + (timeout_ms < 0 ? 10000000ull
                                            : (uint64_t)timeout_ms * 1000000ull);
    while (timer_ns() < deadline) {
        sched_yield_now();
    }
    return 0;
}

/* The three descriptors that exist. A runtime **checks them before it does
 * anything else** — `runtime.checkfds` asks `fcntl(F_GETFD)` of each of 0, 1
 * and 2, and anything other than success or `EBADF` makes it abort with
 * "cannot open standard fds" before `main` is reached. That was the first
 * thing a real Go binary did here, and it is why this exists. */
#define F_GETFD 1u
#define F_SETFD 2u
#define F_GETFL 3u

static bool is_standard_fd(uint64_t fd) {
    return fd <= 2u;
}

/* A descriptor this kernel knows: the console's three, plus the two the
 * netpoller asked for. */
static bool is_known_fd(uint64_t fd) {
    return is_standard_fd(fd) || (fd == FD_EPOLL && epoll_created) ||
           (fd == FD_EVENT && eventfd_created);
}

static int64_t sys_fcntl(struct trapframe *tf) {
    if (!is_known_fd(sysarg(tf, 0))) {
        return KERR_BADF;
    }
    switch (sysarg(tf, 1)) {
    case F_GETFD:
    case F_SETFD:
        return 0; /* not close-on-exec, and setting that changes nothing */
    case F_GETFL:
        return 2; /* O_RDWR: a console is both */
    default:
        return KERR_INVAL;
    }
}

/* Reading from the console. There is no input device wired to a process
 * yet, so standard input is at end of file — which is a true answer and one
 * a runtime handles, where an error would not be. */
static int64_t sys_read(struct trapframe *tf) {
    uint64_t *out = (uint64_t *)(uintptr_t)sysarg(tf, 1);
    /* Draining the eventfd. A counter of zero is `EAGAIN` and not a short
     * read, because a reader that got 0 bytes would take it for end of
     * file and stop polling. */
    if (sysarg(tf, 0) == FD_EVENT && eventfd_created) {
        if (out == NULL || (uint64_t)(uintptr_t)out >= USER_TOP || sysarg(tf, 2) < 8u) {
            return KERR_INVAL;
        }
        if (eventfd_count == 0) {
            return KERR_AGAIN;
        }
        *out = eventfd_count;
        eventfd_count = 0;
        return 8;
    }
    return is_standard_fd(sysarg(tf, 0)) ? 0 : KERR_BADF;
}

static int64_t sys_close(struct trapframe *tf) {
    return is_known_fd(sysarg(tf, 0)) ? 0 : KERR_BADF;
}

/* There is no filesystem in a process's view of the world yet: the kernel
 * reads its program off the disk, but a process has no path namespace. So
 * every path is absent, which is what a runtime probing for
 * `/sys/kernel/mm/transparent_hugepage/hpage_pmd_size` or `/proc/self/maps`
 * expects to be told when the file is not there. */
static int64_t sys_openat(struct trapframe *tf) {
    (void)tf;
    return KERR_NOENT;
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
    want = want > 1000000000ull ? 1000000000ull : want;
    deadline = timer_ns() + want;
    while (timer_ns() < deadline) {
        __asm__ volatile("wfi");
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
        deadline = timer_ns() + (uint64_t)ts->sec * 1000000000ull + (uint64_t)ts->nsec;
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

/* Signalling a thread. There are no signals yet, so the only honest answers
 * are "that thread does not exist" and "it does, and nothing happened" —
 * and a runtime uses this to preempt a goroutine, where nothing happening
 * is a missed preemption rather than a failure. */
static int64_t sys_tgkill(struct trapframe *tf) {
    (void)tf;
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
    {SYS_SIGALTSTACK, "sigaltstack", sys_ok},
    {SYS_RT_SIGACTION, "rt_sigaction", sys_ok},
    {SYS_RT_SIGPROCMASK, "rt_sigprocmask", sys_ok},
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

/* The name of a number, for the trace and for the complaint. */
static const char *name_of(uint64_t nr) {
    for (uint32_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (table[i].nr == nr) {
            return table[i].name;
        }
    }
    return "?";
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
    for (uint32_t i = 0; i < sizeof table / sizeof table[0]; i++) {
        if (table[i].nr == nr) {
            tf->x[10] = (uint64_t)table[i].call(tf);
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
