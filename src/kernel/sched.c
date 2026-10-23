#include "kernel/sched.h"

#include "arch/riscv/cache.h"
#include "arch/riscv/fp.h"
#include "arch/riscv/sbi.h"
#include "arch/riscv/smp.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/fair.h"
#include "kernel/klock.h"
#include "kernel/mem.h"
#include "kernel/uspace.h"

/* The fair class's two aggregates, declared here because the thread
 * table's own helpers use them above their definitions. */
static uint64_t min_vruntime(void);
static uint32_t total_weight(void);

/* 16 KiB of kernel stack per thread. A trap frame is 288 bytes and the
 * handlers nest shallowly, so this is generous — but a kernel stack that
 * overflows corrupts whatever is below it silently, and
 * `-Wframe-larger-than=1024` only bounds one frame at a time. */
#define KSTACK_PAGES 4u

/* A word written at the **lowest** address of every kernel stack, and
 * checked at every context switch.
 *
 * A kernel stack that overflows does not fault — the page below it is
 * ordinary memory belonging to something else, so the write lands and the
 * machine carries on with another thread's stack or a user page quietly
 * altered. That is indistinguishable, from user space, from locks that do
 * not work, and it gets more likely with more threads, which is exactly the
 * shape of the race this is looking for.
 *
 * The check is two loads and a compare on a path that already switches a
 * stack, so it stays in the build rather than being a debugging aid that
 * has to be re-added. */
#define KSTACK_CANARY 0x6b73746163616e21ull /* "kstacan!" */

/* A trap frame's size, which the layout above depends on. Checked against
 * the real struct so that adding a field to it cannot silently move the
 * stack top. */
#define FRAME_BYTES (sizeof(struct trapframe))

/* `THREAD_DYING` is the state between "this thread has ended" and "its slot
 * may be reused".
 *
 * **A thread that has ended is still standing on its kernel stack.** It has
 * to switch away before anything else may have its slot, and marking the
 * slot free at the moment of exit opened a window exactly that wide: another
 * hart calling `find_free_slot` could take the slot, allocate the new
 * thread a stack and fill in its `ksp` — and then the *exiting* thread,
 * still running, reached `yield_to_scheduler`, which writes the stack
 * pointer it is leaving into `threads[was].ksp`. That slot now belonged to
 * the new thread, so the new thread was resumed on the dead one's stack,
 * with the dead one's floating-point state.
 *
 * The code said so and believed otherwise: "`ksp` is written to a slot
 * nothing will read again". Nothing will read it again only once the slot
 * cannot be handed out, which is what this state is for. */
enum thread_state {
    THREAD_FREE = 0,
    THREAD_RUNNABLE,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DYING,
    /* **Stopped by a signal, which is not the same as blocked.**
     *
     * A blocked thread is waiting for something that will happen — a
     * deadline, a futex, a child. A stopped one is waiting for `SIGCONT`,
     * which may never come, and it must not be woken by whatever it was
     * blocked on either: a process stopped in the middle of a sleep stays
     * stopped when the sleep expires. Keeping it a separate state is what
     * makes both of those true without a flag that every wake path has to
     * remember to check. */
    THREAD_STOPPED
};

/* A thread.
 *
 * `ksp` is where its kernel stack pointer was when it was switched away
 * from, which is the whole of a switch. `frame` is only used before it has
 * ever run: a new thread's initial register set lives at the base of its
 * own kernel stack, and `uspace_enter_frame` consumes it once. */
struct thread {
    uint64_t ksp;
    phys_t kstack;
    uint64_t tid;
    enum thread_state state;
    /* The hart that last ran it, for the pinning diagnostic above.
     * `SMP_MAX_HARTS` means "never run anywhere yet". */
    uint32_t last_hart;
    virt_t futex_addr;
    const void *wait_chan;  /* a kernel object being waited on; see sched.h */
    uint64_t futex_deadline;
    bool woken;
    bool owns_stack;
    struct thread_signals signals;
    struct uproc *proc;  /* the process this thread belongs to */
    /* The fair class. `nice` is what a program sets, `weight` is what the
     * scheduler divides by, and `vruntime` is the clock that decides who
     * runs next — smallest first. The weight is kept beside the nice value
     * rather than looked up each time because it is read on every
     * scheduling decision and the nice value changes almost never. */
    int32_t nice;
    uint32_t weight;
    uint64_t vruntime;
    /* Which harts may run this thread, by **index** into the hart table and
     * not by hart id — the ids are the firmware's and on this board they
     * start at 1, so a mask of ids would be off by one against every other
     * per-hart array. Inherited at clone; all schedulable harts by default. */
    uint64_t harts;
    /* The real-time class. `rt_priority` of 0 means the fair class above —
     * one field for both the class and its priority, because "SCHED_FIFO at
     * priority 0" is not a thing and a separate enum would allow it to be
     * written down. 1..99 are real-time, higher first. */
    uint32_t rt_priority;
    bool rt_round_robin; /* SCHED_RR rather than SCHED_FIFO */
    /* The floating-point registers, which are this thread's state as much
     * as the integer ones. Aligned because the save instructions address it
     * directly. */
    uint8_t fp[FP_AREA_BYTES] __attribute__((aligned(8)));
};

/* What a hart is doing.
 *
 * **Per hart, not global**, which is the whole difference between one core
 * and four: `current` used to be one variable, and two harts in the kernel
 * would each have believed they owned the same thread. The index is the
 * hart's position in the device tree's enumeration, because hart ids are
 * not 0..n-1 — on the target board they are an S7 at 0 and four U74s at
 * 1-4.
 *
 * `idle_ksp` is where the hart's own scheduling loop is suspended while a
 * thread runs on it. A thread gives the hart back by switching to it,
 * rather than picking the next thread itself: a thread that chose its own
 * successor could choose one another hart is already running. */
struct hartstate {
    uint64_t idle_ksp;
    /* The process whose address space this hart has loaded. Checked before
     * each switch, because two processes are two `satp` values and a hart
     * that forgets to change it runs one process's code against another's
     * page tables. */
    struct uproc *loaded;
    uint32_t current;   /* SCHED_NO_THREAD when it is idle */
    uint64_t switches;
    bool joined;
    /* In `wfi` with nothing to run. */
    bool idle;
    /* Running a thread with **no timer armed**, because when it was last
     * scheduled nothing else wanted the hart. Such a hart will run its
     * thread for ever unless something tells it the situation has changed —
     * it has no clock of its own to find out with. */
    bool untimed;
    bool backstop; /* the armed deadline is the idle timeout, not a real one */
    /* **Per hart, because a timer interrupt is per hart.** This was one
     * global: every hart's tick set it and whichever hart next returned to
     * user space consumed it. So a hart that had just been given a thread
     * could be preempted on another hart's tick, while the hart whose
     * quantum had actually expired kept running — the opposite of what the
     * tick is for. Nothing was corrupted, because the kernel lock serialises
     * it; it simply scheduled by the wrong hart's clock. */
    bool preempt_pending;
    /* When the running thread started its slice, so the scheduler knows
     * when it ends without a periodic clock to count. */
    uint64_t slice_started;
    /* The next time this hart must do housekeeping, or 0 for never. Only
     * hart 1 has one: deferred work, timer expiry for things that are not
     * threads, and later the frequency governor. Keeping it on one hart
     * means the others can be left completely alone. */
    uint64_t housekeeping;
} CACHE_ALIGNED;

#define SCHED_NO_THREAD 0xFFFFFFFFu

static struct thread threads[SCHED_MAX_THREADS];

/* Harts the scheduler must not place work on.
 *
 * A hart is isolated so that something else can have it — a measurement that
 * must not be disturbed, or (later) a real-time thread that must not share.
 * It is a property of the *machine's configuration*, set once from the
 * command line, rather than something a thread can ask for: a thread asking
 * to be alone on a hart is affinity, and that is the mask above. */
static uint64_t isolated;

/* The harts a thread may actually be placed on: what it asked for, minus
 * what the machine has reserved. Computed rather than stored, so isolating a
 * hart takes effect on threads that already exist. */
static uint64_t placeable(const struct thread *t) {
    uint64_t want = t->harts == 0 ? ~0ull : t->harts;
    return want & ~isolated;
}

/* Whether a thread may be pinned to wherever it last ran.
 *
 * A diagnostic, not a policy: with it on, a thread stays on one hart for
 * its whole life while the other harts keep running other threads. That
 * separates "four harts executing user code at once" from "one thread's
 * state passing between harts" — and the second is where a context switch,
 * a trap frame, a saved `tp` and a hart's `satp` all live. §0's race needs
 * at least two harts; this says which of the two reasons it needs them
 * for. */
static bool pin_threads_where_they_ran;

void sched_pin_threads(bool on) {
    pin_threads_where_they_ran = on;
}

static bool may_run_here(const struct thread *t, uint32_t hart) {
    if (pin_threads_where_they_ran && t->last_hart != SMP_MAX_HARTS && t->last_hart != hart) {
        return false;
    }
    return (placeable(t) & (1ull << hart)) != 0;
}

void sched_set_isolated(uint64_t mask) {
    isolated = mask;
}

uint64_t sched_isolated(void) {
    return isolated;
}


static void poke_idle_harts(void);

/* Whether this scheduler arms its own timer rather than riding a periodic
 * tick. See `sched_run`. */
static bool dynticks_wanted = true;

void sched_set_dynticks(bool on) {
    dynticks_wanted = on;
}

/* Threads refused because the table was full. Counted because a `clone` the
 * kernel refuses is not a slowdown to a Go runtime — it is fatal. */
static uint32_t spawn_failures;

uint32_t sched_spawn_failures(void) {
    return spawn_failures;
}
static struct hartstate harts[SMP_MAX_HARTS];
static volatile bool accepting_harts;
static volatile bool process_over;
static uint32_t idle_ticks;
static uint64_t sleeps;
static uint64_t late_total;
static uint64_t worst_late;
static bool deadlock_reported;
static uint64_t next_tid = 1;

static bool tracing;

