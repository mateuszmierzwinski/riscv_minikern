#include "lib/ed25519.h"

#include "lib/fe25519.h"
#include "lib/sha512.h"

/* The curve's constants, generated rather than transcribed: d is
 * -121665/121666, the square root of -1 is 2^((p-1)/4), the base point's y
 * is 4/5 with the even root for x, and L is the order of the subgroup the
 * keys live in. */
static const uint8_t ED_D[32] = {
    0xa3, 0x78, 0x59, 0x13, 0xca, 0x4d, 0xeb, 0x75,
    0xab, 0xd8, 0x41, 0x41, 0x4d, 0x0a, 0x70, 0x00,
    0x98, 0xe8, 0x79, 0x77, 0x79, 0x40, 0xc7, 0x8c,
    0x73, 0xfe, 0x6f, 0x2b, 0xee, 0x6c, 0x03, 0x52,
};

static const uint8_t ED_SQRT_M1[32] = {
    0xb0, 0xa0, 0x0e, 0x4a, 0x27, 0x1b, 0xee, 0xc4,
    0x78, 0xe4, 0x2f, 0xad, 0x06, 0x18, 0x43, 0x2f,
    0xa7, 0xd7, 0xfb, 0x3d, 0x99, 0x00, 0x4d, 0x2b,
    0x0b, 0xdf, 0xc1, 0x4f, 0x80, 0x24, 0x83, 0x2b,
};

static const uint8_t ED_BASE_X[32] = {
    0x1a, 0xd5, 0x25, 0x8f, 0x60, 0x2d, 0x56, 0xc9,
    0xb2, 0xa7, 0x25, 0x95, 0x60, 0xc7, 0x2c, 0x69,
    0x5c, 0xdc, 0xd6, 0xfd, 0x31, 0xe2, 0xa4, 0xc0,
    0xfe, 0x53, 0x6e, 0xcd, 0xd3, 0x36, 0x69, 0x21,
};

static const uint8_t ED_BASE_Y[32] = {
    0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
};

static const uint8_t ED_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10,
};

/* A point in extended coordinates: x = X/Z, y = Y/Z and T = XY/Z. The
 * fourth coordinate is what makes addition one formula for every pair,
 * including a point and itself. */
struct point {
    struct fe x, y, z, t;
};

static void point_zero(struct point *p) {
    fe_zero(&p->x);
    fe_one(&p->y);
    fe_one(&p->z);
    fe_zero(&p->t);
}

/* P + Q, by the unified formula for extended coordinates: it needs no
 * special case for doubling, which is the reason for carrying T. */
static void point_add(struct point *out, const struct point *p, const struct point *q) {
    struct fe a, b, c, dd, e, f, g, h, two_d;
    fe_from_bytes(&two_d, ED_D);
    fe_add(&two_d, &two_d, &two_d);
    fe_sub(&a, &p->y, &p->x);
    fe_sub(&b, &q->y, &q->x);
    fe_mul(&a, &a, &b);          /* A = (Y1-X1)(Y2-X2) */
    fe_add(&b, &p->y, &p->x);
    fe_add(&c, &q->y, &q->x);
    fe_mul(&b, &b, &c);          /* B = (Y1+X1)(Y2+X2) */
    fe_mul(&c, &p->t, &q->t);
    fe_mul(&c, &c, &two_d);      /* C = 2d T1 T2 */
    fe_mul(&dd, &p->z, &q->z);
    fe_add(&dd, &dd, &dd);       /* D = 2 Z1 Z2 */
    fe_sub(&e, &b, &a);
    fe_sub(&f, &dd, &c);
    fe_add(&g, &dd, &c);
    fe_add(&h, &b, &a);
    fe_mul(&out->x, &e, &f);
    fe_mul(&out->y, &g, &h);
    fe_mul(&out->t, &e, &h);
    fe_mul(&out->z, &f, &g);
}

/* [n]P for a scalar given as little-endian bytes: double and add, highest
 * bit first. Nothing here is secret, so the simple form is the right one. */
static void point_mul(struct point *out, const uint8_t *n, uint32_t bytes, const struct point *p) {
    struct point acc, tmp;
    point_zero(&acc);
    for (uint32_t i = bytes * 8u; i-- > 0;) {
        point_add(&tmp, &acc, &acc);
        if (((n[i / 8u] >> (i % 8u)) & 1u) != 0) {
            point_add(&acc, &tmp, p);
        } else {
            acc = tmp;
        }
    }
    *out = acc;
}

/* The square root of u/v, or false when it has none — which is how an
 * encoding that is not a point on the curve is caught. */
static bool sqrt_ratio(struct fe *out, const struct fe *u, const struct fe *v) {
    struct fe v3, v7, r, check, uv, m1;
    fe_sq(&v3, v);
    fe_mul(&v3, &v3, v);           /* v^3 */
    fe_sq(&v7, &v3);
    fe_mul(&v7, &v7, v);           /* v^7 */
    fe_mul(&uv, u, &v7);
    fe_pow_p58(&r, &uv);           /* (u v^7)^((p-5)/8) */
    fe_mul(&r, &r, &v3);
    fe_mul(&r, &r, u);             /* x = u v^3 (u v^7)^((p-5)/8) */
    fe_sq(&check, &r);
    fe_mul(&check, &check, v);     /* v x^2 */
    if (fe_eq(&check, u)) {
        fe_copy(out, &r);
        return true;
    }
    fe_from_bytes(&m1, ED_SQRT_M1);
    fe_mul(&r, &r, &m1);           /* the other root, if there is one */
    fe_sq(&check, &r);
    fe_mul(&check, &check, v);
    if (fe_eq(&check, u)) {
        fe_copy(out, &r);
        return true;
    }
    return false;
}

