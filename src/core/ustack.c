#include "core/ustack.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"

/* Everything on this stack is a pointer-sized word. */
#define WORD 8u

/* What is being filled: the buffer, where it will live, and a cursor that
 * descends from the top. Strings are placed first, from the very top, and
 * the word-aligned structure below them — so the two never collide and the
 * arithmetic is one subtraction rather than two layouts. */
struct builder {
    uint8_t *buf;
    uint32_t len;
    virt_t top;
    uint32_t at;   /* bytes used from the top, downwards */
    bool full;
};

/* Reserves n bytes at the current cursor and returns where they are, or
 * NULL when the buffer is full — which is sticky: once a reservation has
 * failed, every later one does too, so a caller can check once at the end
 * instead of after every step. */
static uint8_t *take(struct builder *b, uint32_t n) {
    if (b->full || n > b->len - b->at) {
        b->full = true;
        return NULL;
    }
    b->at += n;
    return b->buf + b->len - b->at;
}

/* The address a reserved block will have once the buffer is mapped. */
static virt_t address_of(const struct builder *b, const uint8_t *p) {
    return b->top - (virt_t)(b->buf + b->len - p);
}

/* Copies a string in and returns the address it will have. */
static virt_t put_string(struct builder *b, const char *s) {
    uint32_t n = 0;
    uint8_t *at;
    while (s[n] != '\0') {
        n++;
    }
    at = take(b, n + 1u);
    if (at == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        at[i] = (uint8_t)s[i];
    }
    at[n] = '\0';
    return address_of(b, at);
}

static virt_t put_bytes(struct builder *b, const uint8_t *data, uint32_t n) {
    uint8_t *at = take(b, n);
    if (at == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < n; i++) {
        at[i] = data[i];
    }
    return address_of(b, at);
}

/* Writes one word of the structure. The structure is built from the bottom
 * up in a block reserved all at once, because its size is known from the
 * counts and a word written past the end is a corrupted auxiliary vector
 * rather than a visible failure. */
static void put_word(uint8_t *block, uint32_t index, uint64_t v) {
    for (uint32_t i = 0; i < WORD; i++) {
        block[index * WORD + i] = (uint8_t)(v >> (8u * i));
    }
}

int ustack_build(uint8_t *buf, uint32_t len, virt_t top, const struct ustack_args *a, virt_t *sp,
                 uint32_t *used) {
    struct builder b = {.buf = buf, .len = len, .top = top};
    virt_t argv_addr[32], envp_addr[32], random_addr, execfn_addr = 0;
    uint32_t words, index = 0;
    uint8_t *block;
    if (a->argc > 32u || a->envc > 32u || a->nauxv + 3u > USTACK_MAX_AUXV) {
        return KERR_INVAL;
    }
    /* The strings first, at the very top, so their addresses are known
     * before the structure that points at them is written. */
    for (uint32_t i = 0; i < a->argc; i++) {
        argv_addr[i] = put_string(&b, a->argv[i]);
    }
    for (uint32_t i = 0; i < a->envc; i++) {
        envp_addr[i] = put_string(&b, a->envp[i]);
    }
    random_addr = put_bytes(&b, a->random, 16u);
    if (a->execfn != NULL) {
        execfn_addr = put_string(&b, a->execfn);
    }
    /* argc, argv + NULL, envp + NULL, the given auxv pairs, AT_RANDOM,
     * AT_EXECFN when there is one, and AT_NULL — two words each. */
    words = 1u + a->argc + 1u + a->envc + 1u + (a->nauxv + 2u + (execfn_addr != 0 ? 1u : 0u)) * 2u;
    /* The stack pointer must be 16-byte aligned when a program starts: the
     * ABI requires it, and a runtime that spills a 128-bit value relative to
     * it will fault if it is not. */
    b.at = (b.at + 15u) & ~15u;
    block = take(&b, words * WORD);
    if (block == NULL) {
        return KERR_NOMEM;
    }
    put_word(block, index++, a->argc);
    for (uint32_t i = 0; i < a->argc; i++) {
        put_word(block, index++, argv_addr[i]);
    }
    put_word(block, index++, 0);
    for (uint32_t i = 0; i < a->envc; i++) {
        put_word(block, index++, envp_addr[i]);
    }
    put_word(block, index++, 0);
    for (uint32_t i = 0; i < a->nauxv; i++) {
        put_word(block, index++, a->auxv[i].tag);
        put_word(block, index++, a->auxv[i].value);
    }
    put_word(block, index++, AT_RANDOM);
    put_word(block, index++, random_addr);
    if (execfn_addr != 0) {
        put_word(block, index++, AT_EXECFN);
        put_word(block, index++, execfn_addr);
    }
    put_word(block, index++, AT_NULL);
    put_word(block, index++, 0);
    *sp = address_of(&b, block);
    *used = b.at;
    return (*sp % 16u) == 0 ? KERR_OK : KERR_INVAL;
}
