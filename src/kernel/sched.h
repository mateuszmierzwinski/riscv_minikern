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

/* A process, defined in `uspace.h`. Declared here rather than included,
 * because `uspace.h` needs this header's thread types: the two know about
 * each other and neither owns the other. */
struct uproc;

/* A thread's signal state.
 *
 * **Per thread, and that is not a detail.** The mask and the alternate stack
 * belong to a thread, not to a process: a runtime blocks signals around a
 * critical section on the thread that is in it, and gives each thread its
 * own handler stack. Sharing either is a bug that shows up as corrupted user
 * data rather than as anything about signals — two threads taking a signal
 * at once ran their handlers on the same stack and clobbered each other's
 * saved registers, and the symptom was a Go program panicking with
 * "WaitGroup misuse" on code that misuses nothing. It took a 36-run stress
 * loop to see it once.
 *
 * The *actions* are the process's, which is correct: a handler is installed
 * for the program, not for a thread.
 *
 * One pending fault's address rather than an array: a second fault while one
 * is pending means the process is dying anyway. */
struct thread_signals {
    uint64_t blocked;
    uint64_t pending;
    uint64_t altstack_sp;
    uint64_t altstack_size;
    int32_t altstack_flags;
    virt_t fault_address;
    int32_t fault_code;
    /* The signal number of a pending **fault**, which may not be ignored or
     * blocked away: the faulting instruction would be retried for ever. 0
     * when none is pending. */
    uint32_t forced;
};

/* Go starts one thread per CPU it believes in, plus a few: a handful is
 * enough while `sched_getaffinity` reports one CPU, and running out is
 * reported as `EAGAIN` rather than ignored. */
/* How many threads can exist at once.
 *
 * Sixteen was comfortable for a program with a handful of threads and is not
 * comfortable for a Go runtime: it starts an M per busy P, plus the garbage
 * collector's workers, plus `sysmon`, and it creates and retires them
 * freely. A `clone` that fails is not a slowdown — the runtime treats it as
 * fatal ("failed to create new OS thread") — so the table has to be sized
 * for the runtime's appetite rather than the program's.
 *
 * **Sixty-four was not enough for a userland, and the board said so in the
 * least helpful way available.** A Go runtime keeps several OS threads per
 * process — an M per busy P, plus the garbage collector's and the network
 * poller's — so QuokkaOS's init plus four Go services exhausted the table.
 * What a program saw was `fork/exec: resource temporarily unavailable`,
 * which is `EAGAIN` from `spawn`, which is indistinguishable from the three
 * other things that answer `EAGAIN` on that path. It took a kprintf in each
 * of them to find out which.
 *
 * Two hundred and fifty-six, at four pages of kernel stack each, is four
 * megabytes on a machine with eight gigabytes — and it is the difference
 * between a kernel that can host a handful of Go programs and one that
 * cannot. The table is still fixed: a limit that is reached and says which
 * limit it was beats an allocator that fails somewhere less obvious. */
#define SCHED_MAX_THREADS 256u

/* Starts the scheduler and makes the process's first thread, which enters
 * user mode at `frame`'s `sepc` with `frame`'s registers and `sp`. Reports
 * its thread id.
 *
 * The first thread is an **ordinary** thread. It used to be the stack
 * `uspace_run` was called on, which made it the only one another hart could
 * not run — and with four harts that exception had to go. */
int sched_init(struct uproc *proc, const struct trapframe *frame, uint64_t sp, uint64_t *tid);

/* Makes the first thread, which unlike a cloned one keeps its `a0`. */
int sched_spawn_first(struct uproc *proc, const struct trapframe *frame, uint64_t sp,
                      uint64_t *tid);

/* One hart's scheduling loop: claim a runnable thread, run it, repeat until
 * the process is over. **Every** hart runs this, the one that started the
 * process included, which is what makes a thread runnable anywhere. */
void sched_run(void);

/* Ends **one process**: every thread of it is dropped, and the harts' loops
 * leave only if it was the process they were started for. Does not return
 * when called from a thread of that process.
 *
 * The distinction appeared with the second process. "End the process" used
 * to mean "stop the machine", which is right when there is one and wrong the
 * moment a child exits: a child ending must leave its parent running. */