void sched_set_trace(bool on) {
    tracing = on;
}

bool sched_tracing(void) {
    return tracing;
}

/* One line about one thread. Takes the slot rather than the thread so that
 * a line can name both a thread and who it is about. */
static void trace(const char *what, uint32_t slot, uint64_t detail) {
    if (!tracing) {
        return;
    }
    kprintf("sched: tid %llu %s %llx (%u live)\n", (unsigned long long)threads[slot].tid, what,
            (unsigned long long)detail, sched_live_threads());
}

/* Declared in switch.S. */
void context_switch(uint64_t *save_sp, uint64_t load_sp);
void uspace_enter_frame(struct trapframe *frame);

/* The hart this code is running on. */
static struct hartstate *me(void) {
    return &harts[smp_hart_index()];
}

/* The thread this hart is running, or NULL when it is between threads. */
static struct thread *running_thread(void) {
    struct hartstate *h = me();
    return h->current == SCHED_NO_THREAD ? NULL : &threads[h->current];
}

int sched_init(struct uproc *proc, const struct trapframe *frame, uint64_t sp, uint64_t *tid) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        threads[i] = (struct thread){0};
    }
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        harts[i] = (struct hartstate){.current = SCHED_NO_THREAD};
    }
    next_tid = 1;
    process_over = false;
    idle_ticks = 0;
    deadlock_reported = false;
    sleeps = 0;
    late_total = 0;
    worst_late = 0;
    /* The first thread is an ordinary one. It used to be the stack
     * `uspace_run` was called on, which made it the only thread that could
     * not be scheduled by another hart — and with four harts that
     * exception has to go. */
    return sched_spawn_first(proc, frame, sp, tid);
}

static uint32_t find_free_slot(void) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_FREE) {
            return i;
        }
    }
    return SCHED_MAX_THREADS;
}

/* Lays out a new thread's kernel stack, and says where its first trap frame
 * sits.
 *
 * **At the top, not the bottom.** `uspace_enter_frame` records the stack a
 * trap will land on as the frame's address plus its size, so a frame at the
 * base of the allocation gives the next trap 288 bytes and then lets it run
 * off the end into whatever was allocated before it. Putting the frame at
 * the top means that address *is* the top of the stack, which is what the
 * trap path expects everywhere else.
 *
 * Below it sits a `context_switch` frame whose return address is the
 * trampoline, so switching to this thread for the first time *returns into*
 * the trampoline — a thread that has never run and one that was preempted
 * are then resumed by the same instruction, with no special case in the
 * scheduler. */
static struct trapframe *initial_frame_of(const struct thread *t) {
    return (struct trapframe *)(uintptr_t)(t->kstack + KSTACK_PAGES * PAGE_SIZE - FRAME_BYTES);
}

static void prepare_stack(struct thread *t, const struct trapframe *frame, uint64_t sp,
                          bool is_child);

/* The first thing a new thread does. It is a C function so that the stack
 * layout above has an address to name, and it never returns: the thread
 * leaves through `sret`, and comes back only as a trap. */
static void thread_trampoline(void) {
    struct trapframe *frame = initial_frame_of(running_thread());
    /* The kernel lock, which the scheduling loop was holding when it
     * switched here, must be given up **before** user mode: a hart that
     * entered user code still holding it would shut every other hart out of
     * the kernel until its next trap, which is the whole parallelism gone.
     * Every other path out of the kernel releases it in `trap_handle`; this
     * one has no trap to return from, so it does it itself. */
    trace("entering user mode at", me()->current, frame->sepc);
    klock_exit(true);
    uspace_enter_frame(frame);
}

static void prepare_stack(struct thread *t, const struct trapframe *frame, uint64_t sp,
                          bool is_child) {
    struct trapframe *initial = initial_frame_of(t);
    uint64_t *switch_frame;
    *initial = *frame;
    if (is_child) {
        initial->x[10] = 0;  /* a0: the child's half of clone's return */
    }
    initial->x[2] = sp;  /* the stack the caller gave it */
    /* The switch frame sits just below the initial frame, with `ra` pointing
     * at the trampoline and the callee-saved registers zero. 14 words is
     * `context_switch`'s frame: ra, s0..s11, and a word of padding to keep
     * the stack 16-byte aligned. */
    switch_frame = (uint64_t *)(uintptr_t)((uint64_t)(uintptr_t)initial - 112u);
    for (uint32_t i = 0; i < 14u; i++) {
        switch_frame[i] = 0;
    }
    switch_frame[0] = (uint64_t)(uintptr_t)thread_trampoline;
    t->ksp = (uint64_t)(uintptr_t)switch_frame;
}

/* Both spawns are the same work; the first thread differs only in that
 * `a0` is not forced to 0 — it is not returning from a `clone`. */
static int spawn(struct uproc *proc, const struct trapframe *frame, uint64_t sp, bool is_child,
                 uint64_t *tid) {
    uint32_t slot = find_free_slot();
    phys_t stack = 0;
    int rc;
    if (slot == SCHED_MAX_THREADS) {
        spawn_failures++;
        return KERR_AGAIN;
    }
    rc = mem_alloc_pages(KSTACK_PAGES, 1, &stack);
    if (rc != KERR_OK) {
        return rc;
    }
    *(volatile uint64_t *)(uintptr_t)stack = KSTACK_CANARY;
    threads[slot] = (struct thread){.kstack = stack,
                                    .tid = next_tid++,
                                    .state = THREAD_RUNNABLE,
                                    .owns_stack = true,
                                    .last_hart = SMP_MAX_HARTS,
                                    .proc = proc};
    /* A new thread **inherits its creator's signal mask**, as the ABI
     * requires, and nothing else: not its pending signals, and not its
     * alternate stack, which is the creator's own memory and would be shared
     * if it were copied. A runtime sets a stack per thread for exactly that
     * reason. */
    {
        const struct thread_signals *parent = sched_current_signals();
        threads[slot].signals.blocked = parent != NULL ? parent->blocked : 0u;
    }
    /* **The nice value is inherited, as the ABI requires**, and the virtual
     * clock is placed rather than started at zero: a new thread with a clock
     * of zero is maximally behind every existing one and would take the
     * processor until it caught up — which is how a runtime that creates
     * threads in a burst would stall the threads already working. */
    {
        const struct thread *creator = running_thread();
        threads[slot].nice = creator != NULL ? creator->nice : 0;
        threads[slot].weight = fair_weight_of_nice(threads[slot].nice);
        threads[slot].vruntime = fair_wake_vruntime(0, min_vruntime(),
                                                    SCHED_WAKE_CREDIT_NS);
        threads[slot].harts = creator != NULL ? creator->harts : ~0ull;
        threads[slot].rt_priority = creator != NULL ? creator->rt_priority : 0u;
        threads[slot].rt_round_robin = creator != NULL && creator->rt_round_robin;
    }
    prepare_stack(&threads[slot], frame, sp, is_child);
    /* A new thread is work that has just appeared, and an idle hart has no
     * clock to notice it with. */
    poke_idle_harts();
    *tid = threads[slot].tid;
    trace("created, entering at", slot, frame->sepc);
    return KERR_OK;
}

int sched_spawn(const struct trapframe *frame, uint64_t sp, uint64_t *tid) {
    /* The creator's process: a cloned **thread** shares everything. */
    return spawn(sched_current_proc(), frame, sp, true, tid);
}

int sched_spawn_in(struct uproc *proc, const struct trapframe *frame, uint64_t sp,
                   uint64_t *tid) {
    return spawn(proc, frame, sp, true, tid);
}

int sched_spawn_first(struct uproc *proc, const struct trapframe *frame, uint64_t sp,
                      uint64_t *tid) {
    return spawn(proc, frame, sp, false, tid);
}

/* The channel a thread leaving a hart wakes: what `sched_await_siblings`
 * blocks on, woken by the scheduling loop of whichever hart let the thread
 * go. */
static uint8_t sibling_channel;

/* Whether any hart is running this slot, this moment.
 *
 * The **hart's** `current` is the authority, not the thread's state. Ending
 * a thread sets its state to `THREAD_DYING` while it is still executing on
 * a hart, so a test of the state would answer "no" about a thread that is
 * very much running — which is the whole reason this question has to be
 * asked separately. */
static bool on_a_hart(uint32_t slot) {
    for (uint32_t h = 0; h < SMP_MAX_HARTS; h++) {
        if (harts[h].current == slot) {
            return true;
        }
    }
    return false;
}

/* Sends one hart an interrupt although it is busy.
 *
 * `poke_idle_harts` deliberately skips a busy hart: an IPI costs 1499 ns and
 * a hart that is already working finds new work by itself. **Here that
 * reasoning is inverted**, which is why this is a separate function rather
 * than a relaxed condition in that one. The point is not to make work
 * visible but to make one named hart *stop*, and under dynamic ticks a hart
 * running user code may have no deadline armed at all — so left alone it
 * might not trap again until the idle backstop, a hundred milliseconds
 * away, or until the thread chose to make a syscall. */
static void poke_one_hart(uint32_t index) {
    const struct smp_hart *h = smp_hart(index);
    if (h == NULL || index == smp_hart_index()) {
        return;
    }
    (void)sbi_send_ipi(1ull, (uint64_t)h->id);
}

/* Makes whichever hart is running `slot` leave it at the first opportunity.
 *
 * Two halves, and both are needed. The flag is what the trap path reads on
 * its way back to user mode (`sched_take_preempt_request`); the interrupt is
 * what makes a trap happen at all. Setting the flag alone is what this
 * kernel used to do, and it made "it stops at its next return to user mode"
 * true only of a thread that was going to return anyway. */
static uint32_t leave_pokes;
static uint32_t sibling_wakes;
static uint32_t await_loops;

static void make_it_leave(uint32_t slot) {
    for (uint32_t h = 0; h < SMP_MAX_HARTS; h++) {
        if (harts[h].current != slot) {
            continue;
        }
        harts[h].preempt_pending = true;
        poke_one_hart(h);
        leave_pokes++;
    }
}

