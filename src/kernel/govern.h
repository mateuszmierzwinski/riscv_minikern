/* govern.h — the frequency governor, run from the housekeeping beat.
 *
 * `core/governor.c` decides; this gives it its load reading (the busiest
 * hart's share of the last interval, because all four U74s share one clock
 * divider, so one busy hart needs the clock however idle the others are)
 * and its schedule (every 250 ms of beats), and prints each move. The
 * sensor and the divider come in as functions from the composition root,
 * which owns both drivers. */
#ifndef MINIKERN_KERNEL_GOVERN_H
#define MINIKERN_KERNEL_GOVERN_H

#include <stdint.h>

#include "core/governor.h"

/* Starts governing with `policy` in `mode`; a kernel error when the policy
 * is refused or the beat has no room for another hook. */
int govern_start(const struct cpufreq_policy *policy, enum governor_mode mode,
                 int32_t (*temp_mC)(void), uint32_t (*set_khz)(uint32_t khz));

/* How many times the governor has moved the clock since it started. */
uint32_t govern_moves(void);

#endif