void sched_exit_proc(struct uproc *p, bool is_root);

/* Takes `p`'s table out of every hart's record of what it has loaded, and
 * out of this hart's `satp` if it is there — called before the table is
 * torn down. Another hart cannot be running on it: its threads are off
 * every hart by then, and an idle hart has already released it (see
 * `unload_before_idling`). Clearing the record matters even so, because a
 * process slot is reused, and a hart whose record still named the slot
 * would skip loading the new process's table and run it on the old one's. */
void sched_forget_address_space(const struct uproc *p);

/* How many harts a process may run on: those that reached the kernel. This
 * is what `sched_getaffinity` reports, and therefore what a Go runtime's
 * `NumCPU` and `GOMAXPROCS` become. */
uint32_t sched_cpus(void);

/* How many harts are inside the scheduling loop. The boot hart waits for
 * this to reach zero before freeing anything a hart might still be standing
 * on. */
uint32_t sched_joined_harts(void);

/* Prints which harts are still in the scheduling loop and what they are
 * running — for the one wait in this kernel that cannot be bounded, where a
 * hart that never leaves is otherwise total silence. */
void sched_report_joined(void);

/* Whether secondary harts should join the scheduler, and the switch the boot
 * hart sets. A hart parked before a process exists has nothing to claim. */
bool sched_wants_harts(void);
void sched_set_accepting(bool on);

/* The running thread's signal state, or NULL outside a thread. The signal
 * code works through this rather than keeping its own, so that a thread's
 * mask and stack stay the thread's. */
struct thread_signals *sched_current_signals(void);

/* A given thread's, by id — for `tgkill`, which names the thread it means.
 * NULL when no live thread has that id. */
struct thread_signals *sched_signals_of(uint64_t tid);

/* How many thread switches a hart has made, by enumeration index. The
 * measurement that distinguishes "four harts available" from "four harts
 * used" — a hart that never switched never ran a thread. */
uint32_t sched_hart_switches(uint32_t hart_index);

/* The process the running thread belongs to, or NULL between threads.
 *
 * This is what makes processes plural: a thread belongs to a process, a hart
 * runs a thread, so "the current process" is a property of the hart and not
 * a global. It was a global while there was one process, which is exactly
 * the kind of singleton that looks like a simplification until a second one
 * appears. */
struct uproc *sched_current_proc(void);

/* Makes a thread in a **given** process, for a child's first thread. The
 * ordinary `sched_spawn` puts the new thread in the creator's process, which
 * is what `clone` with `CLONE_THREAD` means. */
int sched_spawn_in(struct uproc *proc, const struct trapframe *frame, uint64_t sp,
                   uint64_t *tid);

/* Ends every thread of a process except the calling one, which is what
 * `execve` must do before it replaces the address space: the other threads
 * were running code that is about to disappear. Reports how many ended. */
uint32_t sched_end_siblings(const struct uproc *proc);

/* Waits until no other thread of `proc` is on a hart, and reports how many
 * still are.
 *
 * The other half of `sched_end_siblings`, and without it that function was
 * only half an answer: it marks the siblings and makes them leave, and the
 * caller used to carry straight on. What a caller does next is **replace or
 * destroy the address space** — `execve` rebuilds it, `exit_group` frees it
 * — so a sibling still executing user code on another hart was running
 * against page tables being taken apart underneath it. Nothing faults: it
 * reads and writes pages that by then belong to something else.
 *
 * Bounded, and the count it returns is the caller's to act on. A thread
 * that has been told to leave and has not is a kernel bug, and continuing
 * into the teardown would be a worse answer than saying so. */
uint32_t sched_await_siblings(const struct uproc *proc);

/* How many threads had to be waited off a hart, and the longest such wait
 * in microseconds. A non-zero count is the kernel reporting that it caught
 * a thread mid-flight — which is what turns this race's closing into a
 * measurement rather than an assertion. */
void sched_await_report(uint32_t *threads_waited, uint64_t *worst_us);

