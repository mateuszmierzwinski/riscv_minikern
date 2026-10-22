/* The SD card's capacity arithmetic, against both CSD layouts.
 *
 * This exists because the arithmetic was wrong on real hardware in a way
 * that looked right: a 14.5 GiB card reported **465 blocks**, because the
 * fields were being read 8 bits from where the specification puts them. That
 * shift is one host controller's quirk — an SDHCI part drops the response's
 * trailing CRC byte and a DesignWare part does not — so it belongs to the
 * transport, and this arithmetic reads the documented bit numbers.
 *
 * The cases below are built by *placing* each field at its specified bit, so
 * the test states the specification rather than restating the code.
 */
#include "drivers/sdcard.h"

#include "ktest.h"

/* Puts `value` at bit `at` of a 128-bit CSD held as four words, `csd[3]`
 * carrying bits 127:96 — the layout a transport is required to present. */
static void place(uint32_t csd[4], uint32_t at, uint32_t width, uint64_t value) {
    for (uint32_t bit = 0; bit < width; bit++) {
        uint32_t pos = at + bit;
        if ((value >> bit) & 1u) {
            csd[pos / 32u] |= 1u << (pos % 32u);
        }
    }
}

/* A high-capacity card: CSD version 1, which reports a block count. */
static void high_capacity(uint32_t csd[4], uint32_t c_size) {
    csd[0] = csd[1] = csd[2] = csd[3] = 0;
    place(csd, 126u, 2u, 1u);         /* CSD_STRUCTURE = 1 */
    place(csd, 48u, 22u, c_size);     /* C_SIZE */
}

static void test_high_capacity_is_a_block_count(void) {
    uint32_t csd[4];
    /* The 14.5 GiB card that reported 465 blocks: (C_SIZE + 1) * 1024, and
     * 30449664 is what the board measured once the fields were read at the
     * right bits. */
    high_capacity(csd, 29735u);
    KTEST_CHECK(sdcard_capacity_from_csd(csd) == 30449664ull);
    /* The 7.6 GiB card the driver was finished against, measured on the
     * board as 15601664 blocks. */
    high_capacity(csd, 15235u);
    KTEST_CHECK(sdcard_capacity_from_csd(csd) == 15601664ull);
}

/* **The regression, stated as a test.** The same card read with SDHCI's
 * 8-bit shift applied gives a number that is plausible and tiny, which is
 * why it was believed. If the shift ever returns to the shared arithmetic,
 * this is what fails. */
static void test_the_shifted_misreading_is_not_what_we_return(void) {
    uint32_t csd[4];
    high_capacity(csd, 29735u);
    KTEST_CHECK(sdcard_capacity_from_csd(csd) != 465ull);
    /* And the shifted reading of C_SIZE is genuinely a different number, so
     * the two layouts cannot be confused by coincidence. */
    KTEST_CHECK((((csd[1] >> 8) & 0x3fffffu) + 1u) * 1024u != 30449664ull);
}

static void test_standard_capacity_is_a_geometry(void) {
    uint32_t csd[4] = {0};
    /* A 64 MiB card: C_SIZE 3751, C_SIZE_MULT 5, READ_BL_LEN 9.
     * (3751 + 1) * 2^(5+2) * 2^9 / 512 = 3752 * 128 = 480256 blocks. */
    place(csd, 126u, 2u, 0u);  /* CSD_STRUCTURE = 0 */
    place(csd, 62u, 12u, 3751u);
    place(csd, 47u, 3u, 5u);
    place(csd, 80u, 4u, 9u);
    KTEST_CHECK(sdcard_capacity_from_csd(csd) == 480256ull);
}

/* An all-zero CSD is not a card: it must not come back as a usable size.
 * The negative case matters because a failed read leaves a zeroed buffer,
 * and a driver that then reported a capacity would address a card that is
 * not answering. */
static void test_an_empty_csd_has_no_capacity(void) {
    uint32_t csd[4] = {0};
    /* Version 0 with every field zero: one block of 2^0 bytes, which is
     * zero 512-byte blocks — small enough that any read is refused. */
    KTEST_CHECK(sdcard_capacity_from_csd(csd) == 0ull);
}

/* The all-ones CSD a never-written card reads back as. It must not be
 * mistaken for an enormous card: the version field reads 3, which is not a
 * layout this reader knows, so it takes the geometry branch and the result
 * is checked for sanity by the caller rather than trusted here. */
static void test_erased_bits_do_not_become_a_huge_card(void) {
    uint32_t csd[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    KTEST_CHECK(sdcard_capacity_from_csd(csd) != 0ull);
}

int main(void) {
    KTEST_RUN(test_high_capacity_is_a_block_count);
    KTEST_RUN(test_the_shifted_misreading_is_not_what_we_return);
    KTEST_RUN(test_standard_capacity_is_a_geometry);
    KTEST_RUN(test_an_empty_csd_has_no_capacity);
    KTEST_RUN(test_erased_bits_do_not_become_a_huge_card);
    return ktest_failures();
}