/* Gives up a thread that is not on a hart.
 *
 * Its slot is free immediately, which is safe precisely because it is not
 * running: nothing will resume it, and a thread here *is* its slot — there
 * is no other reference by which it could be found again.
 *
 * **Its kernel stack is left to `sched_shutdown`**, which frees every one
 * at the end of the run. Handing it back here would be correct by the same
 * argument and would stop a process that exec's in a loop leaking four
 * pages a time, but it is a separate change: freeing a page is not part of
 * closing this race, and the two should not be proved together. */
static void abandon(struct thread *t) {
    phys_t stack = t->kstack;
    bool owned = t->owns_stack;
    *t = (struct thread){0}; /* THREAD_FREE, with no stale pointers left in it */
    t->kstack = stack;
    t->owns_stack = owned;
}

uint32_t sched_end_siblings(const struct uproc *proc) {
    uint32_t ended = 0;
    struct thread *me_thread = running_thread();
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_FREE || threads[i].state == THREAD_DYING ||
            &threads[i] == me_thread) {
            continue;
        }
        if (threads[i].proc != proc) {
            continue;
        }
        if (on_a_hart(i)) {
            /* **Dying, not free.** It is standing on its kernel stack this
             * moment, and a free slot is one `find_free_slot` may hand to a
             * new thread while the old one is still on it. The hart running
             * it releases the slot itself, once the switch has actually left
             * the stack — and `make_it_leave` is what makes that switch
             * happen now rather than whenever the thread next traps. */
            threads[i].state = THREAD_DYING;
            make_it_leave(i);
        } else {
            abandon(&threads[i]);
        }
        ended++;
    }
    return ended;
}

/* How long to wait before looking again. The timer rounds deadlines to
 * 100 µs, so asking for less is asking for the same thing. */
#define SIBLING_LOOK_AGAIN_NS 100000ull

/* How long to wait in total. A thread that has been told to leave its hart
 * and has not done so in 50 ms is not slow, it is a kernel bug — and 50 ms
 * is long enough that no amount of ordinary lock contention reaches it. */
#define SIBLING_WAIT_LIMIT_NS 50000000ull

static uint32_t siblings_on_harts(const struct uproc *proc) {
    uint32_t n = 0;
    const struct thread *me_thread = running_thread();
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (&threads[i] == me_thread || threads[i].proc != proc) {
            continue;
        }
        n += on_a_hart(i) ? 1u : 0u;
    }
    return n;
}

/* How many threads have had to be waited off a hart, and the longest wait.
 *
 * These are what make the race's closing a measurement rather than an
 * assertion. The window used to be open and nothing reported it, which is
 * why the defect could be recorded as "has not been seen to fail" — a
 * sentence that says nothing about whether it fails. A non-zero count is
 * the kernel saying it caught a thread mid-flight. */
static uint32_t awaited_threads;
static uint64_t worst_await_ns;

void sched_await_detail(uint32_t *pokes, uint32_t *wakes, uint32_t *loops) {
    *pokes = leave_pokes;
    *wakes = sibling_wakes;
    *loops = await_loops;
}

void sched_await_report(uint32_t *threads_waited, uint64_t *worst_us) {
    if (threads_waited != NULL) {
        *threads_waited = awaited_threads;
    }
    if (worst_us != NULL) {
        *worst_us = worst_await_ns / 1000u;
    }
}

uint32_t sched_await_siblings(const struct uproc *proc) {
    struct thread *me_thread = running_thread();
    /* **What the caller was**, because the wait below will overwrite it.
     * `sched_wait_on` sets `THREAD_BLOCKED`, and this is reached on the
     * last-thread exit path by a caller that is already `THREAD_DYING` — a
     * dying thread that came back as merely blocked would be woken, resumed
     * and never have its slot released. The same lesson as
     * `sched_yield_now`: a state another path set is not this one's to
     * replace. */
    enum thread_state was = me_thread != NULL ? me_thread->state : THREAD_FREE;
    uint64_t started = timer_ns();
    uint64_t until = started + SIBLING_WAIT_LIMIT_NS;
    uint32_t left = siblings_on_harts(proc);
    awaited_threads += left;
    while (left != 0 && timer_ns() < until) {
        /* **Blocked, not spun.** This hart holds the kernel lock, and a
         * sibling cannot reach the kernel to notice it has been ended until
         * that lock is free — so spinning here is a deadlock rather than a
         * wait. Yielding is not enough either: a thread that yields is
         * immediately the only runnable thread again, and the scheduling
         * loop hands it straight back without the lock having been
         * released. Blocking with a deadline is what makes the hart idle,
         * and an idle hart releases the lock. That is the one path that
         * lets the sibling in. */
        (void)sched_wait_on(&sibling_channel, timer_ns() + SIBLING_LOOK_AGAIN_NS);
        if (me_thread != NULL && was != THREAD_RUNNING) {
            me_thread->state = was;
        }
        await_loops++;
        left = siblings_on_harts(proc);
    }
    {
        uint64_t took = timer_ns() - started;
        worst_await_ns = took > worst_await_ns ? took : worst_await_ns;
    }
    return left;
}

/* A thread no hart is running, claimed for this one. The claim is the
 * state change: a thread marked RUNNING is owned by whichever hart marked
 * it, and under the kernel lock that is enough — two harts cannot both see
 * it RUNNABLE. Round-robin from one shared cursor, so harts do not all
 * start their search at the same thread and collide on it. */
/* **The thread whose virtual clock is furthest behind.**
 *
 * This is the whole of the fair class's decision. A thread's virtual clock
 * advances at a rate inversely proportional to its weight, so the smallest
 * one belongs to whoever has had least of the processor *for their weight* —
 * and picking it shares the machine in proportion to weight without any
 * thread being starved, because a thread that is passed over keeps its
 * clock while the others advance and becomes the smallest eventually and
 * unconditionally.
 *
 * The cursor survives as the tie-break. Equal clocks are the common case —
 * every thread of the same nice value that has run for the same time — and
 * breaking the tie by position rather than by index keeps the round-robin
 * behaviour the previous implementation had, instead of favouring whichever
 * thread happens to sit lowest in the table. */
/* A thread becoming runnable after not being so — new, woken, or its sleep
 * expired — placed relative to the threads already waiting.
 *
 * It must not simply keep the clock it had: a thread that slept for a second
 * is a second behind and would monopolise the processor until it caught up.
 * Nor may it take the minimum outright, which hands a thread that sleeps
 * repeatedly a fresh claim every time. `fair_wake_vruntime` does neither,
 * and the arithmetic is tested on the host. */
/* **A waking real-time thread evicts a lower-priority one immediately.**
 *
 * This is what makes the class real-time rather than merely preferred: the
 * latency that matters is from the wake to the first instruction, and
 * waiting for the running thread's slice to end would put a fair thread's
 * 1.5 ms in front of it. Every hart running something it outranks is marked
 * for preemption and poked; the poke is what reaches a hart that is running
 * untimed and would otherwise not look again until its next trap.
 *
 * Marked rather than switched, because a thread cannot be taken off a hart
 * from another hart — the preemption happens where it always does, on the
 * way back to user mode. */
static void preempt_for_rt(uint32_t slot) {
    bool any = false;
    if (threads[slot].rt_priority == 0) {
        return;
    }
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        uint32_t running = harts[i].current;
        if (!harts[i].joined || running == SCHED_NO_THREAD) {
            continue;
        }
        if (threads[running].rt_priority < threads[slot].rt_priority) {
            harts[i].preempt_pending = true;
            any = true;
        }
    }
    if (any) {
        poke_idle_harts();
    }
}

static void place_on_runqueue(uint32_t slot) {
    threads[slot].state = THREAD_RUNNABLE;
    threads[slot].vruntime = fair_wake_vruntime(threads[slot].vruntime, min_vruntime(),
                                                SCHED_WAKE_CREDIT_NS);
    preempt_for_rt(slot);
}

/* What the thread just ran, charged to its virtual clock.
 *
 * Done on the way *out* of the switch rather than on the way in, because
 * "how long did it run" is only known once it has stopped — and it is the
 * one place every exit path passes through, whether the thread yielded,
 * blocked, was preempted or ended. */
static void charge_vruntime(uint32_t slot, uint64_t started) {
    uint64_t now = timer_ns();
    uint64_t ran = now > started ? now - started : 0u;
    threads[slot].vruntime += fair_vruntime_delta(ran, threads[slot].weight);
}

/* Whether a runnable thread in `slot` can be placed on this hart. */
static bool candidate(uint32_t slot, uint32_t hart) {
    return threads[slot].state == THREAD_RUNNABLE && may_run_here(&threads[slot], hart);
}

/* The highest-priority runnable real-time thread, or none.
 *
 * **Priority order, absolutely.** A real-time thread runs in preference to
 * every fair thread and to every lower-priority real-time one, for as long
 * as it wants to: that is what the class promises, and a kernel that mixed
 * it with proportional share would be promising something weaker. The cost
 * is that a runaway real-time thread can starve everything else, which is
 * the caller's responsibility and is why setting the class is privileged
 * everywhere it exists.
 *
 * Among equal priorities the cursor decides, which makes it round-robin for
 * `SCHED_RR` (those threads also get a quantum) and arrival-ordered enough
 * for `SCHED_FIFO`, whose threads run until they block. */
static uint32_t claim_rt(uint32_t cursor, uint32_t hart) {
    uint32_t best = SCHED_NO_THREAD;
    uint32_t top = 0;
    for (uint32_t step = 0; step < SCHED_MAX_THREADS; step++) {
        uint32_t i = (cursor + step) % SCHED_MAX_THREADS;
        if (!candidate(i, hart) || threads[i].rt_priority == 0) {
            continue;
        }
        if (threads[i].rt_priority > top) {
            best = i;
            top = threads[i].rt_priority;
        }
    }
    return best;
}

/* The fair class's choice: the thread whose virtual clock is furthest
 * behind. */