/* The three numbers behind that wait: pokes aimed at a hart to make it let
 * a thread go, threads actually woken by a departure, and times the waiter
 * looked again. Together they say which of the three mechanisms carried the
 * wait — the poke, the departure's wake, or the deadline behind both. */
void sched_await_detail(uint32_t *pokes, uint32_t *wakes, uint32_t *loops);

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

/* The key a thread waiting for descriptor readiness is blocked on.
 *
 * It must be wakeable, because a runtime **interrupts its own poller**: it
 * writes to an eventfd to say "stop waiting, something changed". A poller
 * that cannot be woken sleeps out the delay it was originally given, and the
 * runtime's timers are then as late as that delay — a correct-but-slow
 * kernel, which is the hard kind to diagnose because every individual sleep
 * is accurate.
 *
 * 1 rather than a real address: it shares the futex key space, and no user
 * futex lives at 1 because the lowest page is never mapped. */
#define SCHED_POLLER_KEY 1ull

/* ── kernel wait channels ───────────────────────────────────────────────────
 *
 * The same idea as a futex, keyed on a **kernel** pointer: a thread sleeps
 * on the address of whatever it is waiting for — a socket, a queue — and
 * whatever changes that thing wakes it.
 *
 * A separate key space from the futex addresses on purpose. A futex key is a
 * *user* address, and this kernel's user space reaches 256 GiB, which
 * includes every address a kernel pointer could have. Sharing one space
 * would let a program's futex alias a kernel object's wait channel, and a
 * spurious wake-up is the *good* outcome of that: the bad one is a wake
 * going to the wrong sleeper.
 *
 * `deadline_ns` of 0 waits with no deadline. Returns KERR_OK when woken,
 * KERR_TIMEDOUT when the deadline passed.
 */
int sched_wait_on(const void *chan, uint64_t deadline_ns);
uint32_t sched_wake_on(const void *chan, uint32_t max);

/* Sleeps until a deadline with nothing able to wake it early — the tick
 * ends it. For `nanosleep` and for an idle runtime's poller, both of which
 * would otherwise spin on the clock. False when there is no thread or no
 * deadline. */
bool sched_sleep_until(uint64_t deadline_ns);

/* How many sleeps there were, and how late the kernel woke them — the
 * average and the worst, in microseconds. The tick is the floor: a sleep
 * cannot end sooner than the next one. It is reported because "the sleep
 * took twenty times too long" has two possible causes, and this separates
 * them: a late wake-up is the kernel's, an on-time one means the program
 * never asked to sleep. */
void sched_sleep_report(uint64_t *count, uint64_t *average_late_us, uint64_t *worst_late_us);

/* Wakes up to `count` threads waiting on `addr`. Reports how many. */
uint32_t sched_futex_wake(virt_t addr, uint32_t count);

/* Ends the calling thread. Does not return. When it was the last one, the
 * process is over and this leaves through `uspace_exit`. */
void sched_exit_thread(int64_t status);

/* The calling thread's id, and how many threads are alive. */
uint64_t sched_current_tid(void);
uint32_t sched_live_threads(void);

/* How many live threads one process has, which is what says whether *that*
 * process is finished. */
uint32_t sched_live_threads_of(const struct uproc *p);

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

/* How many threads have been found to have overflowed their kernel stack.
 * Any value but zero means memory that belonged to something else was
 * written, and nothing after it can be trusted. */
uint32_t sched_stack_overflows(void);

/* How many times a thread could not be created because the table was full.
 * Any value but zero is a Go runtime about to abort. */
uint32_t sched_spawn_failures(void);

/* How many times a thread saving its stack pointer found the slot no longer
 * its own — two harts with different ideas of what is running. */
uint32_t sched_slot_aliases(void);

/* How many ticks the scheduler has seen, and how many blocked threads it
 * has woken because their deadline passed. A deadline that never wakes is a
 * thread that waits for ever, and these two tell that apart from a tick
 * that never arrives. */
uint32_t sched_ticks_seen(void);

