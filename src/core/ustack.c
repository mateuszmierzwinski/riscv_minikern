#include "core/ustack.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"

/* A downward cursor over buf; addr() names a buf offset as a user address. */
struct ucursor {
    uint8_t *buf;
    uint32_t len;
    uint32_t top;
    uint32_t at;  /* offset of the lowest byte written so far */
    bool overflow;
};

static uint32_t uaddr(const struct ucursor *c) {
    return c->top - c->len + c->at;
}

/* Reserves n bytes below what is written; returns their user address. */
static uint32_t push_bytes(struct ucursor *c, const void *data, uint32_t n) {
    if (n > c->at) {
        c->overflow = true;
        return 0;
    }
    c->at -= n;
    for (uint32_t i = 0; data != NULL && i < n; i++) {
        c->buf[c->at + i] = ((const uint8_t *)data)[i];
    }
    return uaddr(c);
}

static uint32_t push_string(struct ucursor *c, const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return push_bytes(c, s, n + 1);
}

static void put_word(struct ucursor *c, uint32_t offset, uint32_t v) {
    c->buf[offset] = (uint8_t)v;
    c->buf[offset + 1] = (uint8_t)(v >> 8);
    c->buf[offset + 2] = (uint8_t)(v >> 16);
    c->buf[offset + 3] = (uint8_t)(v >> 24);
}

/* The vector's word count: argc + argv + NULL + envp + NULL + auxv + AT_NULL. */
static uint32_t vector_words(const struct ustack_args *a, uint32_t nauxv) {
    return 1u + a->argc + 1u + a->envc + 1u + 2u * (nauxv + 1u);
}

/* Pushes strings (argv, envp) and records their user addresses. */
static void push_strings(struct ucursor *c, const char *const *s, uint32_t n, uint32_t *addrs) {
    for (uint32_t i = n; i-- > 0;) {
        addrs[i] = push_string(c, s[i]);
    }
}

/* Writes the vector at the cursor (already reserved and aligned). */
static void write_vector(struct ucursor *c, const struct ustack_args *a, const uint32_t *argv_at,
                         const uint32_t *envp_at, const struct auxv_pair *aux, uint32_t naux) {
    uint32_t w = c->at;
    put_word(c, w, a->argc);
    w += 4;
    for (uint32_t i = 0; i < a->argc; i++, w += 4) {
        put_word(c, w, argv_at[i]);
    }
    put_word(c, w, 0);
    w += 4;
    for (uint32_t i = 0; i < a->envc; i++, w += 4) {
        put_word(c, w, envp_at[i]);
    }
    put_word(c, w, 0);
    w += 4;
    for (uint32_t i = 0; i < naux; i++, w += 8) {
        put_word(c, w, aux[i].type);
        put_word(c, w + 4, aux[i].value);
    }
    put_word(c, w, AT_NULL);
    put_word(c, w + 4, 0);
}

/* The caller's auxv plus the three entries that point into the stack. */
static uint32_t full_auxv(const struct ustack_args *a, struct auxv_pair *aux, uint32_t random, uint32_t platform,
                          uint32_t execfn) {
    uint32_t n = 0;
    for (; n < a->nauxv; n++) {
        aux[n] = a->auxv[n];
    }
    aux[n++] = (struct auxv_pair){AT_RANDOM, random};
    aux[n++] = (struct auxv_pair){AT_PLATFORM, platform};
    aux[n++] = (struct auxv_pair){AT_EXECFN, execfn};
    return n;
}

/* One top-down layout pass: strings and AT_RANDOM first, then the
 * vector below them, 16-byte aligned. */
int ustack_build(uint8_t *buf, uint32_t len, uint32_t top, const struct ustack_args *a, uint32_t *sp,
                 uint32_t *used) {
    struct ucursor c = {.buf = buf, .len = len, .top = top, .at = len};
    uint32_t argv_at[USTACK_MAX_ARGS], envp_at[USTACK_MAX_ARGS];
    struct auxv_pair aux[USTACK_MAX_AUXV];
    uint32_t execfn, random, platform, naux, words;
    if (a->nauxv + 3u > USTACK_MAX_AUXV || a->argc > USTACK_MAX_ARGS || a->envc > USTACK_MAX_ARGS ||
        len % 16u != 0 || top % 16u != 0) {
        return KERR_RANGE;
    }
    push_bytes(&c, NULL, 4);  /* a NULL word at the very top, as Linux leaves */
    execfn = push_string(&c, a->argc > 0 ? a->argv[0] : "");
    push_strings(&c, a->envp, a->envc, envp_at);
    push_strings(&c, a->argv, a->argc, argv_at);
    platform = push_string(&c, a->platform);
    random = push_bytes(&c, a->random, 16);
    naux = full_auxv(a, aux, random, platform, execfn);
    words = vector_words(a, naux);
    if (c.overflow || c.at < words * 4u) {
        return KERR_RANGE;
    }
    c.at = (c.at - words * 4u) & ~15u;
    write_vector(&c, a, argv_at, envp_at, aux, naux);
    *sp = uaddr(&c);
    *used = len - c.at;
    return KERR_OK;
}
