#include "lib/fe25519.h"

/* p = 2^255 - 19, as limbs. */
static const uint32_t P[FE_LIMBS] = {0xffffffedu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                                     0xffffffffu, 0xffffffffu, 0xffffffffu, 0x7fffffffu};

void fe_zero(struct fe *out) {
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        out->v[i] = 0;
    }
}

void fe_one(struct fe *out) {
    fe_zero(out);
    out->v[0] = 1;
}

void fe_copy(struct fe *out, const struct fe *a) {
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        out->v[i] = a->v[i];
    }
}

/* True when a >= p, which is the only case a reduced number needs fixing
 * for after an add. */
static bool at_least_p(const uint32_t *a) {
    for (uint32_t i = FE_LIMBS; i-- > 0;) {
        if (a[i] != P[i]) {
            return a[i] > P[i];
        }
    }
    return true;
}

/* a -= p, which the caller has established is possible. */
static void take_p(uint32_t *a) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        uint64_t d = (uint64_t)a[i] - P[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
}

void fe_add(struct fe *out, const struct fe *a, const struct fe *b) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        carry += (uint64_t)a->v[i] + b->v[i];
        out->v[i] = (uint32_t)carry;
        carry >>= 32;
    }
    /* The sum is below 2p, so at most one p comes off — and a carry out of
     * the top limb means it is certainly above p. */
    if (carry != 0 || at_least_p(out->v)) {
        take_p(out->v);
    }
}

void fe_sub(struct fe *out, const struct fe *a, const struct fe *b) {
    uint64_t borrow = 0;
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        uint64_t d = (uint64_t)a->v[i] - b->v[i] - borrow;
        out->v[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
    if (borrow != 0) {
        uint64_t carry = 0; /* it went negative: add p back */
        for (uint32_t i = 0; i < FE_LIMBS; i++) {
            carry += (uint64_t)out->v[i] + P[i];
            out->v[i] = (uint32_t)carry;
            carry >>= 32;
        }
    }
}

/* ── reducing a wide number ───────────────────────────────────────────────
 *
 * Everything below works on a 16-limb buffer and uses one identity:
 *
 *     2^255 ≡ 19   (mod 2^255 - 19)
 *
 * so the bits above 255 are worth nineteen times their value and can be
 * added back into the bottom. Doing exactly that, twice, brings any
 * 512-bit product within a whisker of the field, and then at most two
 * subtractions of p finish it.
 *
 * This is the plain way rather than the quick one, and that is the point:
 * the quick ways carry delayed reductions and signed limbs, and a mistake
 * in one of those is a wrong answer in a few inputs out of 2^255 — which no
 * test finds. Nineteen and a shift can be read. */
#define WIDE_LIMBS (FE_LIMBS * 2u)

/* out = in >> 255, in place of a shift this size being obvious. */
static void shift_down_255(uint32_t *out, const uint32_t *in) {
    for (uint32_t i = 0; i < WIDE_LIMBS; i++) {
        uint32_t low = (7u + i) < WIDE_LIMBS ? in[7u + i] >> 31 : 0;
        uint32_t high = (8u + i) < WIDE_LIMBS ? in[8u + i] << 1 : 0;
        out[i] = low | high;
    }
}

/* a += 19 * b, over the whole buffer. */
static void add_19x(uint32_t *a, const uint32_t *b) {
    uint64_t carry = 0;
    for (uint32_t i = 0; i < WIDE_LIMBS; i++) {
        carry += (uint64_t)a[i] + (uint64_t)b[i] * 19u;
        a[i] = (uint32_t)carry;
        carry >>= 32;
    }
}

/* One fold: the bits above 255 come back in nineteen times over. */
static void fold_once(uint32_t *wide) {
    uint32_t hi[WIDE_LIMBS];
    shift_down_255(hi, wide);
    wide[FE_LIMBS - 1] &= 0x7fffffffu;
    for (uint32_t i = FE_LIMBS; i < WIDE_LIMBS; i++) {
        wide[i] = 0;
    }
    add_19x(wide, hi);
}

/* A wide buffer down to a field element. Two folds, because the first can
 * still leave a bit above 255, and then p itself at most twice. */
static void reduce_wide(struct fe *out, uint32_t *wide) {
    fold_once(wide);
    fold_once(wide);
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        out->v[i] = wide[i];
    }
    for (uint32_t n = 0; n < 2u && at_least_p(out->v); n++) {
        take_p(out->v);
    }
}

