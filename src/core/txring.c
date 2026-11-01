#include "core/txring.h"

_Static_assert((TXRING_BYTES & (TXRING_BYTES - 1u)) == 0u, "the ring wraps by mask");

uint32_t txring_queued(const struct txring *r) {
    return r->head - r->tail;
}

bool txring_full(const struct txring *r) {
    return txring_queued(r) == TXRING_BYTES;
}

bool txring_push(struct txring *r, char c) {
    if (txring_full(r)) {
        return false;
    }
    r->bytes[r->head & (TXRING_BYTES - 1u)] = c;
    r->head++;
    return true;
}

bool txring_pop(struct txring *r, char *c) {
    if (txring_queued(r) == 0u) {
        return false;
    }
    *c = r->bytes[r->tail & (TXRING_BYTES - 1u)];
    r->tail++;
    return true;
}
