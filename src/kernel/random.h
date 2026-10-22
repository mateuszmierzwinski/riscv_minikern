/* random — the kernel's random bytes, for getrandom(), AT_RANDOM and
 * /dev/urandom. xoshiro128** seeded at boot from the timer and the chip's
 * unique ID: unpredictable enough for hash seeds and ASLR-free stack
 * canaries, NOT a cryptographic source (no hardware TRNG is driven yet;
 * CLAUDE.md §10 records it). */
#ifndef MINIKERN_KERNEL_RANDOM_H
#define MINIKERN_KERNEL_RANDOM_H

#include <stdint.h>

/* Mixes seed material in; may be called more than once. */
void random_seed(const void *data, uint32_t len);

void random_fill(void *buf, uint32_t len);

#endif
