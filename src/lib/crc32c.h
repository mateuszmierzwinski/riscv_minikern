/* crc32c — CRC-32C (Castagnoli, the iSCSI polynomial), which is what every
 * v5 XFS metadata block carries.
 *
 * It is the reflected algorithm over polynomial 0x82f63b78, seeded with
 * 0xffffffff, and the value XFS stores is the complement of the result,
 * little-endian, in a field that was zero while the sum was taken
 * (core/xfs/xfs.h's checksum helper does that part).
 *
 * Eight bytes at a time, from an 8 KiB table (crc32c.c says why). */
#ifndef MINIKERN_LIB_CRC32C_H
#define MINIKERN_LIB_CRC32C_H

#include <stdint.h>

/* Folds len bytes into crc. Start a new sum with CRC32C_SEED. */
#define CRC32C_SEED 0xffffffffu
uint32_t crc32c_update(uint32_t crc, const void *data, uint32_t len);

/* The whole sum of one buffer, complemented: what a verifier compares. */
uint32_t crc32c(const void *data, uint32_t len);

#endif
