/* SHA-512 against the digests Python's hashlib gives for the same inputs,
 * chosen around the padding boundary — 111, 112, 113, 127, 128 and 129
 * bytes — because that is where a hash function is wrong if it is wrong. */
#include "lib/sha512.h"

#include "ktest.h"

#include <stdbool.h>
#include <string.h>

/* The inputs are made rather than stored: "a" repeated, or every byte once. */
static uint32_t fill(uint8_t *buf, uint32_t len, bool bytes) {
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = bytes ? (uint8_t)i : (uint8_t)'a';
    }
    return len;
}

static void check_digest(const uint8_t *in, uint32_t len, const char *want, const char *what) {
    uint8_t out[SHA512_DIGEST];
    char hex[SHA512_DIGEST * 2 + 1];
    sha512(in, len, out);
    for (uint32_t i = 0; i < SHA512_DIGEST; i++) {
        static const char digits[] = "0123456789abcdef";
        hex[i * 2] = digits[out[i] >> 4];
        hex[i * 2 + 1] = digits[out[i] & 15u];
    }
    hex[SHA512_DIGEST * 2] = '\0';
    if (strcmp(hex, want) != 0) {
        fprintf(stderr, "%s: got %s\n", what, hex);
    }
    KTEST_CHECK_STR(hex, want);
}

static void the_vectors(void) {
    uint8_t buf[256];
    check_digest(buf, 0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e", "the empty string");
    check_digest((const uint8_t *)"abc", 3, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", "\"abc\"");
    check_digest(buf, fill(buf, 111, false), "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2", "111 bytes: one short of the padding boundary");
    check_digest(buf, fill(buf, 112, false), "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca", "112 bytes: exactly at it");
    check_digest(buf, fill(buf, 113, false), "55ddd8ac210a6e18ba1ee055af84c966e0dbff091c43580ae1be703bdb85da31acf6948cf5bd90c55a20e5450f22fb89bd8d0085e39f85a86cc46abbca75e24d", "113 bytes: one past, so two blocks of padding");
    check_digest(buf, fill(buf, 127, false), "828613968b501dc00a97e08c73b118aa8876c26b8aac93df128502ab360f91bab50a51e088769a5c1eff4782ace147dce3642554199876374291f5d921629502", "127 bytes");
    check_digest(buf, fill(buf, 128, false), "b73d1929aa615934e61a871596b3f3b33359f42b8175602e89f7e06e5f658a243667807ed300314b95cacdd579f3e33abdfbe351909519a846d465c59582f321", "128 bytes: a whole block");
    check_digest(buf, fill(buf, 129, false), "4f681e0bd53cda4b5a2041cc8a06f2eabde44fb16c951fbd5b87702f07aeab611565b19c47fde30587177ebb852e3971bbd8d3fd30da18d71037dfbd98420429", "129 bytes");
    check_digest((const uint8_t *)"The quick brown fox jumps over the lazy dog", 43, "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb642e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6",
                 "the pangram");
    check_digest(buf, fill(buf, 256, true), "1e7b80bc8edc552c8feeb2780e111477e5bc70465fac1a77b29b35980c3f0ce4a036a6c9462036824bd56801e62af7e9feba5c22ed8a5af877bf7de117dcac6d", "every byte value once");
}

/* Feeding it in pieces must give the same answer as feeding it at once:
 * that is the whole contract of an incremental hash, and the place a
 * buffering bug hides. */
static void feeding_it_in_pieces_is_the_same(void) {
    uint8_t buf[256], whole[SHA512_DIGEST], pieces[SHA512_DIGEST];
    struct sha512 s;
    fill(buf, sizeof buf, true);
    sha512(buf, sizeof buf, whole);
    for (uint32_t cut = 1; cut < sizeof buf; cut += 7u) {
        sha512_init(&s);
        sha512_update(&s, buf, cut);
        sha512_update(&s, buf + cut, (uint32_t)sizeof buf - cut);
        sha512_final(&s, pieces);
        KTEST_CHECK(memcmp(whole, pieces, SHA512_DIGEST) == 0);
    }
}

int main(void) {
    KTEST_RUN(the_vectors);
    KTEST_RUN(feeding_it_in_pieces_is_the_same);
    return ktest_failures();
}