void fe_mul(struct fe *out, const struct fe *a, const struct fe *b) {
    uint32_t wide[WIDE_LIMBS];
    for (uint32_t i = 0; i < WIDE_LIMBS; i++) {
        wide[i] = 0;
    }
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < FE_LIMBS; j++) {
            carry += (uint64_t)wide[i + j] + (uint64_t)a->v[i] * b->v[j];
            wide[i + j] = (uint32_t)carry;
            carry >>= 32;
        }
        wide[i + FE_LIMBS] += (uint32_t)carry;
    }
    reduce_wide(out, wide);
}

void fe_sq(struct fe *out, const struct fe *a) {
    fe_mul(out, a, a);
}

void fe_neg(struct fe *out, const struct fe *a) {
    struct fe zero;
    fe_zero(&zero);
    fe_sub(out, &zero, a);
}

bool fe_is_zero(const struct fe *a) {
    uint32_t any = 0;
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        any |= a->v[i];
    }
    return any == 0;
}

bool fe_eq(const struct fe *a, const struct fe *b) {
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        if (a->v[i] != b->v[i]) {
            return false;
        }
    }
    return true;
}

bool fe_is_odd(const struct fe *a) {
    return (a->v[0] & 1u) != 0;
}

/* a to the power of an exponent given as bytes, little-endian, highest bit
 * first in the loop: square and multiply, which is all an exponentiation
 * over public data needs to be. */
static void fe_pow_bytes(struct fe *out, const struct fe *a, const uint8_t *exp, uint32_t bits) {
    struct fe acc;
    fe_one(&acc);
    for (uint32_t i = bits; i-- > 0;) {
        fe_sq(&acc, &acc);
        if (((exp[i / 8u] >> (i % 8u)) & 1u) != 0) {
            fe_mul(&acc, &acc, a);
        }
    }
    fe_copy(out, &acc);
}

/* p - 2, for the inverse, and (p - 5) / 8, for a square root. */
static const uint8_t P_MINUS_2[FE_BYTES] = {
    0xeb, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};

static const uint8_t P_MINUS_5_OVER_8[FE_BYTES] = {
    0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0f};

void fe_inv(struct fe *out, const struct fe *a) {
    fe_pow_bytes(out, a, P_MINUS_2, 255u);
}

void fe_pow_p58(struct fe *out, const struct fe *a) {
    fe_pow_bytes(out, a, P_MINUS_5_OVER_8, 253u);
}

void fe_from_bytes(struct fe *out, const uint8_t in[FE_BYTES]) {
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        out->v[i] = (uint32_t)in[i * 4u] | ((uint32_t)in[i * 4u + 1u] << 8) |
                    ((uint32_t)in[i * 4u + 2u] << 16) | ((uint32_t)in[i * 4u + 3u] << 24);
    }
    out->v[FE_LIMBS - 1] &= 0x7fffffffu; /* the top bit is x's sign, not part of y */
    if (at_least_p(out->v)) {
        take_p(out->v);
    }
}

void fe_to_bytes(uint8_t out[FE_BYTES], const struct fe *a) {
    for (uint32_t i = 0; i < FE_LIMBS; i++) {
        out[i * 4u] = (uint8_t)a->v[i];
        out[i * 4u + 1u] = (uint8_t)(a->v[i] >> 8);
        out[i * 4u + 2u] = (uint8_t)(a->v[i] >> 16);
        out[i * 4u + 3u] = (uint8_t)(a->v[i] >> 24);
    }
}