static uint32_t claim_fair(uint32_t cursor, uint32_t hart) {
    uint32_t best = SCHED_NO_THREAD;
    uint64_t least = 0;
    for (uint32_t step = 0; step < SCHED_MAX_THREADS; step++) {
        uint32_t i = (cursor + step) % SCHED_MAX_THREADS;
        if (!candidate(i, hart) || threads[i].rt_priority != 0) {
            continue;
        }
        if (best == SCHED_NO_THREAD || threads[i].vruntime < least) {
            best = i;
            least = threads[i].vruntime;
        }
    }
    return best;
}

static uint32_t claim_runnable(void) {
    static uint32_t cursor;
    uint32_t me_index = smp_hart_index();
    uint32_t best;
    /* An isolated hart takes nothing at all, which is what isolating it
     * means. It still idles in the scheduler rather than being parked, so it
     * can be given work again without being restarted. */
    if ((isolated & (1ull << me_index)) != 0) {
        return SCHED_NO_THREAD;
    }
    best = claim_rt(cursor, me_index);
    if (best == SCHED_NO_THREAD) {
        best = claim_fair(cursor, me_index);
    }
    if (best != SCHED_NO_THREAD) {
        cursor = (best + 1u) % SCHED_MAX_THREADS;
        threads[best].state = THREAD_RUNNING;
    }
    return best;
}

/* Wakes the harts that need to look again.
 *
 * Two kinds, and the second is easy to forget. An **idle** hart is asleep
 * in `wfi` and has no clock at all; it must be poked or it sleeps for ever.
 * A hart **running a thread with no timer** is not asleep, but it is in the
 * same position: it disarmed because nothing else wanted the hart, and now
 * something does. Without a poke it runs its thread until that thread makes
 * a syscall — which a tight loop never does.
 *
 * That second case is the spec's "or should preempt", and leaving it out
 * made a Go runtime hang on its own first thread: the thread ran, nothing
 * else could, and nothing was ever going to interrupt it.
 *
 * **Required, not an optimisation.** With a periodic tick an idle hart woke
 * a hundred times a second and noticed new work by accident; with dynticks
 * it has no timer at all, so the only thing that can end its `wfi` is
 * another hart telling it to look. A scheduler that makes a thread runnable
 * and does not poke is a scheduler whose thread may never run.
 *
 * Only harts that are actually idle, because an IPI is **1499 ns** on this
 * machine — thirty context switches — and poking a hart that is already
 * busy buys nothing at all (docs/scheduler-phase0.md). */
static void poke_idle_harts(void) {
    uint64_t mask = 0;
    uint32_t base = 0;
    bool any = false;
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        bool wants_looking = harts[i].idle || harts[i].untimed;
        if (!harts[i].joined || !wants_looking || i == smp_hart_index()) {
            continue;
        }
        {
            const struct smp_hart *h = smp_hart(i);
            if (h == NULL) {
                continue;
            }
            if (!any) {
                base = (uint32_t)h->id;
                any = true;
            }
            if (h->id >= base && h->id - base < 64u) {
                mask |= 1ull << (h->id - base);
            }
        }
    }
    if (any) {
        (void)sbi_send_ipi(mask, base);
    }
}

void sched_wake_idle_harts(void) {
    poke_idle_harts();
}

/* How long an idle hart will sleep before looking again, even though
 * nothing asked it to. See `sched_arm_timer`. */
#define IDLE_BACKSTOP_NS 100000000ull

/* Times the backstop fired on a hart that had work waiting — which means a
 * poke went missing, and the machine was saved by a timeout rather than by
 * being correct. */
static uint32_t missed_pokes;
static uint32_t lost_deadlines;

static void report_lost_deadline(const char *where, uint64_t soonest);
static bool armed_somewhere(uint64_t deadline);
static bool armed_covering(uint64_t deadline, uint32_t except);

uint32_t sched_missed_pokes(void) {
    return missed_pokes;
}

uint32_t sched_lost_deadlines(void) {
    return lost_deadlines;
}

uint64_t sched_affinity(void) {
    const struct thread *t = running_thread();
    return t != NULL ? placeable(t) : ~isolated;
}

/* Sets the running thread's affinity.
 *
 * Refused when the mask leaves nowhere to run, as Linux refuses it: a thread
 * pinned to a hart that does not exist, or to one the machine has reserved,
 * would simply never be chosen again — a hang that looks like the scheduler
 * losing a thread rather than like a bad argument. The caller is told
 * instead, and keeps the affinity it had. */
bool sched_set_affinity(uint64_t mask) {
    struct thread *t = running_thread();
    uint64_t exists = smp_hart_count() >= 64u ? ~0ull : (1ull << smp_hart_count()) - 1u;
    if (t == NULL || (mask & exists & ~isolated) == 0) {
        return false;
    }
    t->harts = mask & exists;
    /* **Moved now, not at its next slice.** If this hart is no longer
     * allowed, the thread must leave it — otherwise a thread can pin itself
     * away from the hart it is running on and carry on running there, which
     * makes the call look as though it did nothing. */
    if (!may_run_here(t, smp_hart_index())) {
        sched_yield_now();
    }
    return true;
}

/* The running thread's scheduling class and priority. */
/* **Stops every thread of a process, and resumes them.**
 *
 * `SIGSTOP` and `SIGCONT` are not deliverable to a handler — they act on
 * the process itself — so they are the scheduler's business rather than the
 * signal frame's. A thread that is running on another hart is marked and
 * poked; it stops at its next return to user mode, which is the same place
 * every other preemption happens.
 *
 * The stopped state is remembered across what the thread was doing: a
 * thread stopped while blocked on a sleep is `THREAD_STOPPED`, and when the
 * sleep expires it stays stopped, because the wake path moves `BLOCKED` to
 * `RUNNABLE` and this is neither. That is the whole reason it is a state
 * and not a flag.
 *
 * Returns how many threads it moved, so a caller can tell "stopped a
 * process" from "there was nothing to stop". */
uint32_t sched_stop_process(uint32_t pid) {
    uint32_t moved = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].proc == NULL || uspace_pid_of(threads[i].proc) != pid) {
            continue;
        }
        if (threads[i].state == THREAD_FREE || threads[i].state == THREAD_DYING ||
            threads[i].state == THREAD_STOPPED) {
            continue;
        }
        if (threads[i].state == THREAD_RUNNING) {
            /* On a hart now: marked **and interrupted**, so it stops on its
             * way out of a trap that happens because of this rather than
             * whenever one happened to be due. */
            make_it_leave(i);
        }
        threads[i].state = THREAD_STOPPED;
        moved++;
    }
    if (moved != 0) {
        poke_idle_harts();
    }
    return moved;
}

bool sched_signal_pending(void) {
    const struct thread *t = running_thread();
    return t != NULL && (t->signals.pending & ~t->signals.blocked) != 0;
}

bool sched_raise_on_process(uint32_t pid, uint32_t signo) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_FREE || threads[i].state == THREAD_DYING ||
            threads[i].proc == NULL || uspace_pid_of(threads[i].proc) != pid) {
            continue;
        }
        signal_raise_on(&threads[i].signals, signo, 0, 0);
        /* A thread that is blocked or stopped has to be made runnable or the
         * signal waits for an event that a stopped process will not see. */
        if (threads[i].state == THREAD_BLOCKED || threads[i].state == THREAD_STOPPED) {
            place_on_runqueue(i);
            poke_idle_harts();
        }
        return true;
    }
    return false;
}

uint32_t sched_continue_process(uint32_t pid) {
    uint32_t moved = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state != THREAD_STOPPED || threads[i].proc == NULL ||
            uspace_pid_of(threads[i].proc) != pid) {
            continue;
        }
        /* Placed rather than given its old virtual clock: it has been
         * stopped for an unbounded time, and a thread that far behind would
         * take the processor until it caught up. */
        place_on_runqueue(i);
        moved++;
    }
    if (moved != 0) {
        poke_idle_harts();
    }
    return moved;
}

uint32_t sched_rt_priority(void) {
    const struct thread *t = running_thread();
    return t != NULL ? t->rt_priority : 0u;
}

bool sched_rt_is_round_robin(void) {
    const struct thread *t = running_thread();
    return t != NULL && t->rt_round_robin;
}

/* Puts the running thread in the real-time class, or takes it out.
 *
 * A priority of 0 returns it to the fair class, and its virtual clock is
 * **placed** rather than kept: a thread that has been real-time has not been
 * accumulating virtual time anybody can compare, so handing it back its old
 * clock would make it either immortal or instantly starved depending on how
 * long it was away.
 *
 * Entering the class **yields**, because the decision has already been
 * made by the time this returns: a thread that raises itself above the one
 * it is sharing a hart with should be running, and the way a thread gets
 * onto a hart here is by going through the scheduler. */
bool sched_set_rt(uint32_t priority, bool round_robin) {
    struct thread *t = running_thread();
    if (t == NULL || priority > SCHED_RT_PRIORITY_MAX) {
        return false;
    }
    t->rt_priority = priority;
    t->rt_round_robin = round_robin;
    if (priority == 0) {
        t->vruntime = fair_wake_vruntime(0, min_vruntime(), SCHED_WAKE_CREDIT_NS);
    }
    sched_yield_now();
    return true;
}

int32_t sched_nice(void) {
    const struct thread *t = running_thread();
    return t != NULL ? t->nice : 0;
}

bool sched_set_nice(int32_t nice) {
    struct thread *t = running_thread();
    if (t == NULL) {
        return false;
    }
    t->nice = fair_clamp_nice(nice);
    t->weight = fair_weight_of_nice(t->nice);
    /* The virtual clock is **left alone**. It records what this thread has
     * had, which does not change because its weight did; re-basing it here
     * would hand a thread a fresh claim on the processor every time it
     * renices, which is a way to get more than its share by asking twice. */
    return true;
}

/* How many threads could run right now, counting the one that is. Two or
 * more is what makes a slice mean anything. */
/* Threads **waiting for a hart**, which is not the same question as
 * `runnable_count`: that one counts the running ones too, because it is
 * asked to decide whether the running thread has competition. A lost
 * wake-up is about work nobody has picked up, so it counts only the
 * waiting. Counting the running ones here made every idle hart's legitimate
 * timer interrupt look like a lost poke — 196 of them in a run that had
 * none. */
