#include "lib/sha256.h"

/* The round constants: the first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes (FIPS 180-4 §4.2.2). */
static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static uint32_t big_sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

static uint32_t big_sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

static uint32_t small_sigma0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

static uint32_t small_sigma1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

void sha256_init(struct sha256 *s) {
    static const uint32_t iv[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    *s = (struct sha256){0};
    for (uint32_t i = 0; i < 8; i++) {
        s->h[i] = iv[i];
    }
}

/* The message schedule of one block, big-endian words then expansion. */
static void schedule(const uint8_t *p, uint32_t w[64]) {
    for (uint32_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) |
               p[i * 4 + 3];
    }
    for (uint32_t i = 16; i < 64; i++) {
        w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
    }
}

static void compress(struct sha256 *s, const uint8_t *p) {
    uint32_t w[64], v[8];
    schedule(p, w);
    for (uint32_t i = 0; i < 8; i++) {
        v[i] = s->h[i];
    }
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t t1 = v[7] + big_sigma1(v[4]) + ch(v[4], v[5], v[6]) + K[i] + w[i];
        uint32_t t2 = big_sigma0(v[0]) + maj(v[0], v[1], v[2]);
        v[7] = v[6], v[6] = v[5], v[5] = v[4], v[4] = v[3] + t1;
        v[3] = v[2], v[2] = v[1], v[1] = v[0], v[0] = t1 + t2;
    }
    for (uint32_t i = 0; i < 8; i++) {
        s->h[i] += v[i];
    }
}

void sha256_update(struct sha256 *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->len += len;
    while (len > 0) {
        uint32_t room = SHA256_BLOCK - s->buffered;
        uint32_t n = len < room ? (uint32_t)len : room;
        if (s->buffered == 0 && len >= SHA256_BLOCK) {
            compress(s, p);
            p += SHA256_BLOCK;
            len -= SHA256_BLOCK;
            continue;
        }
        for (uint32_t i = 0; i < n; i++) {
            s->buf[s->buffered + i] = p[i];
        }
        s->buffered += n;
        p += n;
        len -= n;
        if (s->buffered == SHA256_BLOCK) {
            compress(s, s->buf);
            s->buffered = 0;
        }
    }
}

void sha256_final(struct sha256 *s, uint8_t out[SHA256_DIGEST]) {
    uint64_t bits = s->len * 8u;
    uint8_t pad[SHA256_BLOCK + 8] = {0x80};
    uint32_t zeros = (s->buffered < 56u ? 56u : 120u) - s->buffered - 1u;
    for (uint32_t i = 0; i < 8; i++) {
        pad[1 + zeros + i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    sha256_update(s, pad, 1u + zeros + 8u);
    for (uint32_t i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST]) {
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, data, len);
    sha256_final(&s, out);
}

int sha256_equal(const uint8_t a[SHA256_DIGEST], const uint8_t b[SHA256_DIGEST]) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < SHA256_DIGEST; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

void sha256_format(const uint8_t d[SHA256_DIGEST], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < SHA256_DIGEST; i++) {
        out[i * 2] = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 0xfu];
    }
    out[64] = '\0';
}
