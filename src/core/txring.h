#ifndef MINIKERN_CORE_TXRING_H
#define MINIKERN_CORE_TXRING_H

#include <stdbool.h>
#include <stdint.h>

/* **Bytes waiting for a slow transmitter**, so that whoever produced them
 * does not wait with it. A program's console `write` used to be sent at
 * 115200 baud with the kernel lock held — ~87 µs a byte, 8 ms for one line,
 * the longest hold of a Go run — and every other hart waited behind it.
 *
 * Indices run free and wrap by mask, so `head - tail` is the count with no
 * full/empty ambiguity. Not locked: the caller holds whatever protects it. */
#define TXRING_BYTES 16384u /* 1.4 s of output at 115200 baud */

struct txring {
    uint32_t head, tail;
    char bytes[TXRING_BYTES];
};

uint32_t txring_queued(const struct txring *r);
bool txring_full(const struct txring *r);

/* Appends one byte; false, and nothing changes, when the ring is full. */
bool txring_push(struct txring *r, char c);

/* Takes the oldest byte; false when there is none. */
bool txring_pop(struct txring *r, char *c);

#endif
