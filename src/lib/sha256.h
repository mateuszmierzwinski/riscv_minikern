/* sha256 — SHA-256 (FIPS 180-4), written here from the specification: the
 * kernel verifies a netbooted root filesystem against a digest before it
 * runs anything out of it. Pure and streaming, so it is host-tested against
 * the published vectors. */
#ifndef MINIKERN_LIB_SHA256_H
#define MINIKERN_LIB_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST 32u
#define SHA256_BLOCK  64u

struct sha256 {
    uint32_t h[8];
    uint64_t len;              /* bytes absorbed so far */
    uint8_t buf[SHA256_BLOCK];
    uint32_t buffered;
};

void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t len);

/* Finishes and writes the digest; s must not be used again. */
void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]);

/* The digest of one buffer. */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST]);

/* Constant-time comparison: a digest check must not leak where it differs. */
int sha256_equal(const uint8_t a[SHA256_DIGEST], const uint8_t b[SHA256_DIGEST]);

/* Formats a digest as 64 lowercase hex characters plus NUL. */
void sha256_format(const uint8_t d[SHA256_DIGEST], char out[65]);

#endif
