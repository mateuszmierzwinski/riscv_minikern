#include "kernel/entropy.h"

#include "lib/sha256.h"

#define POOL_WORDS 16u

/* The pool, and the samples that have gone into it. Written from the
 * interrupt path, so what happens there is deliberately cheap: a rotate and
 * an exclusive-or, with the hashing done when the pool is read. */
static uint32_t pool[POOL_WORDS];
static uint32_t at;
static uint32_t samples;

static uint32_t rotate_left(uint32_t v, uint32_t by) {
    by &= 31u;
    return by == 0 ? v : (v << by) | (v >> (32u - by));
}

void entropy_add_timing(uint32_t cycles) {
    uint32_t slot = at % POOL_WORDS;
    /* Rotating by the sample count spreads successive samples across the
     * word instead of piling them into the same low bits — which is where
     * the jitter is, and so where they would cancel. */
    pool[slot] ^= rotate_left(cycles, samples);
    pool[slot] += cycles;
    at++;
    samples += samples < 0xffffffffu ? 1u : 0u;
}

uint32_t entropy_samples(void) {
    return samples;
}

/* Reads the pool through SHA-256, in counter mode so that a longer request
 * is not the same block repeated — and so that reading it does not hand out
 * the pool itself. */
bool entropy_fill(void *buf, uint32_t len) {
    uint8_t *out = buf;
    uint32_t done = 0;
    for (uint32_t block = 0; done < len; block++) {
        uint8_t digest[SHA256_DIGEST];
        struct sha256 h;
        sha256_init(&h);
        sha256_update(&h, pool, sizeof pool);
        sha256_update(&h, &block, sizeof block);
        sha256_update(&h, &samples, sizeof samples);
        sha256_final(&h, digest);
        for (uint32_t i = 0; i < SHA256_DIGEST && done < len; i++) {
            out[done++] = digest[i];
        }
        /* Stir, so that two reads of an unchanged pool differ. */
        pool[block % POOL_WORDS] ^= ((uint32_t)digest[0] << 24) | ((uint32_t)digest[1] << 16) |
                                    ((uint32_t)digest[2] << 8) | digest[3];
    }
    return samples >= ENTROPY_SAMPLES_WANTED;
}
