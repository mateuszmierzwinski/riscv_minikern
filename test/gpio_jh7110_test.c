/* Where a pin's bits live, on the host.
 *
 * This is the half of a GPIO driver that can be proved without the board,
 * and it is the half that matters most: the JH7110 puts a pin's output
 * enable and output value in a **byte each**, not a bit in a word, so
 * writing a bit where a byte belongs lands on a neighbouring pin. On a board
 * wired to real hardware, a GPIO driver that sets the wrong pin is how
 * something catches fire — and no boot log would show it.
 *
 * The behaviour of those registers is not tested here and cannot be: no
 * emulator models this controller (see the header).
 */
#include "drivers/gpio_jh7110.h"

#include "ktest.h"

/* A byte per pin, in two tables 64 bytes apart. The spacing is the evidence:
 * the output-value table starts exactly where 64 single-byte entries end. */
static void test_a_byte_per_pin(void) {
    KTEST_CHECK(gpio_jh7110_doen_offset(0) == 0x000u);
    KTEST_CHECK(gpio_jh7110_doen_offset(1) == 0x001u);
    KTEST_CHECK(gpio_jh7110_doen_offset(63) == 0x03fu);
    KTEST_CHECK(gpio_jh7110_dout_offset(0) == 0x040u);
    KTEST_CHECK(gpio_jh7110_dout_offset(63) == 0x07fu);
    /* The two tables must not overlap, which is what a bit-per-pin
     * assumption would produce. */
    KTEST_CHECK(gpio_jh7110_doen_offset(63) < gpio_jh7110_dout_offset(0));
    /* And consecutive pins must be consecutive bytes, not the same byte. */
    for (uint32_t pin = 1; pin < JH7110_SYS_GPIOS; pin++) {
        KTEST_CHECK(gpio_jh7110_doen_offset(pin) == gpio_jh7110_doen_offset(pin - 1u) + 1u);
        KTEST_CHECK(gpio_jh7110_dout_offset(pin) == gpio_jh7110_dout_offset(pin - 1u) + 1u);
    }
}

/* Inputs are the conventional shape: one bit per pin, two words. Mixing the
 * two layouts up is the mistake this file exists to prevent. */
static void test_inputs_are_bits_in_words(void) {
    KTEST_CHECK(gpio_jh7110_input_offset(0) == 0x118u);
    KTEST_CHECK(gpio_jh7110_input_offset(31) == 0x118u);
    KTEST_CHECK(gpio_jh7110_input_offset(32) == 0x11cu);
    KTEST_CHECK(gpio_jh7110_input_offset(63) == 0x11cu);
    KTEST_CHECK(gpio_jh7110_input_bit(0) == 0);
    KTEST_CHECK(gpio_jh7110_input_bit(31) == 31u);
    KTEST_CHECK(gpio_jh7110_input_bit(32) == 0);
    KTEST_CHECK(gpio_jh7110_input_bit(63) == 31u);
    /* Every pin must map to exactly one (word, bit) pair, and no two pins to
     * the same one. */
    for (uint32_t a = 0; a < JH7110_SYS_GPIOS; a++) {
        for (uint32_t b = a + 1u; b < JH7110_SYS_GPIOS; b++) {
            bool same = gpio_jh7110_input_offset(a) == gpio_jh7110_input_offset(b) &&
                        gpio_jh7110_input_bit(a) == gpio_jh7110_input_bit(b);
            KTEST_CHECK(!same);
        }
    }
}

/* The input words must not land inside the output tables. */
static void test_the_tables_do_not_collide(void) {
    for (uint32_t pin = 0; pin < JH7110_SYS_GPIOS; pin++) {
        KTEST_CHECK(gpio_jh7110_input_offset(pin) > gpio_jh7110_dout_offset(JH7110_SYS_GPIOS - 1u));
    }
}

int main(void) {
    KTEST_RUN(test_a_byte_per_pin);
    KTEST_RUN(test_inputs_are_bits_in_words);
    KTEST_RUN(test_the_tables_do_not_collide);
    return ktest_failures();
}
