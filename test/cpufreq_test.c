/* The frequency governor's policy, on the host.
 *
 * This file is carried over from the ARM project and had **no build target
 * at all** until Phase 3 went looking for it: the policy this kernel claimed
 * to have was not compiled, let alone tested. So the first thing to do with
 * it was to check that it means what its comments say.
 *
 * It is worth testing here rather than on a board because it is a pure
 * decision and every mistake in it is a *plausible* wrong answer — a
 * governor that throttles one step late, or oscillates between two points,
 * or climbs under load while over temperature, looks like the hardware
 * behaving badly rather than like a bug. The ARM project's own lesson was
 * the mirror of this: a fake proved the policy and nothing about the
 * registers. Both halves need their own test, and this is the policy's.
 */
#include "core/cpufreq.h"

#include <stdint.h>

#include "ktest.h"

/* The VisionFive 2's real table, read from its device tree: four points,
 * three of which share a voltage. */
static struct cpufreq_policy board_policy(void) {
    struct cpufreq_policy p = {
        .opps = {{375000u, 800u}, {500000u, 800u}, {750000u, 800u}, {1500000u, 1040u}},
        .nopps = 4u,
        .throttle_mC = 85000,
        .critical_mC = 95000,
        .hysteresis_mC = 5000,
        .load_up_pct = 80u,
        .load_down_pct = 30u,
    };
    return p;
}

static void test_the_boards_real_table_is_valid(void) {
    struct cpufreq_policy p = board_policy();
    KTEST_CHECK(cpufreq_policy_valid(&p));
}

static void test_a_table_out_of_order_is_refused(void) {
    struct cpufreq_policy p = board_policy();
    uint32_t swap = p.opps[0].khz;
    p.opps[0].khz = p.opps[3].khz;
    p.opps[3].khz = swap;
    /* Descending points would make "one step up" one step down, silently. */
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void test_an_empty_table_is_refused(void) {
    struct cpufreq_policy p = board_policy();
    p.nopps = 0;
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void test_overlapping_load_limits_are_refused(void) {
    struct cpufreq_policy p = board_policy();
    p.load_up_pct = 30u;
    p.load_down_pct = 80u;
    /* Up at 30% and down at 80% means every load is both, which is a
     * governor that never settles. */
    KTEST_CHECK(!cpufreq_policy_valid(&p));
}

static void test_a_critical_temperature_goes_straight_to_the_bottom(void) {
    struct cpufreq_policy p = board_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    /* From the top, and under full load, which must not matter: at the
     * critical temperature the answer is the lowest point and not one step
     * down from where it was. */
    KTEST_CHECK(cpufreq_decide(&p, 3u, 96000, 100u, &why) == 0u);
    KTEST_CHECK(why == CPUFREQ_CRITICAL);
}

static void test_over_temperature_steps_down_one_at_a_time(void) {
    struct cpufreq_policy p = board_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    KTEST_CHECK(cpufreq_decide(&p, 3u, 86000, 100u, &why) == 2u);
    KTEST_CHECK(why == CPUFREQ_THROTTLE);
    /* And keeps stepping while it is still hot, rather than jumping. */
    KTEST_CHECK(cpufreq_decide(&p, 2u, 86000, 100u, &why) == 1u);
}

static void test_load_climbs_and_falls_one_step(void) {
    struct cpufreq_policy p = board_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    KTEST_CHECK(cpufreq_decide(&p, 0u, 40000, 90u, &why) == 1u);
    KTEST_CHECK(why == CPUFREQ_LOAD_UP);
    KTEST_CHECK(cpufreq_decide(&p, 2u, 40000, 10u, &why) == 1u);
    KTEST_CHECK(why == CPUFREQ_LOAD_DOWN);
}

static void test_the_ends_of_the_table_hold(void) {
    struct cpufreq_policy p = board_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    /* At the top under load, and at the bottom idle: nowhere to go, and the
     * answer must be the index itself rather than one past either end. */
    KTEST_CHECK(cpufreq_decide(&p, 3u, 40000, 100u, &why) == 3u);
    KTEST_CHECK(cpufreq_decide(&p, 0u, 40000, 0u, &why) == 0u);
}

static void test_hysteresis_stops_it_oscillating(void) {
    struct cpufreq_policy p = board_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    /* Just below the throttle point, under load: it must **not** climb
     * straight back, or a hot chip bounces between two points for ever and
     * spends its time switching. The hysteresis is 5 degrees, so 81 is still
     * inside it. */
    KTEST_CHECK(cpufreq_decide(&p, 2u, 81000, 100u, &why) == 2u);
    KTEST_CHECK(why == CPUFREQ_HOLD);
    /* Far enough below, it climbs. */
    KTEST_CHECK(cpufreq_decide(&p, 2u, 60000, 100u, &why) == 3u);
    KTEST_CHECK(why == CPUFREQ_LOAD_UP);
}

static void test_an_impossible_reading_is_treated_as_the_worst_case(void) {
    struct cpufreq_policy p = board_policy();
    enum cpufreq_reason why = CPUFREQ_HOLD;
    /* The ARM project's lesson, as a test: a sensor that reports -264 C is
     * broken, and a governor that believes it goes to the top of the table
     * with nothing watching the temperature. Whatever this does, it must not
     * be "climb". */
    uint32_t at_cold = cpufreq_decide(&p, 1u, -264000, 100u, &why);
    KTEST_CHECK(at_cold <= 2u);
}

int main(void) {
    KTEST_RUN(test_the_boards_real_table_is_valid);
    KTEST_RUN(test_a_table_out_of_order_is_refused);
    KTEST_RUN(test_an_empty_table_is_refused);
    KTEST_RUN(test_overlapping_load_limits_are_refused);
    KTEST_RUN(test_a_critical_temperature_goes_straight_to_the_bottom);
    KTEST_RUN(test_over_temperature_steps_down_one_at_a_time);
    KTEST_RUN(test_load_climbs_and_falls_one_step);
    KTEST_RUN(test_the_ends_of_the_table_hold);
    KTEST_RUN(test_hysteresis_stops_it_oscillating);
    KTEST_RUN(test_an_impossible_reading_is_treated_as_the_worst_case);
    return ktest_failures();
}
