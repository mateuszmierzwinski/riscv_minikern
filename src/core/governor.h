/* governor.h — the frequency governor: `cpufreq_decide` fed by a sensor
 * and a load reading, and acting through a port.
 *
 * The policy (`cpufreq.c`) answers "given this temperature and this load,
 * which point?". This is the part that asks it on a schedule and carries the
 * answer out — the half that, until it existed, made the policy a tested
 * function nobody called (ROADMAP §4).
 *
 * Two modes. **Thermal** (the default) passes a load of 100 %, so the board
 * sits at its top point and moves only for heat: a governor that is on by
 * default must not change a benchmark. **Ondemand** passes the real load and
 * also steps down when idle. In both, a reading the sensor cannot vouch for
 * is taken as the critical temperature — the ARM governor went to the top
 * point on a -264 °C reading, and an unbelievable number is the worst case,
 * not a cold one. */
#ifndef MINIKERN_CORE_GOVERNOR_H
#define MINIKERN_CORE_GOVERNOR_H

#include <stdbool.h>
#include <stdint.h>

#include "core/cpufreq.h"

/* What a sensor that has nothing to say returns. */
#define GOVERNOR_NO_READING INT32_MIN

/* The range a die temperature may plausibly have; outside it the reading
 * is treated as no reading at all. */
#define GOVERNOR_COLDEST_MC (-40000)
#define GOVERNOR_HOTTEST_MC 150000

struct governor_ports {
    int32_t (*temp_mC)(void *self);              /* or GOVERNOR_NO_READING */
    uint32_t (*load_pct)(void *self);            /* the busiest hart since the last call */
    uint32_t (*set_khz)(void *self, uint32_t khz); /* the frequency set; 0 when refused */
    void *self;
};

enum governor_mode {
    GOVERNOR_THERMAL,
    GOVERNOR_ONDEMAND,
};

/* One decision as the governor saw it — what the log line prints. */
struct governor_step {
    enum cpufreq_reason why;
    int32_t temp_mC;   /* as used: an unbelievable reading is already the critical one */
    uint32_t load_pct; /* as used: 100 in thermal mode */
    uint32_t khz;      /* the point after the step */
    bool moved;
};

struct governor {
    struct cpufreq_policy policy;
    struct governor_ports ports;
    enum governor_mode mode;
    uint32_t at;      /* the current point's index; starts at the top */
    uint32_t moves;   /* how many steps changed the point */
    uint32_t refused; /* steps the port would not carry out */
};

/* Starts at the policy's top point, which is where the firmware leaves the
 * board; false when the policy is not one `cpufreq_policy_valid` accepts. */
bool governor_init(struct governor *g, const struct cpufreq_policy *policy,
                   const struct governor_ports *ports, enum governor_mode mode);

/* Reads the sensor and the load, decides, and carries the decision out. */
struct governor_step governor_tick(struct governor *g);

#endif
