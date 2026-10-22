#include "lib/crc32c.h"

/* The reflected CRC-32C polynomial, four bits per step. Entry i is the
 * remainder of the nibble i, computed once at first use so the table is
 * .bss rather than .rodata and the source states the polynomial instead of
 * a page of constants somebody has to trust. */
#define CRC32C_POLY 0x82f63b78u

static uint32_t table[16];
static int ready;

static void build_table(void) {
    for (uint32_t i = 0; i < 16u; i++) {
        uint32_t crc = i;
        for (uint32_t bit = 0; bit < 4u; bit++) {
            crc = (crc >> 1) ^ (CRC32C_POLY & (uint32_t) - (int32_t)(crc & 1u));
        }
        table[i] = crc;
    }
    ready = 1;
}

uint32_t crc32c_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *at = data;
    if (!ready) {
        build_table();
    }
    for (uint32_t i = 0; i < len; i++) {
        crc ^= at[i];
        crc = (crc >> 4) ^ table[crc & 0xfu];
        crc = (crc >> 4) ^ table[crc & 0xfu];
    }
    return crc;
}

uint32_t crc32c(const void *data, uint32_t len) {
    return ~crc32c_update(CRC32C_SEED, data, len);
}
