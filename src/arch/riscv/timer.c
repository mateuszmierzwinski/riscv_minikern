#include "arch/riscv/timer.h"

#include <stddef.h>

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
static uint64_t next_deadline[SMP_MAX_HARTS];
static uint64_t hart_ticks[SMP_MAX_HARTS];

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
    next_deadline[smp_hart_index()] = timer_now() + timer.interval;
    sbi_set_timer(next_deadline[smp_hart_index()]);
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
    next_deadline[me] = timer_now() + timer.interval;
    sbi_set_timer(next_deadline[me]);
    __asm__ volatile("csrs sie, %0" : : "r"(SIE_STIMER));
    return KERR_OK;
}

void timer_rearm(void) {
    uint32_t me = smp_hart_index();
    /* From the deadline that passed, not from now: a handler that took a
     * while must not push the next tick out by however long it took, or the
     * clock drifts by exactly the kernel's own cost. */
    next_deadline[me] += timer.interval;
    if (next_deadline[me] < timer_now()) {
        /* Unless it fell so far behind that catching up would mean a storm
         * of back-to-back ticks. Then the clock has lost time, which is
         * honest, rather than spending the next second making it up. */
        next_deadline[me] = timer_now() + timer.interval;
    }
    sbi_set_timer(next_deadline[me]);
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
    timer.ticks++;
    hart_ticks[smp_hart_index()]++;
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
    return hart_ticks[smp_hart_index()];
}

uint64_t timer_ticks_all_harts(void) {
    return timer.ticks;
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
