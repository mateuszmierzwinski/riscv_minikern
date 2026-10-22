#include "kernel/sched.h"

#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/mem.h"
#include "kernel/uspace.h"

/* 16 KiB of kernel stack per thread. A trap frame is 288 bytes and the
 * handlers nest shallowly, so this is generous — but a kernel stack that
 * overflows corrupts whatever is below it silently, and
 * `-Wframe-larger-than=1024` only bounds one frame at a time. */
#define KSTACK_PAGES 4u

/* A trap frame's size, which the layout above depends on. Checked against
 * the real struct so that adding a field to it cannot silently move the
 * stack top. */
#define FRAME_BYTES (sizeof(struct trapframe))

enum thread_state { THREAD_FREE = 0, THREAD_RUNNABLE, THREAD_RUNNING, THREAD_BLOCKED };

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
    virt_t futex_addr;
    uint64_t futex_deadline;
    bool woken;
    bool owns_stack;
};

static struct thread threads[SCHED_MAX_THREADS];
static uint32_t current;
static uint64_t next_tid = 1;
static bool preempt_pending;
static bool tracing;

void sched_set_trace(bool on) {
    tracing = on;
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

void sched_init(uint64_t *tid) {
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        threads[i] = (struct thread){0};
    }
    current = 0;
    next_tid = 1;
    preempt_pending = false;
    threads[0].state = THREAD_RUNNING;
    threads[0].tid = next_tid++;
    threads[0].owns_stack = false; /* it is the stack uspace_run was called on */
    *tid = threads[0].tid;
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

static void prepare_stack(struct thread *t, const struct trapframe *frame, uint64_t sp);

/* The first thing a new thread does. It is a C function so that the stack
 * layout above has an address to name, and it never returns: the thread
 * leaves through `sret`, and comes back only as a trap. */
static void thread_trampoline(void) {
    uspace_enter_frame(initial_frame_of(&threads[current]));
}

static void prepare_stack(struct thread *t, const struct trapframe *frame, uint64_t sp) {
    struct trapframe *initial = initial_frame_of(t);
    uint64_t *switch_frame;
    *initial = *frame;
    initial->x[10] = 0;  /* a0: the child's half of clone's return */
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

int sched_spawn(const struct trapframe *frame, uint64_t sp, uint64_t *tid) {
    uint32_t slot = find_free_slot();
    phys_t stack = 0;
    int rc;
    if (slot == SCHED_MAX_THREADS) {
        return KERR_AGAIN;
    }
    rc = mem_alloc_pages(KSTACK_PAGES, 1, &stack);
    if (rc != KERR_OK) {
        return rc;
    }
    threads[slot] = (struct thread){.kstack = stack,
                                    .tid = next_tid++,
                                    .state = THREAD_RUNNABLE,
                                    .owns_stack = true};
    prepare_stack(&threads[slot], frame, sp);
    *tid = threads[slot].tid;
    trace("created, entering at", slot, frame->sepc);
    return KERR_OK;
}

/* The next thread that can run, starting after the current one so that the
 * order is round-robin rather than "whoever is lowest-numbered". Returns
 * the current slot when nothing else is runnable. */
static uint32_t next_runnable(void) {
    for (uint32_t step = 1; step <= SCHED_MAX_THREADS; step++) {
        uint32_t i = (current + step) % SCHED_MAX_THREADS;
        if (threads[i].state == THREAD_RUNNABLE) {
            return i;
        }
    }
    return current;
}

/* Hands the hart to `to`. The caller has already set its own state. */
static void switch_to(uint32_t to) {
    uint32_t from = current;
    if (to == from) {
        return;
    }
    threads[to].state = THREAD_RUNNING;
    current = to;
    context_switch(&threads[from].ksp, threads[to].ksp);
}

void sched_yield_now(void) {
    uint32_t to = next_runnable();
    if (to == current) {
        return;
    }
    threads[current].state = THREAD_RUNNABLE;
    switch_to(to);
}

bool sched_futex_wait(virt_t addr, uint64_t deadline_ns) {
    struct thread *me = &threads[current];
    uint32_t to;
    me->futex_addr = addr;
    me->futex_deadline = deadline_ns;
    me->woken = false;
    me->state = THREAD_BLOCKED;
    trace("waits on", current, addr);
    to = next_runnable();
    if (to == current) {
        /* Nothing else can run. Waiting would be a deadlock the kernel
         * caused, so the wait fails and the caller re-checks — which is
         * what a futex loop does anyway. A deadline still has to be
         * honoured, so spin against the clock rather than returning
         * instantly and burning the caller's retry loop. */
        me->state = THREAD_RUNNING;
        trace("has nothing to switch to, so cannot sleep on", current, addr);
        while (deadline_ns != 0 && timer_ns() < deadline_ns) {
            __asm__ volatile("wfi");
        }
        return false;
    }
    switch_to(to);
    me->futex_addr = 0;
    return me->woken;
}

uint32_t sched_futex_wake(virt_t addr, uint32_t count) {
    uint32_t woke = 0;
    /* Logged even when it matches nothing. A wake with no waiter is the
     * normal case for a futex — the other side had not got there yet — but
     * it is also exactly what a *lost* wakeup looks like, and the two can
     * only be told apart by seeing both sides. */
    trace("wakes", current, addr);
    for (uint32_t i = 0; i < SCHED_MAX_THREADS && woke < count; i++) {
        if (threads[i].state == THREAD_BLOCKED && threads[i].futex_addr == addr) {
            threads[i].state = THREAD_RUNNABLE;
            threads[i].woken = true;
            threads[i].futex_addr = 0;
            woke++;
            trace("woken on", i, addr);
        }
    }
    return woke;
}

uint32_t sched_live_threads(void) {
    uint32_t live = 0;
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        live += threads[i].state != THREAD_FREE ? 1u : 0u;
    }
    return live;
}

uint64_t sched_current_tid(void) {
    return threads[current].tid;
}

void sched_exit_thread(int64_t status) {
    uint32_t to;
    trace("ends with status", current, (uint64_t)status);
    threads[current].state = THREAD_FREE;
    to = next_runnable();
    if (to == current) {
        /* The last thread. The process is over, and leaving through
         * `uspace_exit` returns to whoever called `uspace_run` — on a stack
         * that is none of these, which is why that is safe to do from
         * here. */
        uspace_exit(status);
    }
    /* Its kernel stack is still being stood on, so it is freed by
     * `sched_shutdown` rather than here. Switching away never comes back,
     * and `ksp` is written to a slot nothing will read again. */
    switch_to(to);
}

void sched_tick(void) {
    uint64_t now = timer_ns();
    for (uint32_t i = 0; i < SCHED_MAX_THREADS; i++) {
        if (threads[i].state == THREAD_BLOCKED && threads[i].futex_deadline != 0 &&
            now >= threads[i].futex_deadline) {
            threads[i].state = THREAD_RUNNABLE;
            threads[i].woken = false;
            threads[i].futex_addr = 0;
        }
    }
    preempt_pending = true;
}

/* Whether the tick asked for a switch, cleared by asking. Preemption
 * happens on the way *out* of a trap and not inside the interrupt handler,
 * because switching from inside an interrupt would resume the other thread
 * with the interrupt still unacknowledged. */
bool sched_take_preempt_request(void) {
    bool want = preempt_pending;
    preempt_pending = false;
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