/* A 32-byte encoding back into a point: y is the low 255 bits, and the top
 * bit is the sign of x. False when no such point exists. */
static bool point_decode(struct point *out, const uint8_t in[32]) {
    struct fe u, v, d, y2, x;
    fe_from_bytes(&out->y, in);
    fe_sq(&y2, &out->y);
    fe_one(&u);
    fe_sub(&u, &y2, &u);           /* u = y^2 - 1 */
    fe_from_bytes(&d, ED_D);
    fe_mul(&v, &d, &y2);
    fe_one(&d);
    fe_add(&v, &v, &d);            /* v = d y^2 + 1 */
    if (!sqrt_ratio(&x, &u, &v)) {
        return false;
    }
    if (fe_is_odd(&x) != (((in[31] >> 7) & 1u) != 0)) {
        fe_neg(&x, &x);
    }
    /* x = 0 with the sign bit set names no point: it is the one encoding
     * that passes the arithmetic and is still not canonical. */
    if (fe_is_zero(&x) && ((in[31] >> 7) & 1u) != 0) {
        return false;
    }
    fe_copy(&out->x, &x);
    fe_one(&out->z);
    fe_mul(&out->t, &x, &out->y);
    return true;
}

/* A point back to its 32-byte encoding, which is also how two points are
 * compared: one representation, one answer. */
static void point_encode(uint8_t out[32], const struct point *p) {
    struct fe inv, x, y;
    fe_inv(&inv, &p->z);
    fe_mul(&x, &p->x, &inv);
    fe_mul(&y, &p->y, &inv);
    fe_to_bytes(out, &y);
    out[31] = (uint8_t)((out[31] & 0x7fu) | (fe_is_odd(&x) ? 0x80u : 0u));
}

/* True when s, little-endian, is below the group order. A signature with
 * an S at or above it is malleable — another S gives the same equation —
 * so it is refused rather than accepted. */
static bool scalar_in_range(const uint8_t s[32]) {
    for (uint32_t i = 32u; i-- > 0;) {
        if (s[i] != ED_L[i]) {
            return s[i] < ED_L[i];
        }
    }
    return false;
}

/* The challenge: SHA-512 of R, the key and the message.
 *
 * It is **not** reduced modulo the group order, and does not need to be:
 * an Ed25519 public key is a multiple of the base point, so it has order L,
 * and [k]A is therefore the same point for k as for k mod L. What that
 * saves is a whole second modular arithmetic — 512 bits modulo a 253-bit
 * number — for a scalar multiplication that runs twice as long, once per
 * boot. (It does mean a *maliciously constructed* key with a small-order
 * component would be treated differently from the letter of RFC 8032. The
 * key here is the operator's own, configured on the kernel command line;
 * the signature is the untrusted half.) */
static void challenge(uint8_t out[64], const uint8_t *r, const uint8_t *key, const void *msg,
                      uint32_t len) {
    struct sha512 s;
    sha512_init(&s);
    sha512_update(&s, r, 32);
    sha512_update(&s, key, ED25519_PUBLIC_KEY);
    sha512_update(&s, msg, len);
    sha512_final(&s, out);
}

/* [S]B, encoded. Its own frame: the verification's six points together
 * pass the kernel's 1 KiB stack-frame limit on riscv64. */
static __attribute__((noinline)) void scalar_times_base(const uint8_t s[32], uint8_t out[32]) {
    struct point base, sb;
    fe_from_bytes(&base.x, ED_BASE_X);
    fe_from_bytes(&base.y, ED_BASE_Y);
    fe_one(&base.z);
    fe_mul(&base.t, &base.x, &base.y);
    point_mul(&sb, s, 32, &base);
    point_encode(out, &sb);
}

/* R + [k]A, encoded; false when either point does not decode. */
static __attribute__((noinline)) bool r_plus_k_a(const uint8_t key[ED25519_PUBLIC_KEY],
                                                 const uint8_t sig[ED25519_SIGNATURE],
                                                 const uint8_t k[64], uint8_t out[32]) {
    struct point a, r, ka, sum;
    if (!point_decode(&a, key) || !point_decode(&r, sig)) {
        return false;
    }
    point_mul(&ka, k, 64, &a);
    point_add(&sum, &r, &ka);
    point_encode(out, &sum);
    return true;
}

bool ed25519_verify(const uint8_t key[ED25519_PUBLIC_KEY], const uint8_t sig[ED25519_SIGNATURE],
                    const void *msg, uint32_t len) {
    uint8_t k[64], want[32], got[32];
    if (!scalar_in_range(sig + 32)) {
        return false;
    }
    challenge(k, sig, key, msg, len);
    if (!r_plus_k_a(key, sig, k, want)) {
        return false;
    }
    scalar_times_base(sig + 32, got);
    for (uint32_t i = 0; i < 32; i++) {
        if (got[i] != want[i]) {
            return false;
        }
    }
    return true;
}
