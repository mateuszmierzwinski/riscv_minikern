#include "arch/riscv/timer.h"

#include <stddef.h>

#include "arch/riscv/cache.h"
#include "arch/riscv/sbi.h"
#include "arch/riscv/smp.h"
#include "core/errno.h"
#include "core/console.h"

/* The supervisor interrupt-enable bit for the timer. */
#define SIE_STIMER (1ull << 5)
/* Supervisor software interrupt: how one hart wakes another. */
#define SIE_SSOFT (1ull << 1)

static struct {
    uint64_t frequency;  /* counter ticks per second, from the device tree */
    uint64_t interval;   /* counter ticks between ticks of ours */
    uint64_t next;       /* the deadline currently armed */
    uint64_t ticks;
    uint32_t hz;
    bool up;
    /* True once a scheduler arms the timer itself. Before that it ticks
     * periodically, because the boot path proves the clock by counting. */
    bool handed_over;
} timer;

void timer_take_over(void) {
    timer.handed_over = true;
}

uint64_t timer_now(void) {
    uint64_t t;
    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

/* Each hart's own next deadline and its own tick count.
 *
 * **Per hart, because the comparator is.** `sbi_set_timer` arms the timer of
 * the hart that calls it, so a shared deadline would have whichever hart
 * ticked first advance it for everyone — and every other hart would then
 * arm a deadline already in the past, taking an interrupt immediately, for
 * ever. One shared variable is the difference between a 100 Hz tick and an
 * interrupt storm on three cores. */
/* Each hart's **periodic** schedule, and separately the one-off wake it has
 * been asked for. Two numbers rather than one because the comparator holds
 * whichever comes first, and conflating them makes a sleep shift the clock:
 * the periodic deadline must survive an early wake-up untouched. */
/* One struct per hart rather than three parallel arrays, **padded to a cache
 * line**: three arrays indexed by hart put four harts' deadlines in one line,
 * so each hart's rearm invalidated the line the other three read. One struct
 * per hart puts each hart's three values together and nobody else's with
 * them. (Not measurable under an emulator that models no caches; see
 * `cache.h`.) */
struct hart_timer {
    uint64_t next_deadline; /* the periodic schedule */
    uint64_t extra_wake;    /* a one-off wake that was asked for, or 0 */
    uint64_t ticks;
    /* ── dynticks ──────────────────────────────────────────────────────
     * What this hart's comparator currently holds, in counter units, and
     * how many interrupts it has taken. `armed` is 0 when the hart has no
     * deadline at all — which is the state a hart with one runnable thread
     * and nothing asleep should be in, and the thing the Phase 1 test
     * measures. */
    uint64_t armed;
    uint64_t interrupts;
} CACHE_ALIGNED;

static struct hart_timer per_hart[SMP_MAX_HARTS];

/* Counter units for a time in nanoseconds. */
/* Nanoseconds for a counter value, which is the other direction. */
static uint64_t ns_of_counter(uint64_t at) {
    return timer.frequency == 0 ? 0 : at / timer.frequency * 1000000000ull +
                                          (at % timer.frequency) * 1000000000ull /
                                              timer.frequency;
}

static uint64_t counter_of_ns(uint64_t ns) {
    return timer.frequency == 0 ? 0u
                                : (ns / 1000000000ull) * timer.frequency +
                                      (ns % 1000000000ull) * timer.frequency / 1000000000ull;
}

/* Arms the comparator for whichever of the two deadlines comes first. */
static void arm_comparator(uint32_t me) {
    uint64_t at = per_hart[me].next_deadline;
    if (per_hart[me].extra_wake != 0 && per_hart[me].extra_wake < at) {
        at = per_hart[me].extra_wake;
    }
    sbi_set_timer(at);
}

void timer_wake_at(uint64_t ns) {
    uint32_t me = smp_hart_index();
    uint64_t at = counter_of_ns(ns);
    if (timer.handed_over) {
        /* **One path once the scheduler owns the timer.** Two ways of
         * arming the same comparator is two ideas of what it holds: this
         * one used to write it directly while `timer_arm_at` tracked what
         * it had written, so each could undo the other — and a deadline
         * that is silently dropped is a thread that never wakes. */
        timer_arm_at(ns);
        return;
    }
    if (!timer.up || at == 0) {
        return;
    }
    if (per_hart[me].extra_wake == 0 || at < per_hart[me].extra_wake) {
        per_hart[me].extra_wake = at;
        arm_comparator(me);
    }
}

int timer_init(const struct fdt *fdt, uint32_t hz) {
    uint64_t freq = fdt_get_u32(fdt, "/cpus", "timebase-frequency", 0);
    if (freq == 0 || hz == 0) {
        return KERR_NOENT;
    }
    timer.frequency = freq;
    timer.hz = hz;
    timer.interval = freq / hz;
    timer.interval = timer.interval == 0 ? 1 : timer.interval;
    timer.up = true;
    per_hart[smp_hart_index()].next_deadline = timer_now() + timer.interval;
    arm_comparator(smp_hart_index());
    /* Enabling the source is separate from enabling interrupts at all: the
     * caller decides when the kernel is ready to be interrupted. */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIMER));
    return KERR_OK;
}

