/* FAT's DOS timestamps both ways: what the writer stamps must read back as
 * the same second (to its two-second resolution), and a date that was never
 * set must read as no time rather than as 1980. */
#include "core/fat/fat.h"
#include "core/fat/fat_internal.h"

#include "ktest.h"

static void test_a_known_date(void) {
    uint16_t t = 0, d = 0;
    fat_dos_time(1793304000u, &t, &d); /* 2026-10-29 00:40:00 UTC */
    KTEST_CHECK(d == (uint16_t)(((2026u - 1980u) << 9) | (10u << 5) | 29u));
    KTEST_CHECK(fat_unix_time(t, d) == 1793304000u);
}

static void test_round_trip_across_years_and_leap_days(void) {
    const uint64_t samples[] = {315532800u /* 1980-01-01 */, 951782400u /* 2000-02-29 */,
                                1709164800u /* 2024-02-29 */, 4102444798u /* 2099-12-31 */};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; i++) {
        uint16_t t = 0, d = 0;
        fat_dos_time(samples[i] + 7u, &t, &d);
        KTEST_CHECK(fat_unix_time(t, d) == samples[i] + 6u); /* two-second steps */
    }
}

static void test_an_unset_or_broken_date_is_no_time(void) {
    KTEST_CHECK(fat_unix_time(0, 0) == 0u);
    KTEST_CHECK(fat_unix_time(0, (uint16_t)((10u << 9) | (13u << 5) | 1u)) == 0u); /* month 13 */
    KTEST_CHECK(fat_unix_time(0, (uint16_t)((10u << 9) | (1u << 5))) == 0u);       /* day 0 */
}

int main(void) {
    KTEST_RUN(test_a_known_date);
    KTEST_RUN(test_round_trip_across_years_and_leap_days);
    KTEST_RUN(test_an_unset_or_broken_date_is_no_time);
    return ktest_failures();
}
