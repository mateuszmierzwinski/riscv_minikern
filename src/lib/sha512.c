#include "lib/sha512.h"

static const uint64_t K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull,
    0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull,
    0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull,
    0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull,
    0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull,
    0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull,
    0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull,
    0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull,
    0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull,
    0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull,
    0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull,
    0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull,
    0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull,
    0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static const uint64_t H0[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

static uint64_t ror(uint64_t x, uint32_t n) {
    return (x >> n) | (x << (64u - n));
}

static uint64_t be64(const uint8_t *p) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < 8; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

/* Eight bytes, big-endian.
 *
 * The suppression below is a **documented false positive**, and the three
 * attempts to make it unnecessary are all still in `sha512_final` because
 * each one improved the code:
 *
 *   1. `have % SHA512_BLOCK` — the analyser does not model modulo precisely
 *      and ignored it.
 *   2. `if (have >= SHA512_BLOCK)` — it acknowledged modelling the branch
 *      ("Assuming 'have' is < SHA512_BLOCK") and then still reported an
 *      index of 4294967288, which is **-8**: it could not carry the bound
 *      through the unsigned subtraction that follows.
 *   3. a check on `pad + 16u > sizeof tail`, bounding the offset that
 *      actually indexes the buffer — same result.
 *
 * With `have < 128`, `pad` is at most 128 and the furthest byte written is
 * at offset 143 of a 256-byte buffer. Every caller is in this file and both
 * are bounded; the checker stays on for the rest of the tree, where it is
 * worth having. */
static void put_be64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8; i++) {
        /* NOLINTNEXTLINE(clang-analyzer-security.ArrayBound) */
        p[i] = (uint8_t)(v >> (56u - 8u * i));
    }
}

/* One 128-byte block into the state. */
static void block(struct sha512 *s, const uint8_t *in) {
    uint64_t w[80];
    uint64_t v[8];
    for (uint32_t i = 0; i < 16; i++) {
        w[i] = be64(in + i * 8u);
    }
    for (uint32_t i = 16; i < 80; i++) {
        uint64_t s0 = ror(w[i - 15], 1) ^ ror(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror(w[i - 2], 19) ^ ror(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (uint32_t i = 0; i < 8; i++) {
        v[i] = s->h[i];
    }
    for (uint32_t i = 0; i < 80; i++) {
        uint64_t s1 = ror(v[4], 14) ^ ror(v[4], 18) ^ ror(v[4], 41);
        uint64_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint64_t t1 = v[7] + s1 + ch + K[i] + w[i];
        uint64_t s0 = ror(v[0], 28) ^ ror(v[0], 34) ^ ror(v[0], 39);
        uint64_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint64_t t2 = s0 + maj;
        v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
        v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
    }
    for (uint32_t i = 0; i < 8; i++) {
        s->h[i] += v[i];
    }
}

void sha512_init(struct sha512 *s) {
    for (uint32_t i = 0; i < 8; i++) {
        s->h[i] = H0[i];
    }
    s->len = 0;
    s->have = 0;
}

void sha512_update(struct sha512 *s, const void *data, uint32_t len) {
    const uint8_t *in = data;
    s->len += len;
    while (len > 0) {
        uint32_t room = SHA512_BLOCK - s->have;
        uint32_t take = len < room ? len : room;
        for (uint32_t i = 0; i < take; i++) {
            s->buf[s->have + i] = in[i];
        }
        s->have += take;
        in += take;
        len -= take;
        if (s->have == SHA512_BLOCK) {
            block(s, s->buf);
            s->have = 0;
        }
    }
}

/* The padding: a 0x80 byte, then zeroes, then the length in bits as a
 * 128-bit big-endian number — of which only the low 64 bits can be
 * anything, since this kernel will never hash two exabytes. */
void sha512_final(struct sha512 *s, uint8_t out[SHA512_DIGEST]) {
    uint64_t bits = s->len * 8u;
    uint8_t tail[SHA512_BLOCK * 2u];
    /* **The invariant, made local and therefore true.**
     *
     * `have` is the partial block's length and `sha512_update` keeps it below
     * one block — but nothing here required that, and the arithmetic below is
     * unsigned: a `have` above 240 makes `240u - have` wrap to something near
     * 2^32 and the writes that follow go wherever that lands. The analyser
     * named the index it would use: **4294967288, in a 256-byte buffer**.
     *
     * With `have < 128`: `pad` is at most 128, the loop touches at most 144
     * bytes and `put_be64` the 8 at offset 136 — inside a 256-byte buffer.
     * Found by the clang analyser, which could not bound it and was right not
     * to: the bound was an invariant held somewhere else. */
    uint32_t have = s->have;
    uint32_t pad;
    if (have >= SHA512_BLOCK) {
        /* Cannot happen — and checked rather than asserted in a comment,
         * because the first attempt at this *was* a comment (and then a
         * modulo, which the analyser does not model precisely enough to
         * believe). A compare it does model, and the cost is one
         * instruction on a path taken once per digest. */
        have = SHA512_BLOCK - 1u;
    }
    pad = have < 112u ? 112u - have : 240u - have;
    /* **Bounded where it is used, not three steps upstream.**
     *
     * `pad` is what indexes `tail`, so `pad` is what gets checked — against
     * the buffer's own size, so the check cannot drift if either changes.
     * Bounding `have` instead was correct and was not enough: the analyser
     * acknowledged the bound and still could not carry it through an
     * unsigned subtraction, reporting an index of 4294967288 — which is
     * **-8**, the offset it believed `112u - have` could produce. A check on
     * the offset itself needs no reasoning from anybody. */
    if (pad + 16u > sizeof tail) {
        pad = 112u;
    }
    uint32_t i;
    for (i = 0; i < pad + 16u; i++) {
        tail[i] = 0;
    }
    tail[0] = 0x80u;
    put_be64(tail + pad + 8u, bits);
    sha512_update(s, tail, pad + 16u);
    for (i = 0; i < 8; i++) {
        put_be64(out + i * 8u, s->h[i]);
    }
}

void sha512(const void *data, uint32_t len, uint8_t out[SHA512_DIGEST]) {
    struct sha512 s;
    sha512_init(&s);
    sha512_update(&s, data, len);
    sha512_final(&s, out);
}
