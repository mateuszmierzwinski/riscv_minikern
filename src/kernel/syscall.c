#include "kernel/syscall.h"

/* `setpriority`'s first argument. Only the process one is meaningful here:
 * there are no process groups and one user. */
#define PRIO_PROCESS 0u

/* The scheduling policies, as user space numbers them. */
#define SCHED_OTHER 0
#define SCHED_FIFO 1
#define SCHED_RR 2

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

/* **A user pointer is acceptable only if the process has it mapped.**
 *
 * Every check in this file used to be `p == 0 || p >= USER_TOP`, and on this
 * kernel that is not a check at all: `USER_TOP` is 0x4000000000 and the
 * kernel is **identity-mapped in the bottom half**, at 0x80200000 — far
 * below it. So the kernel's own addresses are "valid user addresses" by that
 * test, and a program could hand one to any syscall.
 *
 * The Rule 25 suite found two of them on its first run: `write` printed the
 * kernel's own text to the console, and `clock_gettime` wrote a timespec
 * into it. The first is kernel memory disclosure and the second is arbitrary
 * kernel memory corruption, both from an unprivileged program, and both
 * needed one line to ask for.
 *
 * `uspace_can_read` and `uspace_can_write` were already here and already
 * right: they walk the *calling process's* page tables and require `PTE_U`,
 * which the kernel's mapping does not have. One syscall was using them —
 * `sched_setaffinity`, written days ago — which is why it was the one case
 * the suite found already closed.
 *
 * `CLAUDE.md` lists the bottom-half mapping as a known cost, and records
 * that the workaround had "already cost one memory-corruption bug". This is
 * the second and third, and the lesson is narrower than the layout: a bounds
 * check against a constant is not a permission check, and only the page
 * tables know what a process may touch. */
static bool may_read(const volatile void *p, uint64_t n) {
    return uspace_can_read((virt_t)(uintptr_t)p, (uint32_t)n);
}

static bool may_write(const volatile void *p, uint64_t n) {
    return uspace_can_write((virt_t)(uintptr_t)p, (uint32_t)n);
}

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
static bool tracing_errors;

/* Logs every call that **fails**, rather than the first N calls of any kind.
 *
 * A budget is useless for anything that happens late: a child process makes
 * its first call thousands of calls into the program's life, and by then the
 * budget is long gone. Failures are rare and are what a diagnosis needs —
 * this is how the one failing syscall in a child's setup was found, where
 * the child's only visible symptom was the exit status a runtime uses for
 * "the child could not start". */
void syscall_trace_errors(bool on) {
    tracing_errors = on;
}

void syscall_trace_sleeps(bool on) {
    tracing_sleeps = on;
}

bool syscall_tracing_sleeps(void) {
    return tracing_sleeps;
}