static uint32_t waiting_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        n += threads[i].state == THREAD_RUNNABLE ? 1u : 0u;
    }
    return n;
}

/* The smallest virtual clock among threads that want to run, or 0 if none
 * do. A waking thread is placed relative to this, which is what stops it
 * either monopolising the processor to catch up or being handed a fresh
 * claim every time it sleeps. */
static uint64_t min_vruntime(void) {
    uint64_t least = 0;
    bool any = false;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state != THREAD_RUNNABLE && threads[i].state != THREAD_RUNNING) {
            continue;
        }
        if (!any || threads[i].vruntime < least) {
            least = threads[i].vruntime;
            any = true;
        }
    }
    return least;
}

/* What the runnable threads weigh between them, which is the denominator of
 * every slice. Saturated rather than allowed to wrap: 64 threads at the
 * heaviest weight is 5.6 million, far inside 32 bits, so this is a guard
 * against a corrupt weight rather than against arithmetic. */
static uint32_t total_weight(void) {
    uint64_t sum = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_RUNNING) {
            sum += threads[i].weight == 0 ? FAIR_NICE0_WEIGHT : threads[i].weight;
        }
    }
    return sum > 0xffffffffull ? 0xffffffffu : (uint32_t)sum;
}

static uint32_t runnable_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        n += threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_RUNNING ? 1u : 0u;
    }
    return n;
}

/* The soonest deadline any blocked thread is waiting for, or 0 when none
 * is. What an idle hart should set its timer to. */
static uint64_t earliest_deadline(void) {
    uint64_t soonest = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        uint64_t at = threads[i].futex_deadline;
        if (threads[i].state != THREAD_BLOCKED || at == 0) {
            continue;
        }
        soonest = soonest == 0 || at < soonest ? at : soonest;
    }
    return soonest;
}

/* Gives the hart back to its scheduling loop. The caller has already set
 * its own state, and the lock is held across the switch — handed on, in the
 * sense `klock.h` describes: the loop resumes owning what this thread
 * owned. */
/* Whether a thread's kernel stack still has room below it. Reported rather
 * than silently tolerated: the first overflow is the only honest moment to
 * say so, because everything after it is a consequence. */
static uint32_t stack_overflows;

/* Threads refused the processor because their saved stack pointer was not
 * in their own kernel stack. Always a bug, and never the thread's. */
static uint32_t bad_resumes;

uint32_t sched_bad_resumes(void) {
    return bad_resumes;
}

/* **Is this thread's saved stack pointer its own?** Asked on the way *in*,
 * which is the side nobody was checking.
 *
 * `slot_is_still_ours` asks the same question of the thread on its way out,
 * and that is where the slot-alias bug was caught. The other direction had
 * no check at all — and resuming a thread whose `ksp` points somewhere else
 * restores a register set from whatever is at that address. If that is a
 * kernel stack, the registers restored are a *kernel* context, and the
 * `sret` that follows puts a kernel program counter and a kernel stack
 * pointer into user mode. That is exactly the frame
 * `uspace_impossible_frames` caught once on the board: `sepc` inside the
 * kernel's own text, `sp` in its identity map, reported as a fault in user
 * code long after the switch that caused it.
 *
 * A thread that fails this is **not run**. The alternative is to run it and
 * watch the machine misbehave somewhere else, which is what has been
 * happening. */
static bool resume_is_sane(uint32_t t) {
    uint64_t base = threads[t].kstack;
    uint64_t sp = threads[t].ksp;
    /* **Is anybody else already running it?**
     *
     * Two harts in one thread is the shape of every symptom this race has:
     * both restore the same register set, both run the same code, and their
     * writes to the thread's own memory interleave — which from outside is a
     * computation that produced a number nobody computed. The claim is made
     * under the kernel lock and should make this impossible, which is
     * exactly why it is worth asserting rather than assuming. */
    for (uint32_t h = 0; h < SMP_MAX_HARTS; h++) {
        if (h == smp_hart_index() || harts[h].current != t) {
            continue;
        }
        bad_resumes++;
        kprintf("\nsched: **tid %llu is already running on hart %u** (this is hart %u)\n",
                (unsigned long long)threads[t].tid, h, smp_hart_index());
        return false;
    }
    if (base == 0) {
        return true; /* the canary has already reported this one */
    }
    if (sp > base && sp <= base + KSTACK_PAGES * PAGE_SIZE) {
        return true;
    }
    bad_resumes++;
    kprintf("\nsched: **tid %llu cannot be resumed**: ksp %016llx is not in its stack"
            " [%016llx, %016llx) — state %u\n",
            (unsigned long long)threads[t].tid, (unsigned long long)sp,
            (unsigned long long)base, (unsigned long long)(base + KSTACK_PAGES * PAGE_SIZE),
            (unsigned)threads[t].state);
    return false;
}

static void check_kernel_stack(uint32_t t) {
    if (threads[t].kstack == 0 ||
        *(volatile uint64_t *)(uintptr_t)threads[t].kstack == KSTACK_CANARY) {
        return;
    }
    stack_overflows++;
    kprintf("sched: **tid %llu overflowed its kernel stack** (%u pages)\n",
            (unsigned long long)threads[t].tid, KSTACK_PAGES);
    threads[t].kstack = 0; /* say it once per thread */
}

uint32_t sched_stack_overflows(void) {
    return stack_overflows;
}

/* Threads found to be standing on a stack that is no longer theirs. */
static uint32_t slot_aliases;

uint32_t sched_slot_aliases(void) {
    return slot_aliases;
}

/* The invariant that matters when a thread saves its stack pointer: **the
 * slot it is about to write into is still its own**.
 *
 * If another hart has taken the slot — because it was marked free too early
 * — then writing `ksp` there hands the dead thread's stack to the live
 * thread that now owns the slot, and the machine carries on with two
 * different ideas of what is running. The check is one comparison: the
 * stack pointer this thread is using must lie inside the kernel stack the
 * slot records, and a reused slot has a different allocation. */
static bool slot_is_still_ours(uint32_t was) {
    uint64_t sp;
    uint64_t base = threads[was].kstack;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    if (base == 0) {
        return true; /* the canary already reported this one */
    }
    return sp > base && sp <= base + KSTACK_PAGES * PAGE_SIZE;
}

static void yield_to_scheduler(void) {
    struct hartstate *h = me();
    uint32_t was = h->current;
    if (!slot_is_still_ours(was)) {
        slot_aliases++;
        kprintf("sched: **slot %u is no longer the stack this thread is on**\n", was);
    }
    check_kernel_stack(was);
    /* The floating-point registers go with the thread. Saved here, where
     * the thread stops running, and restored where it starts again. */
    fp_save(threads[was].fp);
    h->current = SCHED_NO_THREAD;
    h->switches++;
    context_switch(&threads[was].ksp, h->idle_ksp);
}

/* This hart has stopped running a thread: releases the slot if the thread
 * ended, and says so if anybody is waiting for it to.
 *
 * The slot is released **here and not where the thread ended**, which is
 * the whole of `THREAD_DYING`: control has to have left the thread's kernel
 * stack first, or another hart may hand the slot to a new thread while the
 * old one is still standing on it.
 *
 * The wake is for `sched_await_siblings`, and it is **only for a thread
 * that ended**. Waking on every switch instead — which is where this
 * started — keeps the waiting thread runnable continuously, so the hart
 * never idles, and an idle hart is the only thing that releases the kernel
 * lock. The hart that needs that lock is the one with the thread to let go,
 * so the result was not a slow wait but a livelock: measured, the wait
 * never completed at all. */
static void let_go_of(struct hartstate *h, uint32_t t) {
    bool ended = threads[t].state == THREAD_DYING;
    if (ended) {
        threads[t].state = THREAD_FREE;
    }
    h->current = SCHED_NO_THREAD;
    if (ended) {
        sibling_wakes += sched_wake_on(&sibling_channel, SCHED_MAX_THREADS);
    }
}

/* One hart's scheduling loop: claim a thread, run it, repeat until the
 * process is over.
 *
 * Every hart runs this, including the one that started the process — which
 * is why there is no special case for the first thread any more. A hart
 * with nothing to claim waits for an interrupt with the lock released,
 * because the thing that will give it work is another hart inside the
 * kernel.
 *
 * Runs to 28 lines: the claim, the switch and the idle wait are one
 * sequence whose middle cannot be named honestly — "run_the_thread" would
 * hide that control leaves and comes back on the same line (Rule 11,
 * Naming). */
