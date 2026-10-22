/* cpufreq — which operating point the CPU should be at.
 *
 * Pure decision-making: it is given a temperature, a load and where the CPU
 * is now, and answers where it should be. It touches no register, which is
 * what makes it the testable half of a feature whose other half cannot be
 * tested here at all (QEMU models neither the H3's thermal sensor nor its
 * PLLs, so the adapters are unverified until the board runs — CLAUDE.md §3).
 *
 * Two things decide, in this order:
 *
 *   1. **Temperature, which overrules everything.** Above the throttle
 *      threshold the policy steps down, whatever the load is asking for,
 *      and keeps stepping while it stays hot. This is the half that
 *      protects the silicon, which is why it comes first and why it was
 *      built before any clock was raised (the operator's instruction:
 *      thermal before DVFS).
 *   2. **Load**, which may raise the point only while the temperature is
 *      below the threshold by the hysteresis margin.
 *
 * The hysteresis is not decoration: a policy that steps down at 80 °C and
 * up again the moment it reads 79 °C spends its life changing frequency,
 * and every change stops the CPU for the PLL to relock.
 */
#ifndef MINIKERN_CORE_CPUFREQ_H
#define MINIKERN_CORE_CPUFREQ_H

#include <stdbool.h>
#include <stdint.h>

/* One operating point: a frequency and the voltage it needs. The H3's
 * published points (CLAUDE.md §12); a board offers the subset it can
 * actually supply. */
struct cpu_opp {
    uint32_t khz;
    uint32_t mv;
};

#define CPUFREQ_MAX_OPPS 8u

/* What the policy decides against. Temperatures are milli-degrees Celsius,
 * so that a sensor's resolution survives the arithmetic. */
struct cpufreq_policy {
    struct cpu_opp opps[CPUFREQ_MAX_OPPS]; /* lowest first */
    uint32_t nopps;
    int32_t throttle_mC;                   /* at or above this, step down */
    int32_t critical_mC;                   /* at or above this, go to the lowest point */
    int32_t hysteresis_mC;                 /* how far below throttle before stepping up */
    uint32_t load_up_pct;                  /* load at or above which to step up */
    uint32_t load_down_pct;                /* load at or below which to step down */
};

/* Checks a policy before anything uses it: points in ascending frequency
 * order, a hysteresis that actually separates the thresholds, and load
 * limits that do not overlap. A policy that fails this is a bug in the
 * board's table, and the kernel refuses it rather than acting on it. */
bool cpufreq_policy_valid(const struct cpufreq_policy *p);

/* The operating point to be at. `at` is the current index; the answer is
 * never more than one step away from it, so a sweep is gradual and every
 * step is a decision that can be logged. */
uint32_t cpufreq_pick(const struct cpufreq_policy *p, uint32_t at, int32_t temp_mC, uint32_t load_pct);

/* Why the last pick moved, for the log. */
enum cpufreq_reason {
    CPUFREQ_HOLD,
    CPUFREQ_CRITICAL,
    CPUFREQ_THROTTLE,
    CPUFREQ_LOAD_UP,
    CPUFREQ_LOAD_DOWN,
};

/* The same decision as cpufreq_pick, with the reason. cpufreq_pick is the
 * facade over it (CLAUDE.md §2, master Rule 5.2). */
uint32_t cpufreq_decide(const struct cpufreq_policy *p, uint32_t at, int32_t temp_mC, uint32_t load_pct,
                        enum cpufreq_reason *why);

/* The name of a reason, for the log. */
const char *cpufreq_reason_name(enum cpufreq_reason why);

#endif
