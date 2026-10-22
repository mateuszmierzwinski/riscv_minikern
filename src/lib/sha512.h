/* sha512 — SHA-512, because Ed25519 is defined in terms of it.
 *
 * The same shape as the SHA-256 beside it, with 64-bit words, 80 rounds and
 * a 128-byte block. Its constants were generated rather than transcribed
 * (they are the fractional parts of the cube roots of the first eighty
 * primes), because eighty-eight 64-bit numbers copied by hand is eighty-
 * eight chances to be wrong in a way only a test vector would catch.
 */
#ifndef MINIKERN_LIB_SHA512_H
#define MINIKERN_LIB_SHA512_H

#include <stdint.h>

#define SHA512_DIGEST 64u
#define SHA512_BLOCK  128u

struct sha512 {
    uint64_t h[8];
    uint64_t len;              /* bytes fed so far */
    uint8_t buf[SHA512_BLOCK]; /* the partial block */
    uint32_t have;
};

void sha512_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, uint32_t len);
void sha512_final(struct sha512 *s, uint8_t out[SHA512_DIGEST]);

/* The whole digest of one buffer. */
void sha512(const void *data, uint32_t len, uint8_t out[SHA512_DIGEST]);

#endif
