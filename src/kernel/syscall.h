/* syscall — the table, and what an unimplemented call does.
 *
 * RISC-V uses the **generic** Linux syscall numbers (`asm-generic/unistd`),
 * which is the modern set: one `mmap` taking byte offsets, one `lseek`, one
 * `fcntl`, one `stat`, one `futex`. The architecture this kernel migrated
 * from had `mmap2` with page offsets, `_llseek`, `fcntl64`, a 104-byte
 * `stat64` and a `futex_time64` beside the plain one — eight legacy shapes
 * that simply do not exist here.
 *
 * A table rather than a `switch`, because there will be dozens of entries
 * and each wants a name: **an unimplemented call is logged once, by number
 * and by name where one is known**, and that log is how the next thing a
 * program needs gets found. That is exactly how the ARM kernel was brought
 * up to running Go, one line of log at a time, and it is faster than reading
 * a runtime's source.
 */
#ifndef MINIKERN_KERNEL_SYSCALL_H
#define MINIKERN_KERNEL_SYSCALL_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/trap.h"

/* The generic numbers this kernel knows about. Named here so the table and
 * the handlers cannot drift apart. */
#define SYS_GETCWD 17u
#define SYS_EVENTFD2 19u
#define SYS_EPOLL_CREATE1 20u
#define SYS_EPOLL_CTL 21u
#define SYS_EPOLL_PWAIT 22u
#define SYS_DUP 23u
#define SYS_DUP3 24u
#define SYS_FCNTL 25u
#define SYS_IOCTL 29u
#define SYS_FACCESSAT 48u
#define SYS_OPENAT 56u
#define SYS_CLOSE 57u
#define SYS_PIPE2 59u
#define SYS_LSEEK 62u
#define SYS_READ 63u
#define SYS_WRITE 64u
#define SYS_WRITEV 66u
#define SYS_PREAD64 67u
#define SYS_READLINKAT 78u
#define SYS_FSTATAT 79u
#define SYS_FSTAT 80u
#define SYS_EXIT 93u
#define SYS_EXIT_GROUP 94u
#define SYS_SET_TID_ADDRESS 96u
#define SYS_FUTEX 98u
#define SYS_SET_ROBUST_LIST 99u
#define SYS_NANOSLEEP 101u
#define SYS_CLOCK_GETTIME 113u
#define SYS_SCHED_SETSCHEDULER 119u
#define SYS_SCHED_GETSCHEDULER 120u
#define SYS_SCHED_GET_PRIORITY_MAX 125u
#define SYS_SCHED_GET_PRIORITY_MIN 126u
#define SYS_SCHED_SETAFFINITY 122u
#define SYS_SCHED_GETAFFINITY 123u
#define SYS_SETPRIORITY 140u
#define SYS_GETPRIORITY 141u
#define SYS_SCHED_YIELD 124u
#define SYS_TGKILL 131u
#define SYS_SIGALTSTACK 132u
#define SYS_RT_SIGACTION 134u
#define SYS_RT_SIGPROCMASK 135u
#define SYS_RT_SIGRETURN 139u
#define SYS_UNAME 160u
#define SYS_GETRUSAGE 165u
#define SYS_PRCTL 167u
#define SYS_CAPGET 90u
#define SYS_CAPSET 91u
#define SYS_SECCOMP 277u
#define SYS_GETPID 172u
#define SYS_GETTID 178u
#define SYS_BRK 214u
#define SYS_MUNMAP 215u
#define SYS_MREMAP 216u
/* Puts back the pre-fix behaviour of keeping pages a program discarded, so
 * that the test of the guarantee can be shown to fail without it. */
void syscall_keep_discarded_pages(bool on);

#define SYS_SWAPON 224u
#define SYS_SWAPOFF 225u
#define SYS_CLONE 220u
#define SYS_MMAP 222u
#define SYS_MPROTECT 226u
#define SYS_MADVISE 233u
#define SYS_RSEQ 293u
#define SYS_PRLIMIT64 261u
#define SYS_GETRANDOM 278u
#define SYS_RISCV_HWPROBE 258u

/* A handler sees the frame and returns what goes in `a0`. Negative values
 * are errno, as user space expects. */
typedef int64_t (*syscall_fn)(struct trapframe *tf);

struct syscall_entry {
    uint32_t nr;
    const char *name;
    syscall_fn call;
};

/* Logs every call, by thread and by name, for the first `budget` of them —
 * bounded because a runtime makes hundreds a second and an unbounded trace
 * is its own denial of service on the console. */
void syscall_set_trace(uint32_t budget);

/* Logs every sleep with the duration it asked for. Separate from the call
 * trace because a sleep happens long after any sensible call budget. */
void syscall_trace_sleeps(bool on);
bool syscall_tracing_sleeps(void);

/* Logs every call that returns an error, however late it happens. A budget
 * cannot see a child process's first call; a failure filter can. */
void syscall_trace_errors(bool on);
bool syscall_tracing_errors(void);

/* Whether `tgkill` really raises a signal. Off drops it, which costs a
 * runtime its asynchronous preemption and nothing else. */
void syscall_allow_tgkill(bool on);

/* Makes every thread report thread id 1, which is what `gettid` did before
 * it was fixed — a lever for measuring what that cost rather than arguing
 * about it. See `sys_gettid`. */
void syscall_one_tid(bool on);

/* The socket domain's own table, in `sys_socket.c`, terminated by a zero
 * entry. A table per domain is how this grows without one file becoming the
 * place every syscall lives: the dispatcher searches the domains in turn,
 * and a new domain is a new file and one line here. */
extern const struct syscall_entry sys_socket_table[];

/* The process domain's table, in `sys_proc.c`: `clone`, `execve`, `wait4`. */
extern const struct syscall_entry sys_proc_table[];

/* `capget`, `capset`, `prctl` and `seccomp` — the calls whose purpose is to
 * reduce what a process may do (`kernel/sys_priv.c`). */
extern const struct syscall_entry sys_priv_table[];

/* A cloned thread, which the process domain's `clone` calls for the
 * `CLONE_THREAD` case. */
int64_t syscall_clone_thread(struct trapframe *tf);

/* Dispatches one call. Returns true when it was handled — which includes
 * being handled by returning an error. False only for `exit`, which does
 * not return at all. */
bool syscall_dispatch(struct trapframe *tf);

/* Whether the caller should advance `sepc` past the `ecall` after this
 * call. False for `rt_sigreturn`, which has replaced `sepc` with the
 * interrupted instruction's address. */
bool syscall_steps_over_ecall(const struct trapframe *tf);

/* How many calls have been made, and how many were refused as
 * unimplemented. The second number is the one to watch while bringing a
 * runtime up. */
uint64_t syscall_count(void);
uint64_t syscall_unimplemented(void);

/* Argument accessors. The generic convention: the number in `a7`, arguments
 * in `a0`..`a5`, the result in `a0`. Named so a handler reads as the
 * syscall it implements rather than as register arithmetic. */
static inline uint64_t sysarg(const struct trapframe *tf, uint32_t n) {
    return tf->x[10u + n]; /* a0 is x10 */
}

#endif
