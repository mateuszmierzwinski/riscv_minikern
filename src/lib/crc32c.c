#include "lib/crc32c.h"

/* The reflected CRC-32C polynomial, eight bytes per step ("slicing by
 * eight"): table[0] is the ordinary byte table and table[k][i] is byte i's
 * remainder pushed through k more zero bytes, so eight table lookups fold
 * a whole 64-bit word. Built at first use, so the table is .bss and the
 * source states the polynomial rather than a page of constants.
 *
 * **Eight kilobytes for an eighth of the time.** The nibble table this
 * replaces was 64 bytes and spent 93% of a metadata-heavy write test
 * (`xfs_write_test`'s 17,000-inode step: 13 of 14 seconds, 3.9 million
 * calls) folding directory blocks four bits at a time. */
#define CRC32C_POLY 0x82f63b78u

static uint32_t table[8][256];
static int ready;

static void build_table(void) {
    for (uint32_t i = 0; i < 256u; i++) {
        uint32_t crc = i;
        for (uint32_t bit = 0; bit < 8u; bit++) {
            crc = (crc >> 1) ^ (CRC32C_POLY & (uint32_t) - (int32_t)(crc & 1u));
        }
        table[0][i] = crc;
    }
    for (uint32_t i = 0; i < 256u; i++) {
        for (uint32_t k = 1; k < 8u; k++) {
            table[k][i] = (table[k - 1u][i] >> 8) ^ table[0][table[k - 1u][i] & 0xffu];
        }
    }
    ready = 1;
}

/* One byte. */
static uint32_t fold_byte(uint32_t crc, uint8_t b) {
    return (crc >> 8) ^ table[0][(crc ^ b) & 0xffu];
}

/* Eight bytes, little-endian as they are in memory. */
static uint32_t fold_eight(uint32_t crc, const uint8_t *p) {
    uint32_t lo = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                         (uint32_t)p[3] << 24);
    uint32_t hi = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 |
                  (uint32_t)p[7] << 24;
    return table[7][lo & 0xffu] ^ table[6][(lo >> 8) & 0xffu] ^ table[5][(lo >> 16) & 0xffu] ^
           table[4][lo >> 24] ^ table[3][hi & 0xffu] ^ table[2][(hi >> 8) & 0xffu] ^
           table[1][(hi >> 16) & 0xffu] ^ table[0][hi >> 24];
}

uint32_t crc32c_update(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *at = data;
    uint32_t i = 0;
    if (!ready) {
        build_table();
    }
    for (; i + 8u <= len; i += 8u) {
        crc = fold_eight(crc, at + i);
    }
    for (; i < len; i++) {
        crc = fold_byte(crc, at[i]);
    }
    return crc;
}

uint32_t crc32c(const void *data, uint32_t len) {
    return ~crc32c_update(CRC32C_SEED, data, len);
}