/* Nanoseconds summed over every hart that had nothing to run — the measured
 * quantity behind `/proc/stat`'s idle column, accumulated at the one point
 * where a hart's idleness starts and stops rather than sampled. Across four
 * harts it can be four times the uptime, which is what makes it divisible
 * into a proportion. */
uint64_t sched_idle_ns(void);

/* ── when this hart next needs the timer ───────────────────────────────── */

/* Works out what this hart is waiting for and arms — or **disarms** — its
 * timer accordingly.
 *
 * Called after every scheduling decision, because every one of them can
 * change the answer: a thread that blocks removes its slice, a thread that
 * wakes may add one, and a hart left with a single runnable thread and
 * nothing asleep needs no timer at all.
 *
 * Three things can want it:
 *
 *   - the running thread's **slice**, but only when something else is
 *     runnable; one thread alone is not competing with anybody and a timer
 *     that interrupts it would be pure overhead;
 *   - the soonest **deadline** any blocked thread is waiting for;
 *   - **housekeeping**, which only hart 1 does.
 *
 * `timer_arm_at` ignores a request for a deadline it already holds, so
 * calling this often is cheap; what is not cheap is the firmware call it
 * skips.
 */
/* Turns dynamic ticks on: the scheduler arms a one-shot timer for the next
 * thing that is actually due, instead of a periodic clock. Off by default —
 * see `sched_run` and docs/PREEMPTION_MODEL.md. */
void sched_set_dynticks(bool on);

void sched_arm_timer(void);

/* Pokes every hart that is idle, so it looks for the work that has just
 * appeared. With no periodic tick this is the only thing that ends an idle
 * hart's `wfi`. */
void sched_wake_idle_harts(void);

/* How many times an idle hart's backstop timer found work that should have
 * woken it. Any value but zero is a lost wakeup: the machine kept going
 * because of a timeout rather than because the scheduler was right. */
uint32_t sched_missed_pokes(void);

/* How many times a hart stopped looking for work while the soonest deadline
 * in the system was armed on no hart at all. **Every one of these is a hang
 * that did not happen yet**: nothing will interrupt any hart, so whoever was
 * waiting waits for ever. It must be zero. */
uint32_t sched_lost_deadlines(void);

/* Deadlines armed after their moment had passed: how many, how many were
 * followed by a timer interrupt on that hart, the worst lateness at arming
 * and the worst wait for the interrupt after it. */
struct sched_past_arm_report {
    uint64_t arms;
    uint64_t fired;
    uint64_t worst_behind_us;
    uint64_t worst_fire_us;
    uint64_t superseded;
    uint64_t worst_behind_by_source_us[3]; /* slice, beat, sleeper */
};
void sched_past_arms(struct sched_past_arm_report *out);

/* Threads found still blocked a second or more past their own deadline — a
 * wake-up that was armed and never arrived. The deadlock detector cannot
 * see these, because a deadline is a reason to believe something will
 * happen, so without this they are a hang with no output at all. */
uint32_t sched_overdue_wakes(void);

/* Threads that could not be resumed because their saved stack pointer was
 * not inside their own kernel stack. A thread cannot do this to itself:
 * any value but zero is the kernel having lost track of whose stack is
 * whose, and it is the one way a *kernel* register set becomes a user
 * frame. */
uint32_t sched_bad_resumes(void);

/* Keeps every thread on the hart it first ran on. A diagnostic for §0's
 * race: it leaves four harts running user code while no thread's state ever
 * passes between them. */
void sched_pin_threads(bool on);

/* The running thread's nice value, and setting it. Setting clamps to the
 * permitted range rather than refusing, as POSIX requires, and recomputes
 * the weight the scheduler divides by — so it takes effect at the next
 * scheduling decision rather than at the next slice boundary. False only if
 * there is no running thread to set it on. */
int32_t sched_nice(void);
bool sched_set_nice(int32_t nice);

/* **Affinity**: which harts may run the calling thread, as a bitmask of
 * indices into the hart table — not of hart ids, which are the firmware's
 * and start at 1 on this board. Inherited at `clone`.
 *
 * Setting is refused when the mask leaves nowhere to run, rather than
 * accepted into a thread that will never be chosen again. */
