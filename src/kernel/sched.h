/* sched — threads, and what makes a Go program possible.
 *
 * A Go runtime is not optional about this. It starts a thread during
 * initialisation and then **waits on a futex for that thread to run**: a
 * kernel with one thread does not run Go slowly, it deadlocks before
 * `main`. That is the shape of the dependency, and it is why this exists
 * rather than being deferred until something needed concurrency.
 *
 * The model is the simple, real one:
 *
 *   - Threads share one address space. `clone` with `CLONE_VM` makes a
 *     thread, not a process; this kernel makes nothing else.
 *   - Each thread has **its own kernel stack**, because a thread blocked in
 *     the kernel has a kernel call stack that must survive while another
 *     thread runs on the same hart. One kernel stack for all threads is the
 *     design mistake this is written to avoid.
 *   - Switching is `context_switch`: the callee-saved registers and the
 *     stack pointer, which is all a C call expects to survive.
 *   - Scheduling is round-robin, preempted by the timer tick.
 *
 * It is a scheduler for one hart. SMP comes later, and the lock that will
 * be needed then is called out where it belongs rather than pretended to.
 */
#ifndef MINIKERN_KERNEL_SCHED_H
#define MINIKERN_KERNEL_SCHED_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/trap.h"
#include "core/types.h"

/* Go starts one thread per CPU it believes in, plus a few: a handful is
 * enough while `sched_getaffinity` reports one CPU, and running out is
 * reported as `EAGAIN` rather than ignored. */
#define SCHED_MAX_THREADS 16u

/* Starts the scheduler and makes the process's first thread, which enters
 * user mode at `frame`'s `sepc` with `frame`'s registers and `sp`. Reports
 * its thread id.
 *
 * The first thread is an **ordinary** thread. It used to be the stack
 * `uspace_run` was called on, which made it the only one another hart could
 * not run — and with four harts that exception had to go. */
int sched_init(const struct trapframe *frame, uint64_t sp, uint64_t *tid);

/* Makes the first thread, which unlike a cloned one keeps its `a0`. */
int sched_spawn_first(const struct trapframe *frame, uint64_t sp, uint64_t *tid);

/* One hart's scheduling loop: claim a runnable thread, run it, repeat until
 * the process is over. **Every** hart runs this, the one that started the
 * process included, which is what makes a thread runnable anywhere. */
void sched_run(void);

/* Ends the process: every thread is dropped and every hart's loop leaves.
 * Does not return when called from a thread. */
void sched_exit_process(void);

/* How many harts a process may run on: those that reached the kernel. This
 * is what `sched_getaffinity` reports, and therefore what a Go runtime's
 * `NumCPU` and `GOMAXPROCS` become. */
uint32_t sched_cpus(void);

/* How many harts are inside the scheduling loop. The boot hart waits for
 * this to reach zero before freeing anything a hart might still be standing
 * on. */
uint32_t sched_joined_harts(void);

/* Whether secondary harts should join the scheduler, and the switch the boot
 * hart sets. A hart parked before a process exists has nothing to claim. */
bool sched_wants_harts(void);
void sched_set_accepting(bool on);

/* How many thread switches a hart has made, by enumeration index. The
 * measurement that distinguishes "four harts available" from "four harts
 * used" — a hart that never switched never ran a thread. */
uint32_t sched_hart_switches(uint32_t hart_index);

/* Makes a thread that enters user mode at `frame`'s `sepc` with `frame`'s
 * registers, except `a0`, which is 0 — the child's half of `clone`'s
 * two-faced return. Reports its thread id.
 *
 * KERR_AGAIN when the table is full, which is what a runtime retries on. */
int sched_spawn(const struct trapframe *frame, uint64_t sp, uint64_t *tid);

/* Gives up the hart to the next runnable thread, and comes back when the
 * scheduler picks this one again. A no-op when nothing else can run. */
void sched_yield_now(void);

/* Blocks the caller until someone wakes `addr`, or `deadline_ns` passes
 * (0 means no deadline). Returns false when it timed out.
 *
 * The value check is the caller's: a futex is only correct if the test and
 * the sleep happen without a window between them, and on one hart with
 * interrupts still off through the syscall that window does not exist. */
bool sched_futex_wait(virt_t addr, uint64_t deadline_ns);

/* Sleeps until a deadline with nothing able to wake it early — the tick
 * ends it. For `nanosleep` and for an idle runtime's poller, both of which
 * would otherwise spin on the clock. False when there is no thread or no
 * deadline. */
bool sched_sleep_until(uint64_t deadline_ns);

/* Wakes up to `count` threads waiting on `addr`. Reports how many. */
uint32_t sched_futex_wake(virt_t addr, uint32_t count);

/* Ends the calling thread. Does not return. When it was the last one, the
 * process is over and this leaves through `uspace_exit`. */
void sched_exit_thread(int64_t status);

/* The calling thread's id, and how many threads are alive. */
uint64_t sched_current_tid(void);
uint32_t sched_live_threads(void);

/* Called from the timer interrupt. Preempts the running thread when another
 * is runnable, and wakes anything whose futex deadline has passed. */
void sched_tick(void);

/* Turns on a line of log for every thread created, blocked, woken and
 * ended. It is verbose by design — a runtime's own view of what it asked
 * for, which is the only way to see why it is waiting for something that
 * will not happen — and off unless `minikern.trace=sched` is given. */
void sched_set_trace(bool on);
bool sched_tracing(void);

/* Whether the last tick asked for a preemption, cleared by asking.
 * Preemption happens on the way out of a trap, not inside the interrupt
 * handler — switching from inside one resumes the other thread with the
 * interrupt still unacknowledged. */
bool sched_take_preempt_request(void);

/* Frees every thread's kernel stack. Called when a process is torn down,
 * from a stack that is not one of them. */
void sched_shutdown(void);

#endif
