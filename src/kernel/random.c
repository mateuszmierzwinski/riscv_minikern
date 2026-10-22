#include "kernel/random.h"

#include "kernel/entropy.h"

/* xoshiro128** (Blackman & Vigna, public domain algorithm), written here. */
static uint32_t s[4] = {0x9e3779b9u, 0x243f6a88u, 0xb7e15162u, 0x85a308d3u};

static uint32_t rotl(uint32_t x, int k) {
    return (x << k) | (x >> (32 - k));
}

static uint32_t next(void) {
    uint32_t result = rotl(s[1] * 5u, 7) * 9u;
    uint32_t t = s[1] << 9;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rotl(s[3], 11);
    return result;
}

void random_seed(const void *data, uint32_t len) {
    const uint8_t *d = data;
    for (uint32_t i = 0; i < len; i++) {
        s[i % 4u] ^= (uint32_t)d[i] << ((i % 4u) * 8u);
        (void)next();
    }
    for (int i = 0; i < 16; i++) {
        (void)next();
    }
}

void random_fill(void *buf, uint32_t len) {
    uint8_t *b = buf;
    /* Once the pool has had its fill of timing samples it **is** the
     * answer: the generator below is a good sequence from a seed that is
     * the same on every boot, which is fine for a hash seed and not for a
     * key (kernel/entropy.h). Before then, the pool is mixed into the
     * generator's state rather than used on its own. */
    if (entropy_fill(b, len)) {
        return;
    }
    random_seed(b, len < 16u ? len : 16u);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t r = next();
        for (uint32_t k = 0; k < 4 && i + k < len; k++) {
            b[i + k] = (uint8_t)(r >> (8 * k));
        }
    }
}
