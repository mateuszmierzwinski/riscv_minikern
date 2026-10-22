#include "core/cpufreq.h"

bool cpufreq_policy_valid(const struct cpufreq_policy *p) {
    if (p->nopps == 0 || p->nopps > CPUFREQ_MAX_OPPS) {
        return false;
    }
    for (uint32_t i = 1; i < p->nopps; i++) {
        if (p->opps[i].khz <= p->opps[i - 1].khz || p->opps[i].mv < p->opps[i - 1].mv) {
            return false;
        }
    }
    if (p->critical_mC <= p->throttle_mC || p->hysteresis_mC <= 0) {
        return false;
    }
    return p->load_down_pct < p->load_up_pct && p->load_up_pct <= 100u;
}

static uint32_t step_down(uint32_t at) {
    return at == 0 ? 0 : at - 1;
}

static uint32_t step_up(const struct cpufreq_policy *p, uint32_t at) {
    return at + 1 >= p->nopps ? p->nopps - 1 : at + 1;
}

/* True while the temperature leaves no room to raise the frequency: at or
 * above the throttle point, or still inside the hysteresis band below it. */
static bool too_warm_to_rise(const struct cpufreq_policy *p, int32_t temp_mC) {
    return temp_mC > p->throttle_mC - p->hysteresis_mC;
}

uint32_t cpufreq_decide(const struct cpufreq_policy *p, uint32_t at, int32_t temp_mC, uint32_t load_pct,
                        enum cpufreq_reason *why) {
    uint32_t here = at < p->nopps ? at : p->nopps - 1;
    *why = CPUFREQ_HOLD;
    /* The reason describes the *decision*, not whether it moved anything:
     * a CPU already at its lowest point still has a critical temperature,
     * and that is the single most important thing the log can say. The
     * caller sees for itself that the answer equals where it already is. */
    if (temp_mC >= p->critical_mC) {
        *why = CPUFREQ_CRITICAL;
        return 0;
    }
    if (temp_mC >= p->throttle_mC) {
        *why = CPUFREQ_THROTTLE;
        return step_down(here);
    }
    if (load_pct >= p->load_up_pct && !too_warm_to_rise(p, temp_mC)) {
        *why = CPUFREQ_LOAD_UP;
        return step_up(p, here);
    }
    if (load_pct <= p->load_down_pct) {
        *why = CPUFREQ_LOAD_DOWN;
        return step_down(here);
    }
    return here;
}

uint32_t cpufreq_pick(const struct cpufreq_policy *p, uint32_t at, int32_t temp_mC, uint32_t load_pct) {
    enum cpufreq_reason why;
    return cpufreq_decide(p, at, temp_mC, load_pct, &why);
}

const char *cpufreq_reason_name(enum cpufreq_reason why) {
    switch (why) {
    case CPUFREQ_CRITICAL:
        return "critical temperature";
    case CPUFREQ_THROTTLE:
        return "throttling";
    case CPUFREQ_LOAD_UP:
        return "load";
    case CPUFREQ_LOAD_DOWN:
        return "idle";
    default:
        return "holding";
    }
}
