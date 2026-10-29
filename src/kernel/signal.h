/* signal — delivering one, and coming back from it.
 *
 * What this is for: a Go runtime turns a **fault into a recoverable panic**.
 * A nil pointer dereference is a page fault, Linux delivers SIGSEGV, and the
 * runtime's handler reads the faulting address out of the signal frame and
 * raises a Go panic that `recover` can catch. Without signals the same
 * program dies, and the difference is visible in ordinary code — any `if err
 * := recover()` around a nil map or pointer.
 *
 * It is also how a runtime preempts a goroutine that is not cooperating
 * (`SIGURG` through `tgkill`), which is why both exist here.
 *
 * The hard part is not the table of handlers, it is the **return path**. A
 * handler is an ordinary function: it returns. On this architecture a
 * runtime sets the `SA_RESTORER` flag but leaves `sa_restorer` zero — only
 * x86 supplies one — so the address to return through is the kernel's to
 * provide. It maps a page of its own containing `rt_sigreturn` and points
 * `ra` at it, which is what Linux does through its VDSO.
 */
#ifndef MINIKERN_KERNEL_SIGNAL_H
#define MINIKERN_KERNEL_SIGNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/trap.h"
#include "core/types.h"

/* 65, not 64: the signals are numbered **1 to 64**, so a table indexed by
 * number needs 65 entries and a bound of `< 64` rejects signal 64 — which a
 * runtime sets a handler for along with all the others, and which then
 * failed with EINVAL in the middle of a child's setup. The unused entry 0 is
 * the price of indexing by the number the ABI uses. */
#define SIG_COUNT 65u

/* The signals this kernel raises or is asked about by name. */
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
/* The signals that act on the **process** rather than on a handler.
 *
 * `SIGSTOP` and `SIGCONT` cannot be caught, blocked or ignored, which is why
 * they are the scheduler's business and not the signal frame's. The other
 * three stop a process *by default* and can be caught — a shell catches
 * `SIGTSTP` so that it can put the job in its own list before stopping it —
 * so "what does this signal do?" is a question about the signal **and** the
 * process, and `uspace_deliver_signal` is the one place that asks it. */
#define SIGSTOP 19
#define SIGCONT 18
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGSEGV 11
/* **The signal a refused syscall raises.** `seccomp`'s `TRAP` action
 * delivers it, which is what lets a runtime handle a call the filter
 * refuses rather than merely dying of it. */
#define SIGSYS 31
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGURG 23

/* `sa_handler`'s two reserved values. */
#define SIG_DFL 0ull
#define SIG_IGN 1ull

/* `sa_flags` bits that change what delivery does. */
#define SA_SIGINFO 0x00000004u
#define SA_ONSTACK 0x08000000u
#define SA_RESTART 0x10000000u
#define SA_NODEFER 0x40000000u

/* What `rt_sigaction` is given. `sa_restorer` is **not** part of this
 * architecture's structure — a runtime that appends one is writing a word
 * the kernel does not read, and this kernel must not read it either: a
 * caller is entitled to pass a three-word structure. */
struct user_sigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_mask;
};

/* `sigaltstack`'s argument: where a handler should run instead of the
 * interrupted stack. A runtime sets one per thread, because a stack overflow
 * must be reportable and the overflowing stack cannot carry the frame. */
struct user_sigaltstack {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t pad;
    uint64_t ss_size;
};

#define SS_ONSTACK 1
#define SS_DISABLE 2

/* Turns delivery on or off. Off makes every raised signal pending for ever
 * and nothing enter a handler, which is how a run can be compared with and
 * without signals when something else is suspected. */
void signal_set_delivery(bool on);

/* Forgets every handler, for a process starting. */
void signal_reset(void);

/* `rt_sigaction`: install a handler, and report the one replaced. Either
 * pointer may be NULL. KERR_INVAL for a signal number out of range or an
 * attempt to catch SIGKILL. */
int signal_action(uint32_t signo, const struct user_sigaction *in, struct user_sigaction *out);

/* `rt_sigprocmask`'s three operations, by the ABI's numbers. */
#define SIG_BLOCK 0u
#define SIG_UNBLOCK 1u
#define SIG_SETMASK 2u

int signal_mask(uint32_t how, const uint64_t *set, uint64_t *old);

/* `sigaltstack`. */
int signal_altstack(const struct user_sigaltstack *in, struct user_sigaltstack *out);

/* The same, told the caller's stack pointer, so it can refuse a change made
 * while running on the stack being changed — the frame that will be returned
 * through is on it. */
int signal_altstack_at(virt_t current_sp, const struct user_sigaltstack *in,
                       struct user_sigaltstack *out);

/* Raises a signal against the process. It is delivered on the way back to
 * user mode, not here — a signal raised from inside a syscall must not
 * rewrite the frame the syscall is still using. */
void signal_raise(uint32_t signo, virt_t fault_address, int32_t code);

/* Raises a signal that came from a **synchronous fault**, which cannot be
 * ignored away: if it is blocked, ignored, or arrives while its own handler
 * is running, the process ends. Anything else retries the faulting
 * instruction for ever. */
void signal_raise_fault(uint32_t signo, virt_t fault_address, int32_t code);

/* The same, against a named thread rather than the running one — for
 * `tgkill`, which says which thread it means, and means it: a runtime
 * preempting a specific goroutine has chosen the thread on purpose. */
struct thread_signals;
struct uproc;

/* Raises a signal on the process: it waits in the process's pending set
 * until a thread with it unblocked takes it. */
void signal_raise_shared(struct uproc *p, uint32_t signo);

/* Whether `who`, a thread of `p`, could take signal `signo` now: it does
 * not block it. */
bool signal_unblocked_in(const struct thread_signals *who, uint32_t signo);

/* The signals waiting for `who` that it does not block: its own and the
 * process's. */
uint64_t signal_deliverable_for(const struct thread_signals *who, const struct uproc *p);

void signal_raise_on(struct thread_signals *who, uint32_t signo, virt_t fault_address,
                     int32_t code);

/* Delivers whatever is pending by rewriting `tf` to enter the handler.
 *
 * Returns true when it rewrote the frame. False means nothing was pending,
 * or the pending signal's disposition is to terminate — which the caller
 * must then do, because a default-action signal is the end of the process
 * and that is not this module's decision to carry out. */
bool signal_deliver(struct trapframe *tf);

/* Whether a signal is pending whose action is to kill the process, and
 * which one. For the caller that has to end it. */
bool signal_is_fatal(uint32_t *signo);

/* `rt_sigreturn`: restore the frame a handler was entered with. Does not
 * return a value to user space — it replaces the whole register set. */
int signal_return(struct trapframe *tf);

/* The trampoline's instructions, and how many bytes of them: `li a7, 139`
 * then `ecall`, which is `rt_sigreturn`. Exposed so the address space
 * builder can copy them in without this module knowing how pages work. */
const uint8_t *signal_trampoline_code(uint32_t *bytes);

/* How many signals have been delivered, for a report. */
uint64_t signal_delivered(void);

/* Which signal was delivered most often, and how often. An aggregate count
 * cannot tell a busy program from a kernel re-delivering one signal for
 * ever, and the failing runs this is for are the second kind. */
void signal_busiest(uint32_t *signo, uint64_t *count);

/* How many times a handler's frame was found overwritten on its way back —
 * which means something else wrote over the stack the frame was on. It
 * should be zero; anything else is a frame placed where it could be
 * clobbered. */
uint64_t signal_clobbered_frames(void);

#endif