/* Arms this hart's timer and lets it take the interrupt. Every hart calls
 * it for itself; `timer_init` must have read the frequency first. */
int timer_init_cpu(void) {
    uint32_t me = smp_hart_index();
    if (!timer.up) {
        return KERR_NODEV;
    }
    per_hart[me].next_deadline = timer_now() + timer.interval;
    per_hart[me].extra_wake = 0;
    arm_comparator(me);
    /* Timer **and** software interrupts: without the second, a hart in
     * `wfi` with no deadline of its own can only be woken by its own clock,
     * and a dynticks hart does not have one. */
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIMER | SIE_SSOFT));
    return KERR_OK;
}

void timer_rearm(void) {
    uint32_t me = smp_hart_index();
    /* A one-off wake has now happened, whichever deadline it was that
     * fired: it is not owed again. */
    per_hart[me].extra_wake = 0;
    /* Only advance the periodic schedule when its deadline has actually
     * passed. An early interrupt — one somebody asked for with
     * `timer_wake_at` — must leave the tick where it was, or every sleep
     * would shift the clock. */
    if (timer_now() >= per_hart[me].next_deadline) {
        /* From the deadline that passed, not from now: a handler that took
         * a while must not push the next tick out by however long it took,
         * or the clock drifts by exactly the kernel's own cost. */
        per_hart[me].next_deadline += timer.interval;
        if (per_hart[me].next_deadline < timer_now()) {
            /* Unless it fell so far behind that catching up would mean a
             * storm of back-to-back ticks. Then the clock has lost time,
             * which is honest, rather than spending the next second making
             * it up. */
            per_hart[me].next_deadline = timer_now() + timer.interval;
        }
    }
    arm_comparator(me);
}

/* A deadline so far away it will not arrive: what "no timer" is spelled as,
 * since there is no way to switch the comparator off. */
#define TIMER_NEVER ((uint64_t)-1)

/* **The one place that changes this hart's timer state.**
 *
 * `armed` and the comparator are two records of the same fact, and keeping
 * them in step is not something to spread around: every path that fires,
 * disarms or arms goes through here, so "armed is non-zero exactly when the
 * comparator holds a live deadline and the interrupt is enabled" is an
 * invariant of one function rather than a convention several functions try
 * to honour.
 *
 * `at` of 0 means "no deadline": the interrupt is masked, because the
 * comparator cannot be switched off and an expired one left enabled is a
 * hart that takes the same interrupt for ever. */
static void set_hart_timer(uint32_t me, uint64_t at) {
    per_hart[me].armed = at;
    if (at == 0) {
        __asm__ volatile("csrc sie, %0" : : "r"(SIE_STIMER) : "memory");
        return;
    }
    sbi_set_timer(at);
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIMER) : "memory");
}

void timer_arm_at(uint64_t ns) {
    uint32_t me = smp_hart_index();
    uint64_t rounded = ((ns + TIMER_GRANULARITY_NS - 1u) / TIMER_GRANULARITY_NS) *
                       TIMER_GRANULARITY_NS;
    uint64_t at = counter_of_ns(rounded);
    if (!timer.up || at == 0) {
        return;
    }
    if (per_hart[me].armed == at) {
        return; /* already waiting for exactly this: no firmware call */
    }
    set_hart_timer(me, at);
}

