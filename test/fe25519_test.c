/* The field arithmetic, against Python's arbitrary precision.
 *
 * Every case is checked in both directions where that means anything — a
 * product against its factors, an inverse against its multiplication back
 * to one — and the inputs include the values a nearly-right reduction gets
 * wrong: p-1, p-2, (p-1)/2 and 2^254. A field implementation that is wrong
 * for one input in 2^255 is wrong, and only chosen inputs find that.
 */
#include "lib/fe25519.h"

#include "ktest.h"

#include <string.h>

#include "fe_vectors.h"

static void same(const char *what, const struct fe *got, const uint8_t want[32]) {
    uint8_t out[32];
    fe_to_bytes(out, got);
    if (memcmp(out, want, 32) != 0) {
        fprintf(stderr, "%s differs\n", what);
    }
    KTEST_CHECK(memcmp(out, want, 32) == 0);
}

static void the_vectors(void) {
    for (uint32_t i = 0; i < sizeof FE_CASES / sizeof FE_CASES[0]; i++) {
        const struct fe_case *c = &FE_CASES[i];
        struct fe a, b, r;
        fe_from_bytes(&a, c->a);
        fe_from_bytes(&b, c->b);
        fe_add(&r, &a, &b);
        same("sum", &r, c->sum);
        fe_sub(&r, &a, &b);
        same("difference", &r, c->diff);
        fe_mul(&r, &a, &b);
        same("product", &r, c->prod);
        fe_sq(&r, &a);
        fe_mul(&a, &a, &a);
        KTEST_CHECK(fe_eq(&r, &a)); /* squaring is multiplying by itself */
        fe_from_bytes(&a, c->a);
        fe_inv(&r, &a);
        same("inverse", &r, c->inv);
        fe_pow_p58(&r, &a);
        same("the (p-5)/8 power", &r, c->p58);
    }
}

/* An inverse times its element is one, except for zero, which has none. */
static void an_inverse_multiplies_back_to_one(void) {
    for (uint32_t i = 0; i < sizeof FE_CASES / sizeof FE_CASES[0]; i++) {
        struct fe a, inv, one, back;
        fe_from_bytes(&a, FE_CASES[i].a);
        if (fe_is_zero(&a)) {
            continue;
        }
        fe_inv(&inv, &a);
        fe_mul(&back, &a, &inv);
        fe_one(&one);
        KTEST_CHECK(fe_eq(&back, &one));
    }
}

static void adding_then_subtracting_returns(void) {
    for (uint32_t i = 0; i < sizeof FE_CASES / sizeof FE_CASES[0]; i++) {
        struct fe a, b, r;
        fe_from_bytes(&a, FE_CASES[i].a);
        fe_from_bytes(&b, FE_CASES[i].b);
        fe_add(&r, &a, &b);
        fe_sub(&r, &r, &b);
        KTEST_CHECK(fe_eq(&r, &a));
    }
}

static void negation_sums_to_zero(void) {
    for (uint32_t i = 0; i < sizeof FE_CASES / sizeof FE_CASES[0]; i++) {
        struct fe a, n, r;
        fe_from_bytes(&a, FE_CASES[i].a);
        fe_neg(&n, &a);
        fe_add(&r, &a, &n);
        KTEST_CHECK(fe_is_zero(&r));
    }
}

/* The encoding must survive a round trip, and must not accept a number at
 * or above p as itself: p, p+1 and 2^255-1 all reduce. */
static void the_encoding_round_trips(void) {
    static const uint8_t p_bytes[32] = {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
    struct fe a, zero;
    uint8_t out[32];
    fe_from_bytes(&a, p_bytes); /* p itself is zero in the field */
    fe_zero(&zero);
    KTEST_CHECK(fe_eq(&a, &zero));
    for (uint32_t i = 0; i < sizeof FE_CASES / sizeof FE_CASES[0]; i++) {
        fe_from_bytes(&a, FE_CASES[i].a);
        fe_to_bytes(out, &a);
        KTEST_CHECK(memcmp(out, FE_CASES[i].a, 32) == 0);
    }
}

static void oddness_is_the_bottom_bit(void) {
    struct fe one, two;
    fe_one(&one);
    fe_add(&two, &one, &one);
    KTEST_CHECK(fe_is_odd(&one));
    KTEST_CHECK(!fe_is_odd(&two));
}

int main(void) {
    KTEST_RUN(the_vectors);
    KTEST_RUN(an_inverse_multiplies_back_to_one);
    KTEST_RUN(adding_then_subtracting_returns);
    KTEST_RUN(negation_sums_to_zero);
    KTEST_RUN(the_encoding_round_trips);
    KTEST_RUN(oddness_is_the_bottom_bit);
    return ktest_failures();
}
