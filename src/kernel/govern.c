#include "kernel/govern.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/smp.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/sched.h"

#define DECIDE_EVERY_BEATS 25u /* 25 beats of 10 ms: a decision every 250 ms */

static struct governor governor;
static uint32_t beats, refusals_said;
static uint64_t last_online[SMP_MAX_HARTS], last_idle[SMP_MAX_HARTS];
static int32_t (*read_temp)(void);
static uint32_t (*write_khz)(uint32_t khz);

static int32_t temp_port(void *self) {
    (void)self;
    return read_temp();
}

static uint32_t set_port(void *self, uint32_t khz) {
    (void)self;
    return write_khz(khz);
}

/* One hart's busy share of the time since it was last asked, in percent. */
static uint32_t hart_busy_pct(uint32_t i) {
    uint64_t online = sched_hart_online_ns(i), idle = sched_hart_idle_ns(i);
    uint64_t span = online - last_online[i], rested = idle - last_idle[i];
    last_online[i] = online;
    last_idle[i] = idle;
    return span == 0 || rested >= span ? 0u : (uint32_t)((span - rested) * 100u / span);
}

/* The busiest application hart's share (see the header for why the most
 * and not the mean). The S7 monitor runs nothing and is skipped. */
static uint32_t busiest_hart_pct(void *self) {
    uint32_t most = 0;
    (void)self;
    for (uint32_t i = 0; i < smp_hart_count() && i < SMP_MAX_HARTS; i++) {
        const struct smp_hart *h = smp_hart(i);
        if (h != NULL && h->has_sv39) {
            uint32_t pct = hart_busy_pct(i);
            most = pct > most ? pct : most;
        }
    }
    return most;
}

/* A decision every DECIDE_EVERY_BEATS beats; each move is printed with its
 * reason and inputs, and a divider that refuses is said once per refusal. */
static void govern_from_beat(void) {
    if (++beats < DECIDE_EVERY_BEATS) {
        return;
    }
    beats = 0;
    struct governor_step s = governor_tick(&governor);
    if (s.moved) {
        kprintf("governor: %u kHz (%s at %d mC, %u%% load)\n", s.khz, cpufreq_reason_name(s.why),
                s.temp_mC, s.load_pct);
    } else if (governor.refused != refusals_said) {
        refusals_said = governor.refused;
        kprintf("governor: the divider refused a step from %u kHz (%s)\n", s.khz,
                cpufreq_reason_name(s.why));
    }
}

int govern_start(const struct cpufreq_policy *policy, enum governor_mode mode,
                 int32_t (*temp_mC)(void), uint32_t (*set_khz)(uint32_t khz)) {
    struct governor_ports ports = {temp_port, busiest_hart_pct, set_port, NULL};
    read_temp = temp_mC;
    write_khz = set_khz;
    if (!governor_init(&governor, policy, &ports, mode)) {
        return KERR_INVAL;
    }
    (void)busiest_hart_pct(NULL); /* the first interval starts now, not at boot */
    kprintf("governor: %s, from %u kHz; throttle at %d mC, critical at %d mC\n",
            mode == GOVERNOR_ONDEMAND ? "ondemand" : "thermal", policy->opps[policy->nopps - 1u].khz,
            policy->throttle_mC, policy->critical_mC);
    return timer_add_housekeeping_hook(govern_from_beat);
}

uint32_t govern_moves(void) {
    return governor.moves;
}