void timer_disarm(void) {
    uint32_t me = smp_hart_index();
    if (!timer.up) {
        return;
    }
    /* **No early-out on "already disarmed".**
     *
     * It is tempting and it is wrong, because the two records are not the
     * same thing: `armed` is what was asked for and the comparator is what
     * the hardware will do. A timer interrupt clears the first and leaves
     * the second holding a time that has now passed, so an early-out here
     * skips the mask, the pending interrupt is taken again the instant it
     * returns, and the hart does nothing else for ever.
     *
     * Measured: a storm of supervisor timer interrupts that looked exactly
     * like a load access fault, because **both report cause 5** and only
     * the interrupt bit tells them apart. The program made four syscalls
     * and then never ran another instruction of its own.
     *
     * `set_hart_timer` is unconditional, which is the point of having one
     * place: the question "is it already in that state?" is the question
     * that got this wrong. */
    set_hart_timer(me, 0);
}

uint64_t timer_armed_ns_of(uint32_t hart) {
    if (!timer.up || hart >= SMP_MAX_HARTS || per_hart[hart].armed == 0) {
        return 0;
    }
    return ns_of_counter(per_hart[hart].armed);
}

uint64_t timer_armed_at(void) {
    uint32_t me = smp_hart_index();
    return per_hart[me].armed == 0 ? 0 : ns_of_counter(per_hart[me].armed);
}

uint64_t timer_interrupts_of(uint32_t hart_index) {
    return hart_index < SMP_MAX_HARTS ? per_hart[hart_index].interrupts : 0u;
}

#define TICK_HOOKS 4u

static void (*tick_hooks[TICK_HOOKS])(void);
static void (*housekeeping_hooks[TICK_HOOKS])(void);

/* **The beat has no owner, and is claimed by whichever hart gets to it.**
 *
 * Two designs were tried and both were wrong in a way worth recording.
 *
 * *Armed from the scheduler*: `sched_arm_timer` runs on returns to user mode
 * and at scheduling decisions, so a kernel doing its own network work with
 * **no user process at all** never armed it. The symptom was precise — a
 * DHCP lease was obtained, because that path polls in a loop of its own, and
 * then a ping got no answer, because nothing was turning the stack's crank.
 * The kernel's final idle loop is not the scheduler, and once the scheduler
 * had taken the comparator, nothing armed that hart ever again.
 *
 * *Pinned to one hart*: a hart that owns the beat and then picks up a
 * compute-bound thread charges that thread a hundred interrupts a second and
 * runs the polling inside them — on the thread's slice, attributed to
 * nobody. Whichever thread lands there loses the property the whole model
 * exists for, and which thread that is depends on the scheduler.
 *
 * So: one due time, no owner, and whoever reaches it first advances it and
 * runs the hooks. The advance happens under the kernel lock, which is what
 * makes "first" meaningful and stops two harts polling the same beat. Which
 * harts bother to arm for it is the scheduler's choice, and it prefers one
 * that has stopped anyway — see `sched_arm_timer`. */
static uint32_t hk_runs;
static uint64_t housekeeping_due;

uint32_t timer_housekeeping_beats(void) {
    return hk_runs;
}

uint64_t timer_housekeeping_due(void) {
    return timer_has_housekeeping() ? housekeeping_due : 0u;
}

/* Runs the hooks if the interval has elapsed, says when the next beat is
 * due, and arms for it — the arming is what makes the beat survive a kernel
 * with nothing else going on. */
static void housekeeping_if_due(bool may_arm) {
    uint64_t now;
    if (!timer_has_housekeeping()) {
        return;
    }
    now = timer_ns();
    if (now < housekeeping_due) {
        return;
    }
    housekeeping_due = now + TIMER_HOUSEKEEPING_NS;
    hk_runs++;
    timer_run_housekeeping();
    /* Only when this scheduler owns the comparator. Under the periodic
     * clock the tick is already 10 ms, so the beat rides it and arming here
     * would overwrite the periodic schedule — the same mistake, in the
     * other direction, as arming from the scheduler while the clock ran. */
    /* Arming again here is what keeps the beat alive on a hart that nobody
     * else will arm — the kernel's own idle loop, which is not the
     * scheduler and does not call it. */
    if (may_arm) {
        timer_arm_at(housekeeping_due);
    }
}