uint64_t sched_affinity(void);
bool sched_set_affinity(uint64_t mask);

/* **Isolation**: harts the scheduler places no work on at all, so that
 * something else can have them. A property of the machine's configuration,
 * set once from the command line (`minikern.isolate=<mask>`), where
 * affinity is a property of a thread. An isolated hart still idles inside
 * the scheduler, so it can be given work again without being restarted. */
/* The round-robin quantum, for `SCHED_RR` only. 10 ms, which is long
 * against the 1.5 ms fair floor on purpose: a real-time thread is not
 * sharing, it is taking turns with its own equals. */
#define SCHED_RR_QUANTUM_NS 10000000ull

/* The real-time priority range. 0 is the fair class; 1..99 are real-time,
 * higher first — the range POSIX and Linux both use, so a program that asks
 * `sched_get_priority_max` gets the answer it expects. */
#define SCHED_RT_PRIORITY_MAX 99u

/* The real-time class: priority 0 is the fair class, 1..99 real-time.
 * Inherited at `clone`, like nice and affinity. Setting it yields, because
 * a thread that has just raised itself above the one it shares a hart with
 * should be the one running. */
/* **Stops and resumes every thread of a process.** `SIGSTOP` and `SIGCONT`
 * act on the process rather than being delivered to a handler, so they are
 * the scheduler's to implement. A thread running on another hart stops at
 * its next return to user mode. Returns how many threads moved, so "stopped
 * a process" can be told from "there was nothing to stop". */
uint32_t sched_stop_process(uint32_t pid);

/* Raises a signal on a process — meaning on the thread of it that will
 * notice first. This kernel has no process-wide pending set, so "the
 * process" is approximated by its first live thread: right for the signals
 * a shell sends, and not right for a multi-threaded program catching them.
 * Returns false if the process has no thread to raise it on. */
bool sched_raise_on_process(uint32_t pid, uint32_t signo);

/* Whether the running thread has an unblocked signal waiting.
 *
 * A blocking read needs it: a read interrupted by a signal has to return so
 * the handler can run, and a read that keeps waiting makes `^C` do nothing
 * until the user types a line — which looks exactly like the key being
 * ignored. */
bool sched_signal_pending(void);
uint32_t sched_continue_process(uint32_t pid);

uint32_t sched_rt_priority(void);
bool sched_rt_is_round_robin(void);
bool sched_set_rt(uint32_t priority, bool round_robin);

void sched_set_isolated(uint64_t mask);

/* Whether the tick may take a hart from a thread that has not yielded.
 * Off means threads switch only at points they chose, which separates what
 * a context switch saves from when one is allowed to happen. */
void sched_set_preemption(bool on);
uint64_t sched_isolated(void);


/* How long a slice is, in nanoseconds. One value for now: the fair class
 * in Phase 2 computes it per thread from the runnable count and weights. */
/* The floor on a slice: below this, switching costs more than the slice
 * delivers. 1.5 ms, which Phase 0 settled by measurement — an
 * address-space switch on this machine costs about 330 ns more than a
 * thread switch (no ASID, so the TLB is flushed), which is 0.02% of this,
 * so the plan's fallback of widening the quantum was not needed.
 * `docs/scheduler-phase0.md` has the table. */
#define SCHED_SLICE_NS 1500000ull

/* The period in which every runnable thread should get a turn. A thread's
 * slice is its share of this by weight, floored at `SCHED_SLICE_NS` — so
 * eight runnable threads of equal weight get 1.5 ms each and the period
 * stretches to 12 ms rather than the slices shrinking to nothing. */
#define SCHED_TARGET_LATENCY_NS 12000000ull

/* What waking is worth, as a credit against the smallest virtual clock.
 * Half the target period: enough that a thread which has just been woken is
 * served before threads that have been running, and bounded so that
 * sleeping repeatedly does not accumulate a claim. */
#define SCHED_WAKE_CREDIT_NS 6000000ull
uint32_t sched_deadline_wakes(void);

#endif