bool syscall_tracing_errors(void) {
    return tracing_errors;
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
    if (!may_write(out, sizeof *out)) {
        return KERR_FAULT;
    }
    out->sec = (int64_t)(ns / 1000000000ull);
    out->nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

static int64_t sys_getrandom(struct trapframe *tf) {
    uint8_t *to = (uint8_t *)(uintptr_t)sysarg(tf, 0);
    uint64_t n = sysarg(tf, 1);
    if (!may_write(to, n)) {
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
/* `sched_getaffinity(pid, len, mask)`.
 *
 * Reports **this thread's** mask, which until affinity existed was "every
 * CPU the machine has" — true then and a lie afterwards. A runtime that sets
 * an affinity and reads it back has to see what it set, and one that reads
 * it without setting it has to see the harts it can actually be placed on:
 * a reserved hart is not available to it, however present it is. */
static int64_t sys_sched_getaffinity(struct trapframe *tf) {
    uint8_t *mask = (uint8_t *)(uintptr_t)sysarg(tf, 2);
    uint64_t bytes = sysarg(tf, 1);
    uint32_t cpus = sched_cpus();
    uint64_t allowed = sched_affinity();
    if (bytes == 0 || !may_write(mask, bytes > 128u ? 128u : bytes)) {
        return KERR_FAULT;
    }
    bytes = bytes > 128u ? 128u : bytes;
    for (uint64_t i = 0; i < bytes; i++) {
        mask[i] = 0u;
    }
    for (uint32_t cpu = 0; cpu < cpus && cpu / 8u < bytes; cpu++) {
        if ((allowed & (1ull << cpu)) != 0) {
            mask[cpu / 8u] |= (uint8_t)(1u << (cpu % 8u));
        }
    }
    return (int64_t)(bytes < 8u ? bytes : 8u);
}

/* `sched_setaffinity(pid, len, mask)`.
 *
 * `pid` of 0 only — another thread's affinity needs a thread table lookup
 * this deliberately does not do yet, and answering about the caller instead
 * would be worse than refusing.
 *
 * The mask is read from user memory a byte at a time through the checked
 * copy, because it is a user pointer and the kernel must not dereference
 * one: a wrong pointer here would fault in the kernel, where a fault is a
 * panic, and one bad argument from any program would stop the machine. */
static int64_t sys_sched_setaffinity(struct trapframe *tf) {
    uint64_t pid = sysarg(tf, 0);
    uint64_t bytes = sysarg(tf, 1);
    virt_t user = (virt_t)sysarg(tf, 2);
    uint64_t want = 0;
    if (pid != 0) {
        return KERR_PERM;
    }
    if (user == 0 || bytes == 0) {
        return KERR_FAULT;
    }
    bytes = bytes > 8u ? 8u : bytes;
    if (uspace_copy_in(&want, user, (uint32_t)bytes) != KERR_OK) {
        return KERR_FAULT;
    }
    return sched_set_affinity(want) ? 0 : KERR_INVAL;
}

/* `setpriority(which, who, nice)`.
 *
 * Only `PRIO_PROCESS` with `who` of 0 — "me" — is supported, which is what a
 * runtime asking for a nice value actually uses. Another process's priority
 * needs the permission model this kernel does not have (there is one
 * privilege level), so asking about one is refused rather than silently
 * answered about the caller.
 *
 * POSIX says the value is **clamped, not refused**, which `fair_clamp_nice`
 * does. A caller that asks for -30 gets -20 and a success, because the
 * alternative — an error it almost certainly does not check — would leave it
 * running at a priority it did not ask for and does not know about. */
static int64_t sys_setpriority(struct trapframe *tf) {
    uint64_t which = sysarg(tf, 0);
    uint64_t who = sysarg(tf, 1);
    int32_t nice = (int32_t)(int64_t)sysarg(tf, 2);
    if (which != PRIO_PROCESS || who != 0) {
        return KERR_INVAL;
    }
    return sched_set_nice(nice) ? 0 : KERR_INVAL;
}

/* `getpriority(which, who)`.
 *
 * Returns the nice value itself, **not** Linux's `20 - nice` encoding: that
 * bias exists so the libc wrapper can tell -1 from an error, and it is the
 * wrapper's job rather than the kernel's. A program using the raw syscall
 * gets the raw value, which is what `setpriority` took. */
static int64_t sys_getpriority(struct trapframe *tf) {
    uint64_t which = sysarg(tf, 0);
    uint64_t who = sysarg(tf, 1);
    if (which != PRIO_PROCESS || who != 0) {
        return KERR_INVAL;
    }
    return sched_nice();
}

/* `sched_setscheduler(pid, policy, param)`.
 *
 * `pid` of 0 only, as with affinity: another thread's policy needs a table
 * lookup this does not do, and answering about the caller instead would be
 * worse than refusing.
 *
 * The priority comes from a `struct sched_param` in user memory — one `int`,
 * read through the checked copy because the kernel must not dereference a
 * user pointer. `SCHED_OTHER` must carry priority 0, which is what the
 * standard says and what distinguishes "put me back in the fair class" from
 * "put me in the fair class at priority 40", a request with no meaning. */
static int64_t sys_sched_setscheduler(struct trapframe *tf) {
    uint64_t pid = sysarg(tf, 0);
    int32_t policy = (int32_t)(int64_t)sysarg(tf, 1);
    virt_t param = (virt_t)sysarg(tf, 2);
    int32_t priority = 0;
    if (pid != 0) {
        return KERR_PERM;
    }
    if (param != 0 && uspace_copy_in(&priority, param, sizeof priority) != KERR_OK) {
        return KERR_FAULT;
    }
    if (priority < 0 || (uint32_t)priority > SCHED_RT_PRIORITY_MAX) {
        return KERR_INVAL;
    }
    switch (policy) {
    case SCHED_OTHER:
        return priority == 0 && sched_set_rt(0, false) ? 0 : KERR_INVAL;
    case SCHED_FIFO:
        return priority > 0 && sched_set_rt((uint32_t)priority, false) ? 0 : KERR_INVAL;
    case SCHED_RR:
        return priority > 0 && sched_set_rt((uint32_t)priority, true) ? 0 : KERR_INVAL;
    default:
        return KERR_INVAL;
    }
}

static int64_t sys_sched_getscheduler(struct trapframe *tf) {
    if (sysarg(tf, 0) != 0) {
        return KERR_PERM;
    }
    if (sched_rt_priority() == 0) {
        return SCHED_OTHER;
    }
    return sched_rt_is_round_robin() ? SCHED_RR : SCHED_FIFO;
}

/* The priority range a policy accepts. Real-time is 1..99 and the fair class
 * is 0..0 — the range POSIX defines and every program that asks expects, so
 * these are interface facts rather than this kernel's choices. */
static int64_t sys_sched_get_priority_max(struct trapframe *tf) {
    int32_t policy = (int32_t)(int64_t)sysarg(tf, 0);
    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        return (int64_t)SCHED_RT_PRIORITY_MAX;
    }
    return policy == SCHED_OTHER ? 0 : KERR_INVAL;
}

static int64_t sys_sched_get_priority_min(struct trapframe *tf) {
    int32_t policy = (int32_t)(int64_t)sysarg(tf, 0);
    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        return 1;
    }
    return policy == SCHED_OTHER ? 0 : KERR_INVAL;
}

/* `ioctl(fd, request, arg)`.
 *
 * Dispatched to the object, and **`ENOTTY` when the object has no `ioctl`**
 * rather than 0. A program uses exactly this to find out whether its output
 * is a terminal, so answering 0 for a pipe would have every pipe claim to
 * be one — and then the program emits escape sequences into a file. */
static int64_t sys_ioctl(struct trapframe *tf) {
    struct file *f = fd_get((int32_t)sysarg(tf, 0));
    uint32_t request = (uint32_t)sysarg(tf, 1);
    virt_t arg = (virt_t)sysarg(tf, 2);
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops->ioctl == NULL) {
        return KERR_NOTTY;
    }
    return f->ops->ioctl(f, request, arg);
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
    if (n != 0 && !may_write(to, n > (1u << 20) ? (1u << 20) : n)) {
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
    if (n != 0 && !may_read(from, n > (1u << 20) ? (1u << 20) : n)) {
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
    if (count > 1024u || !may_read(iov, count * sizeof *iov)) {
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
        if (!may_read((const void *)(uintptr_t)iov[i].base, iov[i].len)) {
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

/* `dup3`: duplicate onto a chosen descriptor. This is what puts a pipe on
 * descriptor 1 before an `exec`, and the reason the call exists. Unlike
 * `dup2` it refuses `from == to`, which is how a caller can tell the two
 * apart. */
static int64_t sys_dup3(struct trapframe *tf) {
    int32_t from = (int32_t)sysarg(tf, 0), to = (int32_t)sysarg(tf, 1);
    if (from == to) {
        return KERR_INVAL;
    }
    return fd_dup_to(from, to);
}

static int64_t sys_pipe2(struct trapframe *tf) {
    int32_t fds[2] = {-1, -1};
    int rc = fdobjects_open_pipe(fds, (uint32_t)sysarg(tf, 1) & (O_NONBLOCK | O_CLOEXEC));
    if (rc != KERR_OK) {
        return rc;
    }
    if (uspace_copy_out(sysarg(tf, 0), fds, sizeof fds) != KERR_OK) {
        (void)fd_close(fds[0]);
        (void)fd_close(fds[1]);
        return KERR_FAULT;
    }
    return 0;
}

/* Opening a path. There is no filesystem in a process's view of the world,
 * so this is the short list of synthesised files in `fdobjects.c` and
 * `ENOENT` for everything else — which is the right answer for a probe and
 * one a runtime handles.
 *
 * `/dev/null` is the entry that had to exist: a runtime opens it for any
 * command whose input it was not given, so `os/exec` could not start a
 * program at all without it. */
#define OPEN_MAX_PATH 128u

static int64_t sys_openat(struct trapframe *tf) {
    char path[OPEN_MAX_PATH];
    virt_t at = sysarg(tf, 1);
    for (uint32_t i = 0; i < OPEN_MAX_PATH; i++) {
        if (uspace_copy_in(&path[i], at + i, 1) != KERR_OK) {
            return KERR_FAULT;
        }
        if (path[i] == '\0') {
            return fdobjects_open_path(path, (uint32_t)sysarg(tf, 2) & (O_NONBLOCK | O_CLOEXEC));
        }
    }
    return KERR_NAMETOOLONG;
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
    if (event != NULL && !may_read(event, sizeof *event)) {
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
    if (max == 0 || !may_write(out, (uint64_t)max * sizeof *out)) {
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
    if (count > 64u || !may_read(pairs, count * sizeof *pairs)) {
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
    if (!may_read(req, sizeof *req)) {
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

/* A cloned **thread**, which is what `CLONE_THREAD` means: everything
 * shared. `sys_proc.c` decides which kind of clone was asked for and calls
 * this for the thread case. */
int64_t syscall_clone_thread(struct trapframe *tf) {
    uint64_t flags = sysarg(tf, 0), stack = sysarg(tf, 1);
    uint64_t tid = 0;
    struct trapframe child = *tf;
    int rc;
    if ((flags & CLONE_VM) == 0 || (flags & CLONE_THREAD) == 0) {
        return KERR_NOSYS; /* fork is not implemented; a thread is */
    }
    /* **Left as a range check, and that is correct here.** The others in
     * this file became permission checks because the kernel dereferences
     * those pointers; it never touches this one. It becomes the child's
     * stack pointer, used in *user* mode — so a value pointing into the
     * kernel's identity map faults on the child's first push, because that
     * mapping has no `PTE_U`. A bad stack is the child's problem and shows
     * up as its own SIGSEGV, which is what it should be. */
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
    if (!may_write(word, sizeof(uint32_t))) {
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
    if (ts != NULL && may_read(ts, sizeof *ts)) {
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
#define RLIMIT_NPROC 6u
#define RLIMIT_NOFILE 7u
#define RLIM_INFINITY 0xFFFFFFFFFFFFFFFFull

struct user_rlimit {
    uint64_t soft;
    uint64_t hard;
};

/* `prlimit64(pid, resource, new, old)`.
 *
 * This **answered and enforced nothing** until now: every limit read as
 * infinite except the stack, and setting one was refused. A stored limit
 * that nothing consults is worse than an absent one, because a program
 * reads it and plans around a number nothing will hold it to — so the two
 * that this kernel can actually enforce are enforced, and the rest still
 * read as infinite because that is the truth about them.
 *
 * The privilege rule is POSIX's and is the whole point of a hard limit: a
 * process may lower either, may raise its **soft** limit up to the hard
 * one, and may raise the **hard** limit only as root. Without that, a limit
 * is a suggestion a program can decline.
 */
static int64_t rlimit_get(struct uproc *me, uint64_t which, struct user_rlimit *out) {
    switch (which) {
    case RLIMIT_STACK:
        out->soft = USER_STACK_PAGES * PAGE_SIZE;
        out->hard = out->soft;
        return 0;
    case RLIMIT_NOFILE:
        out->soft = me->rlim_nofile_soft == 0 ? FD_MAX : me->rlim_nofile_soft;
        out->hard = me->rlim_nofile_hard == 0 ? FD_MAX : me->rlim_nofile_hard;
        return 0;
    case RLIMIT_NPROC:
        out->soft = me->rlim_nproc_soft == 0 ? RLIM_INFINITY : me->rlim_nproc_soft;
        out->hard = me->rlim_nproc_hard == 0 ? RLIM_INFINITY : me->rlim_nproc_hard;
        return 0;
    default:
        out->soft = RLIM_INFINITY;
        out->hard = RLIM_INFINITY;
        return 0;
    }
}

static int64_t rlimit_set(struct uproc *me, uint64_t which, const struct user_rlimit *in) {
    uint64_t *soft, *hard;
    uint64_t ceiling;
    switch (which) {
    case RLIMIT_NOFILE:
        soft = &me->rlim_nofile_soft;
        hard = &me->rlim_nofile_hard;
        ceiling = FD_MAX;
        break;
    case RLIMIT_NPROC:
        soft = &me->rlim_nproc_soft;
        hard = &me->rlim_nproc_hard;
        ceiling = PROC_MAX;
        break;
    default:
        /* Not storable here, and **refused rather than accepted and
         * dropped**: a caller that is told its limit was set and finds it
         * was not has been lied to. */
        return KERR_PERM;
    }
    if (in->soft > in->hard) {
        return KERR_INVAL;
    }
    {
        uint64_t current_hard = *hard == 0 ? ceiling : *hard;
        /* Raising the hard limit is root's alone. Lowering is anybody's,
         * and is one-way for the same reason dropping a uid is. */
        if (in->hard > current_hard && uspace_euid() != 0) {
            return KERR_PERM;
        }
    }
    *soft = in->soft > ceiling ? ceiling : in->soft;
    *hard = in->hard > ceiling ? ceiling : in->hard;
    return 0;
}

static int64_t sys_prlimit64(struct trapframe *tf) {
    uint64_t which = sysarg(tf, 1);
    const struct user_rlimit *in = (const struct user_rlimit *)(uintptr_t)sysarg(tf, 2);
    struct user_rlimit *out = (struct user_rlimit *)(uintptr_t)sysarg(tf, 3);
    struct uproc *me = uspace_current();
    struct user_rlimit want;
    if (me == NULL) {
        return KERR_SRCH;
    }
    if (sysarg(tf, 0) != 0) {
        return KERR_PERM; /* another process's limits need a lookup this does not do */
    }
    /* **Read before write**, so a caller doing both in one call sees the
     * old values rather than the ones it just set — which is what the
     * interface promises and what makes the save-and-restore idiom work. */
    if (out != NULL) {
        struct user_rlimit old;
        (void)rlimit_get(me, which, &old);
        if (!may_write(out, sizeof *out) ||
            uspace_copy_out((virt_t)(uintptr_t)out, &old, sizeof old) != KERR_OK) {
            return KERR_FAULT;
        }
    }
    if (in == NULL) {
        return 0;
    }
    if (uspace_copy_in(&want, (virt_t)(uintptr_t)in, sizeof want) != KERR_OK) {
        return KERR_FAULT;
    }
    return rlimit_set(me, which, &want);
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
    if ((in != NULL && !may_read(in, sizeof *in)) ||
        (out != NULL && !may_write(out, sizeof *out))) {
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
    if ((set != NULL && !may_read(set, SIGSET_BYTES)) ||
        (old != NULL && !may_write(old, SIGSET_BYTES))) {
        return KERR_FAULT;
    }
    return signal_mask((uint32_t)sysarg(tf, 0), set, old);
}

static int64_t sys_sigaltstack(struct trapframe *tf) {
    const struct user_sigaltstack *in =
        (const struct user_sigaltstack *)(uintptr_t)sysarg(tf, 0);
    struct user_sigaltstack *out = (struct user_sigaltstack *)(uintptr_t)sysarg(tf, 1);
    if ((in != NULL && !may_read(in, sizeof *in)) ||
        (out != NULL && !may_write(out, sizeof *out))) {
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
    {29u, "ioctl", sys_ioctl},
    {SYS_SCHED_GETAFFINITY, "sched_getaffinity", sys_sched_getaffinity},
    {SYS_SCHED_SETAFFINITY, "sched_setaffinity", sys_sched_setaffinity},
    {SYS_SCHED_SETSCHEDULER, "sched_setscheduler", sys_sched_setscheduler},
    {SYS_SCHED_GETSCHEDULER, "sched_getscheduler", sys_sched_getscheduler},
    {SYS_SCHED_GET_PRIORITY_MAX, "sched_get_priority_max", sys_sched_get_priority_max},
    {SYS_SCHED_GET_PRIORITY_MIN, "sched_get_priority_min", sys_sched_get_priority_min},
    {SYS_SETPRIORITY, "setpriority", sys_setpriority},
    {SYS_GETPRIORITY, "getpriority", sys_getpriority},
    {SYS_SCHED_YIELD, "sched_yield", sys_sched_yield},
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
    {SYS_DUP3, "dup3", sys_dup3},
    {SYS_PIPE2, "pipe2", sys_pipe2},
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
    for (const struct syscall_entry *d = sys_proc_table; d->call != NULL; d++) {
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
            /* The arguments are copied **before** the call, because `a0` is
             * both the first argument and the result: printing it afterwards
             * shows the return value where the argument should be, and reads
             * as a program passing nonsense. That cost an hour of suspecting
             * the wrong thing. */
            uint64_t a0 = sysarg(tf, 0), a1 = sysarg(tf, 1), a2 = sysarg(tf, 2);
            uint64_t a3 = sysarg(tf, 3);
            tf->x[10] = (uint64_t)e->call(tf);
            if (tracing) {
                kprintf("call:   -> %lld\n", (long long)(int64_t)tf->x[10]);
            }
            if (tracing_errors && (int64_t)tf->x[10] < 0) {
                /* The pid as well as the thread, because "this descriptor
                 * is not open" and "this is the wrong process's table" look
                 * identical from a thread id alone. */
                {
                    /* What the kernel thinks the caller's descriptor table
                     * is. "This descriptor is not open" and "this is the
                     * wrong table" are the same symptom, and only the
                     * contents tell them apart. */
                    const struct uproc *who = uspace_current();
                    kprintf("fail:   proc %p, fds 0..3 = %p %p %p %p\n", (const void *)who,
                            (const void *)(who != NULL ? who->fds[0] : NULL),
                            (const void *)(who != NULL ? who->fds[1] : NULL),
                            (const void *)(who != NULL ? who->fds[2] : NULL),
                            (const void *)(who != NULL ? who->fds[3] : NULL));
                }
                kprintf("fail: pid %u tid %llu %s(%llu) %llx %llx %llx %llx -> %lld\n",
                        uspace_current() != NULL ? uspace_current()->pid : 0u,
                        (unsigned long long)sched_current_tid(), e->name, (unsigned long long)nr,
                        (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2,
                        (unsigned long long)a3, (long long)(int64_t)tf->x[10]);
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
