/* fe25519 — arithmetic in the field of 2^255 - 19, which is where Ed25519
 * lives.
 *
 * Eight 32-bit limbs and schoolbook multiplication, which is the plain way
 * rather than the fast one. That is deliberate: this kernel verifies one
 * signature per boot, so the clever representations buy nothing here and
 * cost the only thing that matters — being able to read the code and
 * believe it (CLAUDE.md §2, master Rule 12).
 *
 * Nothing here is constant-time, and it does not need to be: every input
 * is public. A signature, a public key and a message are all things an
 * attacker already has; there is no secret in this kernel to leak through
 * a branch. **Signing** would be a different matter, and is not done here.
 */
#ifndef MINIKERN_LIB_FE25519_H
#define MINIKERN_LIB_FE25519_H

#include <stdbool.h>
#include <stdint.h>

#define FE_LIMBS 8u
#define FE_BYTES 32u

struct fe {
    uint32_t v[FE_LIMBS]; /* little-endian limbs of a number below 2^255 - 19 */
};

void fe_zero(struct fe *out);
void fe_one(struct fe *out);
void fe_copy(struct fe *out, const struct fe *a);

/* a + b, a - b and a * b, all reduced. */
void fe_add(struct fe *out, const struct fe *a, const struct fe *b);
void fe_sub(struct fe *out, const struct fe *a, const struct fe *b);
void fe_mul(struct fe *out, const struct fe *a, const struct fe *b);
void fe_sq(struct fe *out, const struct fe *a);

/* -a, and 1/a (zero for zero, which no caller relies on). */
void fe_neg(struct fe *out, const struct fe *a);
void fe_inv(struct fe *out, const struct fe *a);

/* a raised to (p-5)/8, which is what a square root needs. */
void fe_pow_p58(struct fe *out, const struct fe *a);

bool fe_is_zero(const struct fe *a);
bool fe_eq(const struct fe *a, const struct fe *b);
bool fe_is_odd(const struct fe *a);

/* 32 little-endian bytes in and out. Decoding ignores the top bit, as
 * Ed25519's encoding uses it for the sign of x. */
void fe_from_bytes(struct fe *out, const uint8_t in[FE_BYTES]);
void fe_to_bytes(uint8_t out[FE_BYTES], const struct fe *a);

#endif