void sched_run(void) {
    struct hartstate *h;
    bool took = klock_enter();
    h = me();
    h->joined = true;
    /* **Off by default, and the reason is recorded rather than guessed.**
     *
     * Taking the timer away from its periodic schedule works — one-shot
     * deadlines, no re-arm when the deadline has not changed, a poke
     * instead of a tick — and it exposes something the periodic tick had
     * been hiding: a Go runtime stops four syscalls into its startup, in a
     * fault loop that a hundred interrupts a second was evidently kicking
     * along. A tick that papers over a fault loop is not a tick worth
     * keeping, but neither is a kernel that only runs because of one.
     *
     * So the machinery is here and `minikern.dynticks` turns it on, which
     * is how the fault loop will be found; the default stays periodic until
     * it is. docs/PREEMPTION_MODEL.md says what is built and what is not. */
    if (dynticks_wanted) {
        timer_take_over();
    }
    while (!process_over) {
        uint32_t t = claim_runnable();
        if (t == SCHED_NO_THREAD) {
            /* Arm this hart for the earliest deadline anything is waiting
             * on, before idling. `timer_wake_at` arms the hart that *asks*,
             * and the hart that ends up idle is often a different one — so
             * without this the deadline is registered on a comparator that
             * is busy and the sleeper waits for the next ordinary tick
             * instead. It is the difference between a 200-microsecond
             * average lateness and a 9-millisecond worst case. */
            /* An idle hart still wants waking for a sleeper's deadline,
             * and for nothing else. */
            sched_arm_timer();
            h->idle = true;
            klock_idle_wait();
            h->idle = false;
            continue;
        }
        if (!resume_is_sane(t)) {
            /* Left alone rather than run: its slot is not released either,
             * because something still believes it owns that memory and
             * handing it out again would spread the damage. */
            threads[t].state = THREAD_BLOCKED;
            continue;
        }
        h->current = t;
        threads[t].last_hart = smp_hart_index();
        h->slice_started = timer_ns();
        /* **This hart's address space must be the thread's.** With one
         * process it never changed, so nothing noticed it was not being
         * checked; with two, a hart that skips this runs one process's code
         * against another's page tables. */
        if (h->loaded != threads[t].proc) {
            uspace_adopt_address_space(threads[t].proc);
            h->loaded = threads[t].proc;
        }
        fp_restore(threads[t].fp);
        /* Armed for whichever comes first — this thread's slice, a
         * sleeper's deadline, or housekeeping — or disarmed entirely when
         * nothing is due. */
        sched_arm_timer();
        /* Into the thread, and back here when it yields, blocks or ends.
         * `idle_ksp` is written before the switch because the thread will
         * use it to come back. */
        context_switch(&h->idle_ksp, threads[t].ksp);
        charge_vruntime(t, h->slice_started);
        check_kernel_stack(t);
        /* Back on this hart's own stack, so the thread that just left is no
         * longer standing on anything: its slot can be reused now and not
         * before. */
        let_go_of(h, t);
        h->preempt_pending = false;
    }
    h->joined = false;
    klock_exit(took);
}

void sched_yield_now(void) {
    struct thread *t = running_thread();
    if (t == NULL) {
        return;
    }
    /* **Only a thread that is still allowed to run becomes runnable
     * again.** This set `THREAD_RUNNABLE` unconditionally, and the
     * preemption path is exactly where a thread that has been ended or
     * stopped finds out about it — so the assignment undid the thing being
     * asked of it. A `THREAD_DYING` thread came back as runnable and was
     * re-claimed, which is why a thread told to leave its hart appeared to
     * leave for a moment and then carry on; and a `THREAD_STOPPED` one
     * resumed in spite of its `SIGSTOP`, which had never been seen because
     * no test had stopped a thread that was running on another hart.
     *
     * Measured before the fix: the kernel waited 6,179 µs for a thread to
     * leave a hart it was never going to leave, and only got its answer
     * from the window between a yield and the next claim. */
    if (t->state == THREAD_RUNNING) {
        t->state = THREAD_RUNNABLE;
    }
    yield_to_scheduler();
}

bool sched_futex_wait(virt_t addr, uint64_t deadline_ns) {
    struct thread *t = running_thread();
    if (t == NULL) {
        return false;
    }
    t->futex_addr = addr;
    t->wait_chan = NULL;
    t->futex_deadline = deadline_ns;
    t->woken = false;
    t->state = THREAD_BLOCKED;
    trace("waits on", me()->current, addr);
    if (deadline_ns != 0) {
        timer_wake_at(deadline_ns);
    }
    /* **Always** give the hart up, even when nothing else is runnable.
     *
     * An earlier version spun on the clock here in that case, to avoid
     * sleeping on a one-hart machine with nothing to wake it — and it did
     * that **holding the kernel lock**, which shut every other hart out of
     * the kernel for the whole timeout. The symptom was a `time.Sleep(50ms)`
     * taking 940 ms on four harts and 55 ms on one: the sleeping hart was
     * not slow, it was holding the thing every other hart needed.
     *
     * Giving the hart up instead lets its scheduling loop idle properly,
     * which releases the lock across the wait. A machine with nothing
     * runnable and nothing with a deadline is a deadlock in the program, and
     * `report_deadlock_once` says so rather than hanging silently. */
    yield_to_scheduler();
    /* Back here only because something made this thread runnable again:
     * a wake, or the tick noticing the deadline. */
    t->futex_addr = 0;
    return t->woken;
}

/* Sleeps until a deadline, with nothing to be woken by.
 *
 * It is a blocked thread with a deadline and **no address**, so no `futex`
 * wake can match it: `futex_addr` is 0 and a wake on address 0 is refused
 * before it reaches the scheduler. The tick is what ends it.
 *
 * It exists because the alternative is a spin. `nanosleep` and an idle
 * runtime's `epoll_pwait` both used to loop on the clock calling
 * `sched_yield_now`, which on four harts produced **four and a half million
 * thread switches** for a program that ran for 56 milliseconds — correct,
 * and almost all of it the scheduler talking to itself. A sleeping thread
 * costs one switch. */
bool sched_sleep_until(uint64_t deadline_ns) {
    struct thread *t = running_thread();
    uint64_t late;
    if (t == NULL || deadline_ns == 0) {
        return false;
    }
    t->futex_addr = 0;
    t->wait_chan = NULL;
    t->futex_deadline = deadline_ns;
    t->woken = false;
    t->state = THREAD_BLOCKED;
    /* Ask for an interrupt at the deadline rather than waiting for the next
     * tick. Without this a 20-microsecond sleep costs up to 10 milliseconds,
     * and a runtime that makes a hundred of them in a row pays for every
     * one. */
    timer_wake_at(deadline_ns);
    yield_to_scheduler();
    /* How late the wake-up was, which is the kernel's own contribution to a
     * sleep and the only way to tell it apart from a runtime that chose not
     * to sleep. The tick is 10 ms, so a few milliseconds late is the floor;
     * anything much larger is this kernel's fault and not the program's. */
    late = timer_ns() > deadline_ns ? timer_ns() - deadline_ns : 0u;
    sleeps++;
    late_total += late;
    worst_late = late > worst_late ? late : worst_late;
    return true;
}

void sched_sleep_report(uint64_t *count, uint64_t *average_late_us, uint64_t *worst_late_us) {
    *count = sleeps;
    *average_late_us = sleeps == 0 ? 0u : late_total / sleeps / 1000u;
    *worst_late_us = worst_late / 1000u;
}

uint32_t sched_futex_wake(virt_t addr, uint32_t count) {
    uint32_t woke = 0;
    bool poked = false;
    /* Logged even when it matches nothing. A wake with no waiter is the
     * normal case for a futex — the other side had not got there yet — but
     * it is also exactly what a *lost* wakeup looks like, and the two can
     * only be told apart by seeing both sides. */
    trace("wakes", me()->current, addr);
    if (addr == 0) {
        return 0; /* no futex lives at address 0; a sleeper's slot does */
    }
    for (uint32_t i = 0; i < SCHED_MAX_THREADS && woke < count; i++) {
        if (threads[i].state == THREAD_BLOCKED && threads[i].wait_chan == NULL &&
            threads[i].futex_addr == addr) {
            place_on_runqueue(i);
            threads[i].woken = true;
            threads[i].futex_addr = 0;
            woke++;
            poked = true;
            trace("woken on", i, addr);
        }
    }
    if (poked) {
        poke_idle_harts();
    }
    return woke;
}

int sched_wait_on(const void *chan, uint64_t deadline_ns) {
    struct thread *t = running_thread();
    if (t == NULL || chan == NULL) {
        return KERR_INVAL;
    }
    t->futex_addr = 0;
    t->wait_chan = chan;
    t->futex_deadline = deadline_ns;
    t->woken = false;
    t->state = THREAD_BLOCKED;
    if (deadline_ns != 0) {
        timer_wake_at(deadline_ns);
    }
    yield_to_scheduler();
    t->wait_chan = NULL;
    return t->woken ? KERR_OK : KERR_TIMEDOUT;
}

uint32_t sched_wake_on(const void *chan, uint32_t max) {
    uint32_t woke = 0;
    if (chan == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < SCHED_MAX_THREADS && woke < max; i++) {
        if (threads[i].state == THREAD_BLOCKED && threads[i].wait_chan == chan) {
            place_on_runqueue(i);
            threads[i].woken = true;
            threads[i].wait_chan = NULL;
            woke++;
        }
    }
    if (woke > 0) {
        poke_idle_harts();
    }
    return woke;
}

uint32_t sched_live_threads(void) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        live += threads[i].state != THREAD_FREE && threads[i].state != THREAD_DYING ? 1u : 0u;
    }
    return live;
}

uint32_t sched_live_threads_of(const struct uproc *p) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        live += threads[i].state != THREAD_FREE && threads[i].state != THREAD_DYING &&
                        threads[i].proc == p
                    ? 1u
                    : 0u;
    }
    return live;
}

uint64_t sched_current_tid(void) {
    struct thread *t = running_thread();
    return t == NULL ? 0 : t->tid;
}

struct uproc *sched_current_proc(void) {
    struct thread *t = running_thread();
    return t == NULL ? NULL : t->proc;
}

struct thread_signals *sched_current_signals(void) {
    struct thread *t = running_thread();
    return t == NULL ? NULL : &t->signals;
}

struct thread_signals *sched_signals_of(uint64_t tid) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state != THREAD_FREE && threads[i].tid == tid) {
            return &threads[i].signals;
        }
    }
    return NULL;
}

uint32_t sched_hart_switches(uint32_t hart_index) {
    return hart_index < SMP_MAX_HARTS ? (uint32_t)harts[hart_index].switches : 0u;
}

void sched_exit_thread(int64_t status) {
    struct thread *t = running_thread();
    if (t == NULL) {
        return;
    }
    trace("ends with status", me()->current, (uint64_t)status);
    /* Not free yet: the slot is released by the scheduling loop, once the
     * switch below has actually left this stack. */
    t->state = THREAD_DYING;
    /* Was it the last thread **of its process**? Then that process is over.
     * The status goes to whoever is waiting for it — its parent, or
     * `uspace_run` — which may be on a different hart, which is why it is
     * recorded rather than returned. */
    if (sched_live_threads_of(t->proc) == 0) {
        uspace_exit(status);
    }
    /* Its kernel stack is still being stood on, so it is freed by
     * `sched_shutdown` rather than here. The switch away never comes back,
     * and the `ksp` it writes goes to a slot that is still this thread's —
     * which is the point of `THREAD_DYING`. */
    yield_to_scheduler();
}

