#include "core/governor.h"

bool governor_init(struct governor *g, const struct cpufreq_policy *policy,
                   const struct governor_ports *ports, enum governor_mode mode) {
    if (!cpufreq_policy_valid(policy)) {
        return false;
    }
    *g = (struct governor){.policy = *policy, .ports = *ports, .mode = mode};
    g->at = policy->nopps - 1u;
    return true;
}

/* The temperature to decide on: the sensor's, unless it cannot be believed,
 * in which case the critical one (see the header). */
static int32_t believable_temp(const struct governor *g) {
    int32_t t = g->ports.temp_mC(g->ports.self);
    if (t == GOVERNOR_NO_READING || t < GOVERNOR_COLDEST_MC || t > GOVERNOR_HOTTEST_MC) {
        return g->policy.critical_mC;
    }
    return t;
}

/* The load to decide on: the real one only when asked to follow it. */
static uint32_t load_to_use(const struct governor *g) {
    if (g->mode != GOVERNOR_ONDEMAND) {
        return 100u;
    }
    uint32_t load = g->ports.load_pct(g->ports.self);
    return load > 100u ? 100u : load;
}

struct governor_step governor_tick(struct governor *g) {
    struct governor_step s = {.temp_mC = believable_temp(g), .load_pct = load_to_use(g)};
    uint32_t next = cpufreq_decide(&g->policy, g->at, s.temp_mC, s.load_pct, &s.why);
    if (next != g->at) {
        if (g->ports.set_khz(g->ports.self, g->policy.opps[next].khz) != 0) {
            g->at = next;
            g->moves++;
            s.moved = true;
        } else {
            g->refused++;
        }
    }
    s.khz = g->policy.opps[g->at].khz;
    return s;
}
