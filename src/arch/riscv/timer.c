#include "arch/riscv/timer.h"

#include <stddef.h>

#include "arch/riscv/cache.h"
#include "arch/riscv/sbi.h"
#include "arch/riscv/smp.h"
#include "core/errno.h"

/* The supervisor interrupt-enable bit for the timer. */
#define SIE_STIMER (1ull << 5)

static struct {
    uint64_t frequency;  /* counter ticks per second, from the device tree */
    uint64_t interval;   /* counter ticks between ticks of ours */
    uint64_t next;       /* the deadline currently armed */
    uint64_t ticks;
    uint32_t hz;
    bool up;
} timer;

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
} CACHE_ALIGNED;

static struct hart_timer per_hart[SMP_MAX_HARTS];

/* Counter units for a time in nanoseconds. */
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
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIMER));
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

#define TICK_HOOKS 4u

static void (*tick_hooks[TICK_HOOKS])(void);

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
    /* A **tick** only when the periodic deadline has passed. An early
     * interrupt somebody asked for is not one, and counting it would make
     * the clock look as though it ran faster whenever something slept. */
    bool periodic = timer_now() >= per_hart[me].next_deadline;
    timer.ticks += periodic ? 1u : 0u;
    per_hart[me].ticks += periodic ? 1u : 0u;
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