void sched_exit_proc(struct uproc *p, bool is_root) {
    struct thread *t = running_thread();
    /* Only the harts' loops stop, and only for the process they were
     * started for. A child ending must leave its parent running. */
    process_over = process_over || is_root;
    /* **And every idle hart told to look.** The loop's exit condition is
     * only read after a wake, so a hart asleep in `wfi` learns the process
     * is over when something interrupts it — and with nothing else due, that
     * was whatever tick happened to come next, or nothing at all. The hart
     * then stayed in the scheduler, and `uspace_run` waits for every hart to
     * leave before freeing what they stand on: the machine stopped with no
     * output, which is how this was found. It is the same rule this
     * scheduler already follows for work appearing, applied to work running
     * out. */
    if (process_over) {
        poke_idle_harts();
    }
    /* Every thread **of this process**, not every thread: one left RUNNABLE
     * would be claimed by a hart whose loop has not noticed yet, and one
     * belonging to another process must not be touched at all.
     *
     * **And not all of them freed.** This marked every slot `THREAD_FREE`,
     * including one running on another hart — the slot-alias bug that
     * `THREAD_DYING` exists for, in its third location: a free slot is one
     * `find_free_slot` may hand to a new thread while the old one is still
     * standing on its kernel stack, and the old thread then writes the stack
     * pointer it is leaving into what is now somebody else's slot. The
     * running ones are told to leave and release their own slots; the rest
     * are given up here. */
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].proc != p || &threads[i] == t) {
            continue;
        }
        if (on_a_hart(i)) {
            threads[i].state = THREAD_DYING;
            make_it_leave(i);
        } else {
            abandon(&threads[i]);
        }
    }
    if (t != NULL && t->proc == p) {
        /* This thread is on this hart, so its slot is released by the
         * scheduling loop once the switch has left its stack. */
        t->state = THREAD_DYING;
        yield_to_scheduler();
    }
}

/* How many harts a process may run on. The harts that reached the kernel,
 * which is what the scheduler can actually hand a thread to — not the
 * number in the device tree, because a hart the firmware refused to start
 * is not a CPU this machine has. */
uint32_t sched_cpus(void) {
    uint32_t n = smp_running_count();
    return n == 0 ? 1u : (n > SMP_MAX_HARTS ? SMP_MAX_HARTS : n);
}

/* Says which harts are still in the scheduling loop and what each is
 * running.
 *
 * `uspace_run` waits for every hart to leave before freeing anything they
 * might be standing on, and that wait is unbounded by design — there is
 * nothing better to do than wait. But a hart that never leaves makes the
 * machine stop with **no output at all**, which is the least diagnosable
 * state this kernel has: it looks exactly like a wedged board. One print of
 * who is still there turns it into a report. */
void sched_report_joined(void) {
    uint32_t next = 0, serving = 0;
    uint64_t owner = 0;
    klock_state(&next, &serving, &owner);
    kprintf("sched: over %u; klock next %u serving %u owner %llu\n", process_over ? 1u : 0u, next,
            serving, (unsigned long long)owner);
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        uint32_t t = harts[i].current;
        if (!harts[i].joined) {
            continue;
        }
        kprintf("sched: hart %u joined, idle %u, armed %llu ns, tid %llu state %u\n", i,
                harts[i].idle ? 1u : 0u, (unsigned long long)timer_armed_ns_of(i),
                t == SCHED_NO_THREAD ? 0ull : (unsigned long long)threads[t].tid,
                t == SCHED_NO_THREAD ? 0u : (unsigned)threads[t].state);
    }
}

uint32_t sched_joined_harts(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        n += harts[i].joined ? 1u : 0u;
    }
    return n;
}

bool sched_wants_harts(void) {
    return accepting_harts;
}

void sched_set_accepting(bool on) {
    accepting_harts = on;
}

/* Whether anything could still make progress: a runnable thread, or one
 * blocked with a deadline the clock will reach. */
static bool something_can_progress(void) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_RUNNABLE || threads[i].state == THREAD_RUNNING) {
            return true;
        }
        if (threads[i].state == THREAD_BLOCKED && threads[i].futex_deadline != 0) {
            return true;
        }
    }
    return false;
}

/* Reports a deadlock instead of hanging on it.
 *
 * Every thread blocked with no deadline means nothing will ever wake any of
 * them: a program that has deadlocked, or a kernel that lost a wake-up. The
 * ARM project needed a flag to find exactly this, because a lost wakeup is
 * indistinguishable from a wedged machine from outside — so this is on by
 * default and says which it was. Ten seconds, because a short threshold
 * would fire on a machine that is merely slow. */
#define DEADLOCK_TICKS 1000u

/* How long past its deadline a blocked thread has to be before it counts as
 * a wake-up that was lost rather than one that is late. A second is three
 * orders of magnitude past anything this kernel schedules. */
#define OVERDUE_NS 1000000000ull

static uint32_t overdue_wakes;

uint32_t sched_overdue_wakes(void) {
    return overdue_wakes;
}

/* Reports a thread whose deadline has long passed and which is still
 * blocked.
 *
 * **The gap the deadlock detector leaves.** That one only fires when every
 * thread is blocked with *no* deadline, because a deadline means something
 * will happen — so a thread blocked on a deadline the clock went past is
 * invisible to it, and the machine hangs in total silence with
 * `something_can_progress` answering "yes" for ever. That is exactly the
 * shape of a lost wake-up, and it is what a timed wait is for: finding it
 * cost several board runs where the only evidence was no output at all.
 *
 * It complements `sched_lost_deadlines`, which asks whether a deadline is
 * *armed* on some hart. This asks whether one that was armed ever
 * **arrived**, which is a different failure and the one that hangs. */
static void watch_for_overdue_wakes(uint64_t now) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        uint64_t at = threads[i].futex_deadline;
        if (threads[i].state != THREAD_BLOCKED || at == 0 || now < at + OVERDUE_NS) {
            continue;
        }
        overdue_wakes++;
        kprintf("\nsched: **tid %llu is %llu ms past its deadline and still blocked**"
                " (waiting on %llx)\n",
                (unsigned long long)threads[i].tid, (unsigned long long)((now - at) / 1000000u),
                (unsigned long long)(uintptr_t)threads[i].wait_chan);
        /* Woken, because the report is worth more than the hang and a
         * spurious wake is something every caller of a timed wait already
         * has to handle. */
        place_on_runqueue(i);
        threads[i].woken = false;
    }
}

static void watch_for_deadlock(void) {
    if (something_can_progress()) {
        idle_ticks = 0;
        return;
    }
    if (sched_live_threads() == 0 || deadlock_reported) {
        return;
    }
    if (++idle_ticks < DEADLOCK_TICKS) {
        return;
    }
    deadlock_reported = true;
    kprintf("\nsched: every one of %u threads is blocked with no deadline — nothing can wake"
            " them, so this is a deadlock rather than a slow machine\n",
            sched_live_threads());
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_BLOCKED) {
            kprintf("sched:   tid %llu waits on %llx\n", (unsigned long long)threads[i].tid,
                    (unsigned long long)threads[i].futex_addr);
        }
    }
}

static uint32_t ticks_seen;
static uint32_t deadline_wakes;

uint32_t sched_deadline_wakes(void) {
    return deadline_wakes;
}

uint32_t sched_ticks_seen(void) {
    return ticks_seen;
}

/* When this hart's running thread must give way, or 0 when nothing says it
 * must. */
/* When the running thread's turn ends.
 *
 * Its share of the target period, by weight — so a nice -5 thread beside a
 * nice 0 one gets the larger slice as well as being chosen more often. The
 * floor is the old fixed quantum: below it a slice costs more in switching
 * than it delivers, so the period stretches instead (`fair_slice_ns`).
 *
 * Zero when nothing else is runnable, which is what dynamic ticks need: a
 * thread with no competition is not taking a turn from anybody and there is
 * nothing to interrupt it for. */
/* Whether another runnable real-time thread shares this one's priority —
 * the only thing a round-robin quantum is for. */
static bool rt_peer_waiting(uint32_t slot) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (i != slot && threads[i].state == THREAD_RUNNABLE &&
            threads[i].rt_priority == threads[slot].rt_priority) {
            return true;
        }
    }
    return false;
}

/* Whether any runnable real-time thread outranks the one in `slot`. A fair
 * thread is outranked by every real-time thread. */
static bool outranked(uint32_t slot) {
    uint32_t mine = threads[slot].rt_priority;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_RUNNABLE && threads[i].rt_priority > mine) {
            return true;
        }
    }
    return false;
}

static uint64_t slice_deadline(const struct hartstate *h) {
    uint64_t slice;
    if (h->current == SCHED_NO_THREAD) {
        return 0;
    }
    /* **A real-time thread is not on a clock.**
     *
     * `SCHED_FIFO` runs until it blocks or yields — a slice would be the
     * proportional share the class exists to escape. `SCHED_RR` gets a
     * quantum, but only against another real-time thread of its *own*
     * priority: being preempted for a lower-priority one would invert the
     * order the class promises.
     *
     * Either way, a higher-priority thread becoming runnable preempts it,
     * and that happens on the wake rather than on a timer — see
     * `preempt_for_rt`. Nothing here needs to poll for it. */
    if (threads[h->current].rt_priority != 0) {
        if (!threads[h->current].rt_round_robin || !rt_peer_waiting(h->current)) {
            return 0;
        }
        return h->slice_started + SCHED_RR_QUANTUM_NS;
    }
    if (runnable_count() < 2u) {
        return 0;
    }
    slice = fair_slice_ns(threads[h->current].weight, total_weight(),
                          SCHED_TARGET_LATENCY_NS, SCHED_SLICE_NS);
    return h->slice_started + slice;
}

