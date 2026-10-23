#include "drivers/cpufreq_jh7110.h"

#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"

/* Index 1 of the system clock generator, which a board running at its top
 * operating point holds at 1, and which writing 2 was measured to halve the
 * processor's speed by. The field is the low byte; the upper bits belong to
 * the mux and the enable and are preserved. */
#define CPU_CORE_CLOCK_INDEX 1u
#define DIVIDER_MASK 0xffu

static struct {
    bool up;
    uint32_t base_khz; /* the frequency at divider 1 */
    uint32_t points;   /* how many operating points were read */
    uint32_t khz[CPUFREQ_MAX_OPPS];
} state;

static uint32_t finish_init(struct cpufreq_policy *policy, uint32_t found);

static volatile uint32_t *divider_register(void) {
    return (volatile uint32_t *)(uintptr_t)(crg_base(CRG_SYS) +
                                            crg_clock_offset(CPU_CORE_CLOCK_INDEX));
}

uint32_t cpufreq_jh7110_divider(void) {
    if (!state.up) {
        return 0;
    }
    return *divider_register() & DIVIDER_MASK;
}

uint32_t cpufreq_jh7110_current_khz(void) {
    uint32_t div = cpufreq_jh7110_divider();
    return div == 0 ? 0 : state.base_khz / div;
}

/* One operating point out of the device tree. The frequency is two cells and
 * the voltage one, which is the `operating-points-v2` binding. */
static bool read_point(const struct fdt *fdt, const char *child, struct cpu_opp *out) {
    struct fdt_prop hz, uv;
    uint64_t rate = 0, volt = 0;
    if (fdt_get_prop(fdt, child, "opp-hz", &hz) != KERR_OK) {
        return false;
    }
    if (fdt_prop_cells(&hz, 0, 2, &rate) != KERR_OK || rate == 0) {
        return false;
    }
    if (fdt_get_prop(fdt, child, "opp-microvolt", &uv) == KERR_OK) {
        (void)fdt_prop_cells(&uv, 0, 1, &volt);
    }
    out->khz = (uint32_t)(rate / 1000u);
    out->mv = (uint32_t)(volt / 1000u);
    return true;
}

/* The thresholds the governor decides against.
 *
 * Numbers rather than measurements, and said so: nothing reads this board's
 * temperature yet (`sfctemp` is not written), so the thermal half of the
 * policy is configured and inert. The load half is live. They are here so
 * that the policy passes its own validity check, and the moment a sensor
 * exists they are what it feeds. */
static void set_thresholds(struct cpufreq_policy *p) {
    p->throttle_mC = 85000;
    p->critical_mC = 95000;
    p->hysteresis_mC = 5000;
    p->load_up_pct = 80u;
    p->load_down_pct = 30u;
}

uint32_t cpufreq_jh7110_init(const struct fdt *fdt, struct cpufreq_policy *policy) {
    char path[128];
    uint32_t found = 0;
    if (policy == NULL || !crg_present(CRG_SYS)) {
        return 0;
    }
    if (fdt_find_compatible(fdt, "operating-points-v2", path, sizeof path) != KERR_OK) {
        return 0;
    }
    for (uint32_t i = 0; i < CPUFREQ_MAX_OPPS; i++) {
        char child[192];
        if (fdt_nth_child(fdt, path, i, child, sizeof child) != KERR_OK) {
            break;
        }
        if (read_point(fdt, child, &policy->opps[found])) {
            found++;
        }
    }
    policy->nopps = found;
    set_thresholds(policy);
    return found == 0 ? 0 : finish_init(policy, found);
}

/* The half of `init` that depends on the points having been read: the base
 * frequency, which is the top point as the hardware is holding it now.
 *
 * Taken from the *current divider* rather than assumed to be the highest
 * point, because the firmware chose it and this driver's whole contract is
 * that it never raises a voltage. If a board booted at a divider of 2, its
 * base is still the top point — the divider tells us what the top is. */
static uint32_t finish_init(struct cpufreq_policy *policy, uint32_t found) {
    uint32_t div = *divider_register() & DIVIDER_MASK;
    if (div == 0 || !cpufreq_policy_valid(policy)) {
        kprintf("cpufreq: refusing this board's table (%u points, divider %u)\n", found, div);
        return 0;
    }
    /* The top point **as the hardware is holding it**: the divider says what
     * the base is, so a board that booted divided still computes it right. */
    state.base_khz = policy->opps[found - 1u].khz * div;
    state.points = found;
    for (uint32_t i = 0; i < found; i++) {
        state.khz[i] = policy->opps[i].khz;
    }
    state.up = true;
    kprintf("cpufreq: %u points, %u kHz at divider %u\n", found, cpufreq_jh7110_current_khz(),
            div);
    return found;
}

/* The divider that gets closest to `khz` without exceeding it. */
static uint32_t divider_for(uint32_t khz) {
    uint32_t best = 0;
    for (uint32_t div = 1u; div <= DIVIDER_MASK; div++) {
        uint32_t at = state.base_khz / div;
        if (at <= khz) {
            best = div;
            break;
        }
    }
    return best;
}

uint32_t cpufreq_jh7110_set_khz(uint32_t khz) {
    volatile uint32_t *reg;
    uint32_t div, was;
    if (!state.up || khz == 0) {
        return 0;
    }
    div = divider_for(khz);
    if (div == 0) {
        return 0;
    }
    reg = divider_register();
    was = *reg;
    *reg = (was & ~DIVIDER_MASK) | div;
    /* Read back, because a write that does not stick is the failure this
     * class of register produces — the GPIO pads did exactly that, accepting
     * byte stores and discarding them. */
    if ((*reg & DIVIDER_MASK) != div) {
        *reg = was;
        return 0;
    }
    return cpufreq_jh7110_current_khz();
}
