/* `seccomp`: a process narrowing its own syscall surface, permanently.
 *
 * **The point is that it cannot be undone.** A process installs a filter
 * and from then on every syscall it makes is checked against it, including
 * the syscall that would remove the filter — so a program can confine
 * itself before parsing something it does not trust, and a bug in the
 * parser then has a much smaller kernel to reach. That is only true if
 * installing is one-way, which is why there is no "clear" operation here
 * and why the mode never goes back to disabled.
 *
 * Two modes, and both are the ABI's rather than invented:
 *
 * - **strict** permits `read`, `write`, `exit` and `rt_sigreturn`, and
 *   kills the process for anything else. It is small, and almost never
 *   what anybody wants, because a real sandbox needs to allow a syscall
 *   *with some arguments and not others*.
 * - **filter** runs a classic-BPF program the process supplied over a
 *   64-byte description of the call. That is the mode `libseccomp` emits
 *   and the reason an interpreter exists here (`core/bpf.h`).
 *
 * **Filters stack and the most restrictive answer wins.** A process may
 * install several; every one runs on every call and the lowest return value
 * is taken, which works because the `SECCOMP_RET_*` constants are numbered
 * so that lower is stricter. A child inherits them at `fork` and keeps them
 * across `execve` — a sandbox that a program could escape by executing
 * something else would not be one.
 *
 * **Why an unprivileged process may do this at all.** Installing a filter
 * is a way to make a syscall fail, and a setuid program that depended on a
 * syscall failing could be confused into granting privilege it should not.
 * Linux's answer, and this one, is that an unprivileged process must first
 * set `no_new_privs`, which makes `execve` unable to raise privilege at all
 * — so there is no setuid program left to confuse. A process with
 * `CAP_SYS_ADMIN` is trusted to know what it is doing and is exempt.
 */
#ifndef KERNEL_SECCOMP_H
#define KERNEL_SECCOMP_H

#include "arch/riscv/trap.h"
#include "core/bpf.h"
#include <stdbool.h>
#include <stdint.h>

struct uproc;

#define SECCOMP_MODE_DISABLED 0u
#define SECCOMP_MODE_STRICT   1u
#define SECCOMP_MODE_FILTER   2u

/* The operations `seccomp(2)` takes. */
#define SECCOMP_SET_MODE_STRICT   0u
#define SECCOMP_SET_MODE_FILTER   1u
#define SECCOMP_GET_ACTION_AVAIL  2u

#define SECCOMP_FILTER_FLAG_TSYNC      (1u << 0)
#define SECCOMP_FILTER_FLAG_LOG        (1u << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW (1u << 2)

/* The actions a filter may return. **Numerically ordered by strictness**,
 * which is what makes "the most restrictive of several filters" a minimum
 * rather than a table. */
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_KILL_THREAD  0x00000000u
#define SECCOMP_RET_TRAP         0x00030000u
#define SECCOMP_RET_ERRNO        0x00050000u
#define SECCOMP_RET_USER_NOTIF   0x7fc00000u
#define SECCOMP_RET_TRACE        0x7ff00000u
#define SECCOMP_RET_LOG          0x7ffc0000u
#define SECCOMP_RET_ALLOW        0x7fff0000u

#define SECCOMP_RET_ACTION_FULL 0xffff0000u
#define SECCOMP_RET_DATA        0x0000ffffu

/* The architecture a filter sees in `seccomp_data.arch`. A filter's first
 * instruction is conventionally a check on this, so that a program built
 * for one architecture refuses to run under another whose syscall numbers
 * mean different things — which is a real escape and not a formality. */
#define SECCOMP_AUDIT_ARCH_RISCV64 0xc00000f3u

/* How many filters one process may stack. Linux bounds this by a memory
 * limit instead; a fixed small number is the same decision made visibly,
 * and eight is more than any real sandbox installs. */
#define SECCOMP_MAX_FILTERS 8u

/* What a process carries. Deliberately a plain struct inside `uproc`
 * rather than a shared, reference-counted object: filters are copied at
 * `fork`, which costs memory that a sharing scheme would save, and buys
 * the absence of a refcount that every exit path has to get right. */
struct seccomp_state {
    uint32_t mode;
    bool no_new_privs;
    bool log_all;
    uint32_t count;
    struct bpf_insn *prog[SECCOMP_MAX_FILTERS];
    uint32_t insns[SECCOMP_MAX_FILTERS];
};

/* Turns on strict mode. */
int seccomp_set_strict(struct uproc *p);

/* Adds a filter, which must already be in kernel memory and verified. The
 * instructions are copied, so the caller's buffer is its own afterwards. */
int seccomp_add_filter(struct uproc *p, const struct bpf_insn *prog, uint32_t count, bool log);

/* The action for one call: `SECCOMP_RET_ALLOW` when nothing objects.
 *
 * `args` is the six syscall arguments and `pc` the instruction that made
 * the call — both go into the structure the filter reads, because a filter
 * that could not see its arguments could only ever allow or deny a syscall
 * outright, which is the thing strict mode already does badly. */
uint32_t seccomp_decide(const struct uproc *p, uint64_t nr, const uint64_t *args, uint64_t pc);

/* Copies a parent's filters into a child. Returns false when the copy will
 * not fit, which must fail the `fork` rather than silently produce a child
 * running unconfined — an unconfined child of a sandboxed parent is the
 * whole failure this feature exists to prevent. */
bool seccomp_inherit(struct uproc *child, const struct uproc *parent);

/* Frees a process's filters. Called where a process is torn down. */
void seccomp_release(struct uproc *p);

/* The action for the call a trapframe describes: the dispatcher's entry
 * point, which pulls the number and the six arguments out of the frame so
 * that the dispatcher does not have to know the filter's input layout. */
uint32_t seccomp_action(const struct trapframe *tf, uint64_t nr);

/* Carries out an action other than `ALLOW`.
 *
 * Returns true when the syscall must not run — which is every action except
 * `LOG`, whose whole point is to let the call through while saying so.
 * Killing happens here rather than in the caller because the frame has to
 * be left in a state the scheduler can retire, and that is one decision
 * rather than one per call site. */
bool seccomp_apply(struct trapframe *tf, uint64_t nr, uint32_t action);

/* Whether `no_new_privs` is set, which `execve` must consult. */
bool seccomp_no_new_privs(const struct uproc *p);
void seccomp_set_no_new_privs(struct uproc *p);

#endif /* KERNEL_SECCOMP_H */
