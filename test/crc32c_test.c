/* CRC-32C against its published check value and against a byte-at-a-time
 * reference, at every length and alignment the eight-byte path splits. */
#include "lib/crc32c.h"

#include <stdint.h>

#include "ktest.h"

static uint32_t reference(const uint8_t *p, uint32_t n) {
    uint32_t crc = CRC32C_SEED;
    for (uint32_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (uint32_t b = 0; b < 8u; b++) {
            crc = (crc >> 1) ^ (0x82f63b78u & (uint32_t) - (int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static void test_the_check_value(void) {
    KTEST_CHECK(crc32c("123456789", 9u) == 0xe3069283u);
    KTEST_CHECK(crc32c("", 0u) == 0u);
}

static void test_every_length_and_offset_matches_the_reference(void) {
    static uint8_t buf[300];
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)(i * 37u + 11u);
    }
    for (uint32_t off = 0; off < 9u; off++) {
        for (uint32_t len = 0; off + len <= sizeof buf; len += 7u) {
            KTEST_CHECK(crc32c(buf + off, len) == reference(buf + off, len));
        }
    }
}

static void test_a_sum_in_pieces_is_the_sum_of_the_whole(void) {
    static uint8_t buf[4096];
    for (uint32_t i = 0; i < sizeof buf; i++) {
        buf[i] = (uint8_t)(i ^ (i >> 3));
    }
    uint32_t part = crc32c_update(CRC32C_SEED, buf, 1001u);
    part = crc32c_update(part, buf + 1001u, sizeof buf - 1001u);
    KTEST_CHECK(~part == crc32c(buf, sizeof buf));
}

int main(void) {
    KTEST_RUN(test_the_check_value);
    KTEST_RUN(test_every_length_and_offset_matches_the_reference);
    KTEST_RUN(test_a_sum_in_pieces_is_the_sum_of_the_whole);
    return ktest_failures();
}
