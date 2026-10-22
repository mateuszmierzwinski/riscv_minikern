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

/* Starts the scheduler with the caller as the first thread, whose kernel
 * stack is the one it is already on. Reports its thread id. */
void sched_init(uint64_t *tid);

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

/* Whether the last tick asked for a preemption, cleared by asking.
 * Preemption happens on the way out of a trap, not inside the interrupt
 * handler — switching from inside one resumes the other thread with the
 * interrupt still unacknowledged. */
bool sched_take_preempt_request(void);

/* Frees every thread's kernel stack. Called when a process is torn down,
 * from a stack that is not one of them. */
void sched_shutdown(void);

#endif
