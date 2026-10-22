/* Where a clock's and a reset's bits live, on the host.
 *
 * This is the half of a clock driver that can be proved without the board,
 * and it is the half that must not be wrong: a reset id that resolves to the
 * wrong bank writes into a *different* peripheral's reset line, which takes
 * a board down in a way nothing explains. The registers' behaviour is not
 * tested and cannot be — no emulator models this generator.
 */
#include "drivers/crg_jh7110.h"

#include "ktest.h"

/* One word per clock, so index and offset move together and a clock never
 * shares a register with its neighbour. */
static void test_a_word_per_clock(void) {
    KTEST_CHECK(crg_clock_offset(0) == 0u);
    KTEST_CHECK(crg_clock_offset(1) == 4u);
    KTEST_CHECK(crg_clock_offset(JH7110_SYSCLK_IOMUX_APB) == 112u * 4u);
    for (uint32_t i = 1; i < 200u; i++) {
        KTEST_CHECK(crg_clock_offset(i) == crg_clock_offset(i - 1u) + 4u);
    }
}

/* Resets are banked 32 to a word. The boundaries are where an off-by-one
 * sends a release into the wrong bank entirely. */
static void test_resets_are_banked_by_thirty_two(void) {
    KTEST_CHECK(crg_reset_bank(0) == 0u && crg_reset_bit(0) == 0u);
    KTEST_CHECK(crg_reset_bank(31u) == 0u && crg_reset_bit(31u) == 31u);
    KTEST_CHECK(crg_reset_bank(32u) == 4u && crg_reset_bit(32u) == 0u);
    KTEST_CHECK(crg_reset_bank(63u) == 4u && crg_reset_bit(63u) == 31u);
    KTEST_CHECK(crg_reset_bank(64u) == 8u && crg_reset_bit(64u) == 0u);
    KTEST_CHECK(crg_reset_bank(125u) == 12u);
    /* The one this kernel actually releases. */
    KTEST_CHECK(crg_reset_bank(JH7110_SYSRST_IOMUX_APB) == 0u);
    KTEST_CHECK(crg_reset_bit(JH7110_SYSRST_IOMUX_APB) == 31u);
}

/* Every id in the system generator's range must map to a distinct
 * (bank, bit) pair — the property that makes "release one reset" mean one. */
static void test_every_reset_is_its_own_bit(void) {
    for (uint32_t a = 0; a < 126u; a++) {
        for (uint32_t b = a + 1u; b < 126u; b++) {
            bool same = crg_reset_bank(a) == crg_reset_bank(b) &&
                        crg_reset_bit(a) == crg_reset_bit(b);
            KTEST_CHECK(!same);
        }
    }
}

int main(void) {
    KTEST_RUN(test_a_word_per_clock);
    KTEST_RUN(test_resets_are_banked_by_thirty_two);
    KTEST_RUN(test_every_reset_is_its_own_bit);
    return ktest_failures();
}