/* **The invariant that makes a lost deadline loud.**
 *
 * With a one-shot timer, every sleeper in the system depends on some hart
 * holding a comparator value at or before its deadline. If none does, the
 * machine is not slow — it is stopped, and nothing will restart it. That is
 * a hang with no output, which is the hardest kind of bug there is.
 *
 * So it is checked rather than reasoned about: whenever a hart is about to
 * stop looking (entering `wfi`, or running a thread with no timer of its
 * own), the soonest deadline anybody waits for must be armed somewhere. The
 * tolerance is one granularity, because arming rounds up.
 *
 * Reported once and counted, not panicked: the report is what a run leaves
 * behind, and a kernel that stops on the first one cannot show how often it
 * happens or whether a fix moved the number.
 */
static void check_deadline_is_armed(const char *where) {
    uint64_t soonest = earliest_deadline();
    uint64_t chores = timer_housekeeping_due();
    /* The beat counts: a lost one is a polled device that has stopped and a
     * protocol whose timers never fire, which is as silent as a lost sleep. */
    if (chores != 0 && (soonest == 0 || chores < soonest)) {
        soonest = chores;
    }
    if (soonest == 0 || armed_somewhere(soonest)) {
        return;
    }
    report_lost_deadline(where, soonest);
}

/* The report half of the check above, separate so that the check itself is
 * the condition and nothing else. */
static void report_lost_deadline(const char *where, uint64_t soonest) {
    lost_deadlines++;
    if (lost_deadlines > 3u) {
        return;
    }
    kprintf("sched: LOST DEADLINE at %s: soonest %llu ns, now %llu ns, armed:",
            where, (unsigned long long)soonest, (unsigned long long)timer_ns());
    for (uint32_t h = 0; h < SMP_MAX_HARTS; h++) {
        kprintf(" %u=%llu", h, (unsigned long long)timer_armed_ns_of(h));
    }
    kprintf("\n");
}

/* **When housekeeping is next due, for the one hart that owns it.**
 *
 * Zero for every other hart, and zero when nothing has registered any — a
 * machine with no polled device pays no periodic interrupt at all, which is
 * most of why a single busy thread can run for ten seconds undisturbed.
 *
 * Hart index 0 owns it. Any fixed choice would do; what matters is that it
 * is **one** hart, because the whole point is not paying the beat four
 * times. An idle hart is the ideal owner and this one usually is: it has no
 * thread to interrupt, so the interrupt costs a wake from `wfi` rather than
 * a thread's slice. */
/* **The beat, but only for a hart that has stopped anyway.**
 *
 * A hart running a thread does not arm for housekeeping: it would charge
 * that thread an interrupt every 10 ms for work the thread did not ask for,
 * and with one runnable thread per hart there is no competition to justify
 * interrupting it at all. An idle hart arms instead — the interrupt costs it
 * a wake from `wfi`, which is what it is for.
 *
 * The exception is a hart that would otherwise be **untimed**: if every hart
 * is running a thread with no competition, nobody is idle, nobody would arm,
 * and the beat would stop. That case arms rather than disarming, so the
 * beat is always somebody's — and `check_deadline_is_armed` says so out loud
 * if this reasoning is ever wrong. */
static uint64_t housekeeping_deadline(const struct hartstate *h) {
    uint64_t chores = timer_housekeeping_due();
    (void)h;
    if (chores == 0) {
        return 0;
    }
    /* **One hart, and whichever one is free to take it.**
     *
     * The rule is only "is anybody already armed for it?" — the first to
     * arrive claims the beat and the rest see it covered, which is what
     * stops every idle hart waking for the same one (measured: 992
     * interrupts each on three idle harts, against 1200 for the machine
     * when one takes it).
     *
     * It used to also require the hart to be **idle**, and that was wrong
     * in the case where no hart is: every hart armed for its own slice,
     * all of those later than the beat, and the beat simply ran late. The
     * invariant found it — 152 reports in one run, each naming a deadline
     * 5 ms sooner than anything armed — which is what the invariant is for
     * and the reason it reports rather than being reasoned about.
     *
     * The cost of the fix is that a busy hart sometimes takes a beat
     * interrupt. That is the right trade: the beat is what drives a polled
     * network and a terminal, and a late beat is a late keystroke. */
    return armed_covering(chores, smp_hart_index()) ? 0u : chores;
}

/* Whether a hart's comparator already covers `deadline`, ignoring hart
 * `except` — pass `SMP_MAX_HARTS` to ask about all of them. The tolerance is
 * one granularity, because arming rounds up.
 *
 * The exclusion matters: a hart asking "does anyone else have this?" must
 * not count its own arm, or it answers yes to itself, stops arming, and the
 * deadline it was holding is lost on the next arm. */
static bool armed_covering(uint64_t deadline, uint32_t except) {
    for (uint32_t i = 0; i < SMP_MAX_HARTS; i++) {
        uint64_t armed = i == except ? 0u : timer_armed_ns_of(i);
        if (armed != 0 && armed <= deadline + TIMER_GRANULARITY_NS) {
            return true;
        }
    }
    return false;
}

static bool armed_somewhere(uint64_t deadline) {
    return armed_covering(deadline, SMP_MAX_HARTS);
}

/* The soonest of the three things that can be due, or 0 for none of them. */
static uint64_t next_deadline(const struct hartstate *h) {
    uint64_t when = slice_deadline(h);
    uint64_t chores = housekeeping_deadline(h);
    uint64_t sleeper = earliest_deadline();
    if (chores != 0 && (when == 0 || chores < when)) {
        when = chores;
    }
    if (sleeper != 0 && (when == 0 || sleeper < when)) {
        when = sleeper;
    }
    return when;
}

/* **Nothing is due: what that means depends on what the hart is doing.**
 *
 * Running a thread with nobody waiting for the hart — genuinely no timer.
 * This is the case Phase 1 measures: one busy task takes *no* timer
 * interrupts, and arming anything here would be the periodic tick back
 * under another name. Measured at one interrupt in ten seconds, against
 * 1001 for the periodic clock.
 *
 * The exception is the beat. If housekeeping is registered and no hart is
 * armed for it, this hart takes it even though it is running a thread —
 * because the alternative is a polled device that stops. That happens only
 * when **no hart is idle**, since an idle one arms for the beat in
 * preference; so the thread pays for it only when every hart is busy, and
 * then only once per beat across the machine.
 *
 * Idle — a backstop, long enough to cost nothing (ten times a second on a
 * hart with nothing to do) and counted, so a missed poke is a number rather
 * than a machine that stopped.
 */
static void stop_the_clock(struct hartstate *h) {
    uint64_t chores = timer_housekeeping_due();
    if (h->current != SCHED_NO_THREAD) {
        if (chores != 0 && !armed_somewhere(chores)) {
            h->backstop = false;
            timer_arm_at(chores);
            return;
        }
        h->untimed = true;
        timer_disarm();
        return;
    }
    h->backstop = true;
    timer_arm_at(timer_ns() + IDLE_BACKSTOP_NS);
}

void sched_arm_timer(void) {
    struct hartstate *h = me();
    uint64_t when;
    /* **Nothing at all unless this scheduler owns the timer.** With the
     * periodic clock still running, arming and disarming here fights it:
     * the comparator holds whatever the periodic schedule last set, and
     * this would overwrite it or mask the interrupt that drives it. One
     * owner at a time. */
    if (!dynticks_wanted) {
        return;
    }
    when = next_deadline(h);
    h->untimed = false;
    if (when == 0) {
        stop_the_clock(h);
        return;
    }
    h->backstop = false;
    timer_arm_at(when);
    check_deadline_is_armed(h->current == SCHED_NO_THREAD ? "idle" : "running");
}

void sched_tick(void) {
    uint64_t now = timer_ns();
    struct hartstate *h = me();
    bool expired = false;
    ticks_seen++;
    /* **The slice ends here, not on a periodic clock.** Without a tick
     * counting down, the only thing that knows a thread has had its turn is
     * this comparison against when the turn started. */
    if (h->current != SCHED_NO_THREAD && outranked(h->current)) {
        h->preempt_pending = true;
    } else if (h->current != SCHED_NO_THREAD) {
        uint64_t ends = slice_deadline(h);
        /* Agreeing with `slice_deadline` rather than re-deriving it: the two
         * used to share a constant, and a per-thread slice is computed. A
         * preemption test that disagreed with the deadline it was armed for
         * would fire early for light threads and late for heavy ones. */
        h->preempt_pending = ends != 0 && now >= ends;
    }
    /* **Was this hart's wake the backstop, with work already waiting?**
     * Then a poke was lost: something made a thread runnable and did not
     * tell a hart that had stopped looking, and only the timeout saved it.
     *
     * All three conditions are needed. Without `backstop` this counts an
     * idle hart's legitimate wake for a sleeper's deadline; without
     * `waiting_count` it counts threads already running on other harts. */
    if (h->current == SCHED_NO_THREAD && h->backstop && waiting_count() > 0) {
        missed_pokes++;
    }
    watch_for_deadlock();
    watch_for_overdue_wakes(now);
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_BLOCKED && threads[i].futex_deadline != 0 &&
            now >= threads[i].futex_deadline) {
            place_on_runqueue(i);
            threads[i].woken = false;
            threads[i].futex_addr = 0;
            deadline_wakes++;
            expired = true;
        }
    }
    if (expired) {
        poke_idle_harts();
    }
    /* Nothing more: `sched_arm_timer` works out what this hart is waiting
     * for next, and the trap path calls it on the way out. */
}

/* Whether the tick asked for a switch, cleared by asking. Preemption
 * happens on the way *out* of a trap and not inside the interrupt handler,
 * because switching from inside an interrupt would resume the other thread
 * with the interrupt still unacknowledged. */
bool sched_take_preempt_request(void) {
    struct hartstate *h = me();
    bool want = h->preempt_pending;
    h->preempt_pending = false;
    return want;
}

void sched_shutdown(void) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].owns_stack && threads[i].kstack != 0) {
            mem_free_pages(threads[i].kstack, KSTACK_PAGES);
        }
        threads[i] = (struct thread){0};
    }
}