int timer_add_housekeeping_hook(void (*hook)(void)) {
    for (uint32_t i = 0; i < TICK_HOOKS; i++) {
        if (housekeeping_hooks[i] == NULL) {
            housekeeping_hooks[i] = hook;
            if (housekeeping_due == 0) {
                housekeeping_due = timer_ns() + TIMER_HOUSEKEEPING_NS;
            }
            return KERR_OK;
        }
        if (housekeeping_hooks[i] == hook) {
            return KERR_OK;
        }
    }
    return KERR_NOSPC;
}

void timer_run_housekeeping(void) {
    for (uint32_t i = 0; i < TICK_HOOKS; i++) {
        if (housekeeping_hooks[i] != NULL) {
            housekeeping_hooks[i]();
        }
    }
}

bool timer_has_housekeeping(void) {
    for (uint32_t i = 0; i < TICK_HOOKS; i++) {
        if (housekeeping_hooks[i] != NULL) {
            return true;
        }
    }
    return false;
}

int timer_add_tick_hook(void (*hook)(void)) {
    for (uint32_t i = 0; i < TICK_HOOKS; i++) {
        if (tick_hooks[i] == NULL) {
            tick_hooks[i] = hook;
            return KERR_OK;
        }
        if (tick_hooks[i] == hook) {
            return KERR_OK; /* already there: adding twice is not two ticks */
        }
    }
    return KERR_NOSPC;
}

void timer_remove_tick_hook(void (*hook)(void)) {
    for (uint32_t i = 0; i < TICK_HOOKS; i++) {
        if (tick_hooks[i] == hook) {
            tick_hooks[i] = NULL;
        }
    }
}

void timer_on_tick(void) {
    uint32_t me = smp_hart_index();
    per_hart[me].interrupts++;
    if (timer.handed_over) {
        /* **Acknowledged here, in the one place that owns the pair.** The
         * deadline that fired is spent, so the record of it goes and the
         * interrupt is masked — the comparator still holds a time in the
         * past and an enabled interrupt against it fires again immediately.
         * Whoever wants another deadline asks for one; a hart that does not
         * ask is a hart that will not be interrupted. */
        set_hart_timer(me, 0);
        timer.ticks++;
        per_hart[me].ticks++;
        for (uint32_t i = 0; i < TICK_HOOKS; i++) {
            if (tick_hooks[i] != NULL) {
                tick_hooks[i]();
            }
        }
        housekeeping_if_due(true);
        return;
    }
    /* A **tick** only when the periodic deadline has passed. An early
     * interrupt somebody asked for is not one, and counting it would make
     * the clock look as though it ran faster whenever something slept. */
    bool periodic = timer_now() >= per_hart[me].next_deadline;
    timer.ticks += periodic ? 1u : 0u;
    per_hart[me].ticks += periodic ? 1u : 0u;
    housekeeping_if_due(false);
    timer_rearm();
    /* Rearmed first, then the hook: a scheduler's tick may not return for a
     * long time — it is where a thread switch happens — and a timer that is
     * rearmed afterwards would stop ticking for exactly as long. */
    for (uint32_t i = 0; i < TICK_HOOKS; i++) {
        if (tick_hooks[i] != NULL) {
            tick_hooks[i]();
        }
    }
}

uint64_t timer_ns(void) {
    uint64_t now = timer_now();
    return timer.frequency == 0 ? 0 : (now / timer.frequency) * 1000000000ull +
                                          (now % timer.frequency) * 1000000000ull / timer.frequency;
}

uint64_t timer_ticks(void) {
    /* **This hart's** ticks, not every hart's added together. The total
     * would be four times the rate on four harts, so anything measuring
     * "ticks per second" against it would conclude the clock runs at 400 Hz
     * — and a tick count is read precisely to check the rate. */
    return per_hart[smp_hart_index()].ticks;
}

uint64_t timer_ticks_all_harts(void) {
    return timer.ticks;
}

uint64_t timer_ticks_of(uint32_t hart_index) {
    return hart_index < SMP_MAX_HARTS ? per_hart[hart_index].ticks : 0u;
}

uint32_t timer_hz(void) {
    return timer.hz;
}

uint64_t timer_frequency(void) {
    return timer.frequency;
}

bool timer_ready(void) {
    return timer.up;
}
