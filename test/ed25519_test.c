/* Verification, against signatures Go's crypto/ed25519 made.
 *
 * Both halves matter and the second one more: a verifier that returns true
 * for everything passes every positive test there is. So each good
 * signature is followed by the same one with a bit moved — in R, in S, in
 * the message, in the key — and by an S raised above the group order, and
 * all of those must be refused.
 */
#include "lib/ed25519.h"

#include "ktest.h"

#include <stdbool.h>
#include <stdint.h>

#include "ed_vectors.h"

static void the_vectors(void) {
    for (uint32_t i = 0; i < sizeof ED_CASES / sizeof ED_CASES[0]; i++) {
        const struct ed_case *c = &ED_CASES[i];
        bool got = ed25519_verify(c->key, c->sig, c->msg, c->msglen);
        if (got != c->good) {
            fprintf(stderr, "%s: got %s, want %s\n", c->what, got ? "accepted" : "refused",
                    c->good ? "accepted" : "refused");
        }
        KTEST_CHECK(got == c->good);
    }
}

/* A signature of a longer message than it was made for must fail, which is
 * the length being part of what is signed rather than an afterthought. */
static void the_length_is_signed_too(void) {
    for (uint32_t i = 0; i < sizeof ED_CASES / sizeof ED_CASES[0]; i++) {
        const struct ed_case *c = &ED_CASES[i];
        if (!c->good || c->msglen >= 64u) {
            continue;
        }
        KTEST_CHECK(!ed25519_verify(c->key, c->sig, c->msg, c->msglen + 1u));
    }
}

/* Every byte of a good signature matters: flipping any one of the 64 must
 * be refused. This is the test that would catch a verifier checking only
 * part of what it was given. */
static void every_byte_of_the_signature_matters(void) {
    uint8_t sig[ED25519_SIGNATURE];
    const struct ed_case *c = &ED_CASES[0];
    KTEST_CHECK(ed25519_verify(c->key, c->sig, c->msg, c->msglen));
    for (uint32_t i = 0; i < ED25519_SIGNATURE; i++) {
        for (uint32_t b = 0; b < ED25519_SIGNATURE; b++) {
            sig[b] = c->sig[b];
        }
        sig[i] ^= 0x40u;
        KTEST_CHECK(!ed25519_verify(c->key, sig, c->msg, c->msglen));
    }
}

/* All-zero and all-one keys and signatures are not points, or not valid
 * ones: a verifier must say so rather than fault. */
static void rubbish_is_refused(void) {
    uint8_t zeros[ED25519_SIGNATURE], ones[ED25519_SIGNATURE];
    for (uint32_t i = 0; i < ED25519_SIGNATURE; i++) {
        zeros[i] = 0;
        ones[i] = 0xff;
    }
    KTEST_CHECK(!ed25519_verify(zeros, zeros, zeros, 32));
    KTEST_CHECK(!ed25519_verify(ones, ones, zeros, 32));
    KTEST_CHECK(!ed25519_verify(zeros, ones, zeros, 32));
    KTEST_CHECK(!ed25519_verify(ones, zeros, zeros, 32));
}

int main(void) {
    KTEST_RUN(the_vectors);
    KTEST_RUN(the_length_is_signed_too);
    KTEST_RUN(every_byte_of_the_signature_matters);
    KTEST_RUN(rubbish_is_refused);
    return ktest_failures();
}
