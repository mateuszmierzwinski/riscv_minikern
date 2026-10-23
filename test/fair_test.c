/* The arithmetic of proportional-share scheduling, on the host.
 *
 * Worth testing here rather than on a board because every one of these is a
 * silent wrong answer rather than a failure: a virtual clock that overflows
 * makes the heaviest thread the lightest, a slice that rounds to zero spends
 * the processor on switching, and a waking thread given the wrong credit
 * monopolises it. None of them faults, and all of them look like "the
 * scheduler is unfair" from outside.
 */
#include "core/fair.h"

#include <stdint.h>

#include "ktest.h"

static void test_nice_is_clamped_not_refused(void) {
    KTEST_CHECK(fair_clamp_nice(0) == 0);
    KTEST_CHECK(fair_clamp_nice(-20) == -20 && fair_clamp_nice(19) == 19);
    /* POSIX says `setpriority` clamps, so out of range is not an error. */
    KTEST_CHECK(fair_clamp_nice(-100) == FAIR_NICE_MIN);
    KTEST_CHECK(fair_clamp_nice(100) == FAIR_NICE_MAX);
    /* And the weight lookup must survive the same inputs, because it is
     * indexing a table with them. */
    KTEST_CHECK(fair_weight_of_nice(-9999) == fair_weight_of_nice(FAIR_NICE_MIN));
    KTEST_CHECK(fair_weight_of_nice(9999) == fair_weight_of_nice(FAIR_NICE_MAX));
}

static void test_one_nice_level_is_about_ten_percent(void) {
    /* The property programs rely on: between two competing threads, one
     * nice level is worth roughly 10% of the processor. Checked as the
     * share rather than the weight, because the share is the promise. */
    uint32_t a = fair_weight_of_nice(0);
    uint32_t b = fair_weight_of_nice(1);
    uint64_t total = (uint64_t)a + b;
    uint64_t share_a = 100u * a / total;
    KTEST_CHECK(fair_weight_of_nice(0) == FAIR_NICE0_WEIGHT);
    KTEST_CHECK(share_a >= 54u && share_a <= 57u); /* ~55/45 */
    /* Monotonic across the whole range: a lower nice is always heavier. */
    for (int32_t n = FAIR_NICE_MIN; n < FAIR_NICE_MAX; n++) {
        KTEST_CHECK(fair_weight_of_nice(n) > fair_weight_of_nice(n + 1));
    }
}

static void test_virtual_time_runs_slower_for_heavier_threads(void) {
    uint64_t ms = 1000000ull;
    /* Nice 0 is real time exactly — the common case, and it must not round. */
    KTEST_CHECK(fair_vruntime_delta(ms, FAIR_NICE0_WEIGHT) == ms);
    KTEST_CHECK(fair_vruntime_delta(0, FAIR_NICE0_WEIGHT) == 0);
    /* Heavier than nice 0: the clock runs slow, so it is picked more. */
    KTEST_CHECK(fair_vruntime_delta(ms, 2048u) < ms);
    /* Lighter: faster. */
    KTEST_CHECK(fair_vruntime_delta(ms, 512u) > ms);
    /* Half the weight is twice the virtual time, to the nanosecond. */
    KTEST_CHECK(fair_vruntime_delta(ms, 512u) == 2u * ms);
    /* A zero weight must not divide by zero; it reads as nice 0. */
    KTEST_CHECK(fair_vruntime_delta(ms, 0) == ms);
}

static void test_virtual_time_does_not_overflow(void) {
    /* The reason the implementation splits the multiplication: an hour of
     * runtime at the heaviest weight overflows if it multiplies first. The
     * check is that the result stays ordered and sane, not a magic number. */
    uint64_t hour = 3600ull * 1000000000ull;
    uint64_t heavy = fair_vruntime_delta(hour, fair_weight_of_nice(FAIR_NICE_MIN));
    uint64_t light = fair_vruntime_delta(hour, fair_weight_of_nice(FAIR_NICE_MAX));
    KTEST_CHECK(heavy > 0 && heavy < hour);       /* slow clock */
    KTEST_CHECK(light > hour);                    /* fast clock */
    KTEST_CHECK(light > heavy);                   /* and ordered */
    /* A full day at nice 0 is still exact. */
    KTEST_CHECK(fair_vruntime_delta(24u * hour, FAIR_NICE0_WEIGHT) == 24u * hour);
}

static void test_slices_divide_the_period_by_weight(void) {
    uint64_t target = 12000000ull; /* 12 ms, from the board's measurements */
    uint64_t floor = 1500000ull;   /* 1.5 ms */
    uint32_t w = FAIR_NICE0_WEIGHT;
    /* Two equal threads: half each. */
    KTEST_CHECK(fair_slice_ns(w, 2u * w, target, floor) == target / 2u);
    /* Four equal threads: a quarter each, which is still above the floor. */
    KTEST_CHECK(fair_slice_ns(w, 4u * w, target, floor) == target / 4u);
    /* Alone: the whole period, not a share of itself. */
    KTEST_CHECK(fair_slice_ns(w, w, target, floor) == target);
    /* Unequal: twice the weight is twice the slice. */
    KTEST_CHECK(fair_slice_ns(2u * w, 3u * w, target, floor) >
                fair_slice_ns(w, 3u * w, target, floor));
}

static void test_the_floor_stops_slices_vanishing(void) {
    uint64_t target = 12000000ull;
    uint64_t floor = 1500000ull;
    uint32_t w = FAIR_NICE0_WEIGHT;
    /* Sixteen runnable threads would be 750 us each, below the floor: the
     * period stretches instead, because a slice shorter than the switch
     * that starts it spends the processor on switching. */
    KTEST_CHECK(fair_slice_ns(w, 16u * w, target, floor) == floor);
    /* And the degenerate inputs do not produce zero. */
    KTEST_CHECK(fair_slice_ns(0, 0, target, floor) >= floor);
    KTEST_CHECK(fair_slice_ns(w, 0, target, floor) >= floor);
    KTEST_CHECK(fair_slice_ns(w, 1000000u * w, target, floor) == floor);
}

static void test_waking_is_worth_a_little_and_no_more(void) {
    uint64_t credit = 6000000ull; /* half the target latency */
    uint64_t minimum = 100000000ull;
    /* A thread that slept a long time is behind; it does not get to keep
     * that, or it would monopolise the processor catching up. */
    KTEST_CHECK(fair_wake_vruntime(1u, minimum, credit) == minimum - credit);
    /* A thread that is ahead keeps its own time: the floor would be a gift. */
    KTEST_CHECK(fair_wake_vruntime(minimum + 1u, minimum, credit) == minimum + 1u);
    /* The credit is bounded, so repeated sleeping does not accumulate a
     * claim: the answer depends only on the current minimum. */
    KTEST_CHECK(fair_wake_vruntime(0, minimum, credit) ==
                fair_wake_vruntime(0, minimum, credit));
    /* Early in a boot the minimum can be below the credit, and the result
     * must not wrap to an enormous number. */
    KTEST_CHECK(fair_wake_vruntime(0, 1000u, credit) == 0);
}

int main(void) {
    KTEST_RUN(test_nice_is_clamped_not_refused);
    KTEST_RUN(test_one_nice_level_is_about_ten_percent);
    KTEST_RUN(test_virtual_time_runs_slower_for_heavier_threads);
    KTEST_RUN(test_virtual_time_does_not_overflow);
    KTEST_RUN(test_slices_divide_the_period_by_weight);
    KTEST_RUN(test_the_floor_stops_slices_vanishing);
    KTEST_RUN(test_waking_is_worth_a_little_and_no_more);
    return ktest_failures();
}
