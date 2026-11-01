/* The governor on the host: a fake sensor, a fake load and a fake divider.
 *
 * What a fake can prove is the wiring — that the sensor's reading reaches
 * the policy, that the policy's answer reaches the divider, and that the
 * governor's idea of where it is follows what the divider accepted. The
 * registers are the board's to prove (`hil` case "§4: the governor"). */
#include "core/governor.h"

#include "ktest.h"

struct fake {
    int32_t temp;
    uint32_t load;
    uint32_t khz;   /* the last frequency set */
    bool refuse;    /* the divider will not move */
    uint32_t loads; /* how often the load was asked for */
};

static int32_t fake_temp(void *self) {
    return ((struct fake *)self)->temp;
}

static uint32_t fake_load(void *self) {
    struct fake *f = self;
    f->loads++;
    return f->load;
}

static uint32_t fake_set(void *self, uint32_t khz) {
    struct fake *f = self;
    if (f->refuse) {
        return 0;
    }
    f->khz = khz;
    return khz;
}

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

static struct governor started(struct fake *f, enum governor_mode mode) {
    struct cpufreq_policy p = board_policy();
    struct governor_ports ports = {fake_temp, fake_load, fake_set, f};
    struct governor g;
    KTEST_CHECK(governor_init(&g, &p, &ports, mode));
    return g;
}

static void test_it_starts_at_the_top_and_a_cool_board_stays_there(void) {
    struct fake f = {.temp = 45000, .load = 0};
    struct governor g = started(&f, GOVERNOR_THERMAL);
    struct governor_step s = governor_tick(&g);
    KTEST_CHECK(!s.moved && s.khz == 1500000u && f.khz == 0);
    KTEST_CHECK(s.load_pct == 100u && f.loads == 0); /* thermal mode never asks */
}

static void test_heat_steps_down_one_point_a_tick_to_the_bottom(void) {
    struct fake f = {.temp = 86000};
    struct governor g = started(&f, GOVERNOR_THERMAL);
    uint32_t seen[4];
    for (uint32_t i = 0; i < 4u; i++) {
        seen[i] = governor_tick(&g).khz;
    }
    KTEST_CHECK(seen[0] == 750000u && seen[1] == 500000u && seen[2] == 375000u);
    KTEST_CHECK(seen[3] == 375000u && g.moves == 3u && f.khz == 375000u);
}

static void test_critical_goes_straight_to_the_bottom(void) {
    struct fake f = {.temp = 96000};
    struct governor g = started(&f, GOVERNOR_THERMAL);
    struct governor_step s = governor_tick(&g);
    KTEST_CHECK(s.moved && s.khz == 375000u && s.why == CPUFREQ_CRITICAL);
}

static void test_cooling_climbs_back_only_below_the_hysteresis_band(void) {
    struct fake f = {.temp = 96000};
    struct governor g = started(&f, GOVERNOR_THERMAL);
    (void)governor_tick(&g);
    f.temp = 82000; /* below throttle, inside the 5 °C band */
    KTEST_CHECK(!governor_tick(&g).moved);
    f.temp = 79000;
    KTEST_CHECK(governor_tick(&g).khz == 500000u);
}

static void test_an_unbelievable_reading_is_the_worst_case(void) {
    int32_t readings[] = {GOVERNOR_NO_READING, -264000, 200000};
    for (uint32_t i = 0; i < 3u; i++) {
        struct fake f = {.temp = readings[i]};
        struct governor g = started(&f, GOVERNOR_ONDEMAND);
        f.load = 100u;
        struct governor_step s = governor_tick(&g);
        KTEST_CHECK(s.why == CPUFREQ_CRITICAL && s.khz == 375000u && s.temp_mC == 95000);
    }
}

static void test_ondemand_follows_the_load_both_ways(void) {
    struct fake f = {.temp = 45000, .load = 5u};
    struct governor g = started(&f, GOVERNOR_ONDEMAND);
    for (uint32_t i = 0; i < 3u; i++) {
        (void)governor_tick(&g);
    }
    KTEST_CHECK(f.khz == 375000u && governor_tick(&g).why == CPUFREQ_LOAD_DOWN);
    f.load = 100u;
    struct governor_step s = governor_tick(&g);
    KTEST_CHECK(s.moved && s.khz == 500000u && s.why == CPUFREQ_LOAD_UP && f.loads == 5u);
}

static void test_a_refused_step_leaves_the_governor_where_the_hardware_is(void) {
    struct fake f = {.temp = 96000, .refuse = true};
    struct governor g = started(&f, GOVERNOR_THERMAL);
    struct governor_step s = governor_tick(&g);
    KTEST_CHECK(!s.moved && s.khz == 1500000u && g.refused == 1u && g.at == 3u);
}

static void test_an_invalid_policy_is_not_started(void) {
    struct fake f = {0};
    struct cpufreq_policy p = board_policy();
    struct governor_ports ports = {fake_temp, fake_load, fake_set, &f};
    struct governor g;
    p.critical_mC = p.throttle_mC; /* critical must be above throttle */
    KTEST_CHECK(!governor_init(&g, &p, &ports, GOVERNOR_THERMAL));
}

int main(void) {
    KTEST_RUN(test_it_starts_at_the_top_and_a_cool_board_stays_there);
    KTEST_RUN(test_heat_steps_down_one_point_a_tick_to_the_bottom);
    KTEST_RUN(test_critical_goes_straight_to_the_bottom);
    KTEST_RUN(test_cooling_climbs_back_only_below_the_hysteresis_band);
    KTEST_RUN(test_an_unbelievable_reading_is_the_worst_case);
    KTEST_RUN(test_ondemand_follows_the_load_both_ways);
    KTEST_RUN(test_a_refused_step_leaves_the_governor_where_the_hardware_is);
    KTEST_RUN(test_an_invalid_policy_is_not_started);
    return ktest_failures();
}
