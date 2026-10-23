#include "core/cmdline.h"

static size_t word_length(const char *w) {
    size_t n = 0;
    while (w[n] != '\0' && w[n] != ' ') {
        n++;
    }
    return n;
}

/* If word (of length n) is key or key=..., returns the value's offset in it. */
static bool match_key(const char *word, size_t n, const char *key, size_t *value_at) {
    size_t k = 0;
    while (key[k] != '\0' && k < n && word[k] == key[k]) {
        k++;
    }
    if (key[k] != '\0' || k == 0) {
        return false;
    }
    if (k == n) {
        *value_at = n;
        return true;
    }
    *value_at = k + 1;
    return word[k] == '=';
}

const char *cmdline_value(const char *cmdline, const char *key, size_t *len) {
    for (const char *w = cmdline; w != NULL && *w != '\0';) {
        size_t n = word_length(w);
        size_t at = 0;
        if (n > 0 && match_key(w, n, key, &at)) {
            *len = n - at;
            return w + at;
        }
        w += n;
        while (*w == ' ') {
            w++;
        }
    }
    return NULL;
}

const char *cmdline_next(const char *cmdline, size_t *at, size_t *len) {
    const char *w;
    if (cmdline == NULL) {
        return NULL;
    }
    while (cmdline[*at] == ' ') {
        (*at)++;
    }
    if (cmdline[*at] == '\0') {
        return NULL;
    }
    w = cmdline + *at;
    *len = word_length(w);
    *at += *len;
    return w;
}

bool cmdline_equals(const char *cmdline, const char *key, const char *value) {
    size_t len = 0;
    const char *v = cmdline_value(cmdline, key, &len);
    size_t i = 0;
    if (v == NULL) {
        return false;
    }
    while (i < len && value[i] != '\0' && v[i] == value[i]) {
        i++;
    }
    return i == len && value[i] == '\0';
}

bool cmdline_has(const char *cmdline, const char *key) {
    size_t len = 0;
    return cmdline_value(cmdline, key, &len) != NULL;
}

/* One hexadecimal digit's value, or 16 if it is not one. */
static uint32_t hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return (uint32_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint32_t)(c - 'a') + 10u;
    }
    if (c >= 'A' && c <= 'F') {
        return (uint32_t)(c - 'A') + 10u;
    }
    return 16u;
}

static uint64_t parse_hex(const char *v, size_t len, uint64_t fallback) {
    uint64_t n = 0;
    if (len == 0) {
        return fallback; /* "0x" with nothing after it is not a number */
    }
    for (size_t i = 0; i < len; i++) {
        uint32_t d = hex_digit(v[i]);
        if (d >= 16u) {
            return fallback;
        }
        n = n * 16u + d;
    }
    return n;
}

/* A number from the command line, **decimal or `0x` hexadecimal**.
 *
 * The hex half was added because a value that is a *bitmask* is unreadable
 * in decimal, and the first use of one — `minikern.isolate=0x8`, to reserve
 * a hart — hit the failure this parser is shaped to produce: a non-digit
 * makes it return the fallback, so the option was silently ignored and the
 * hart it was meant to reserve went on taking work. Nothing reported
 * anything, because "the operator did not ask" and "the operator asked in a
 * notation I do not read" were the same answer.
 *
 * It still returns the fallback on anything malformed, which is the contract
 * the callers rely on; what changed is that `0x8` is no longer malformed. */
uint64_t cmdline_number_in(const char *v, size_t len, uint64_t fallback) {
    uint64_t n = 0;
    if (v == NULL || len == 0) {
        return fallback;
    }
    if (len > 2u && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        return parse_hex(v + 2, len - 2u, fallback);
    }
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return fallback;
        }
        n = n * 10u + (uint64_t)(v[i] - '0');
    }
    return n;
}

uint64_t cmdline_number(const char *cmdline, const char *key, uint64_t fallback) {
    size_t len = 0;
    const char *v = cmdline_value(cmdline, key, &len);
    uint64_t n = 0;
    if (v == NULL || len == 0) {
        return fallback;
    }
    if (len > 2u && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        return parse_hex(v + 2, len - 2u, fallback);
    }
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return fallback;
        }
        n = n * 10u + (uint64_t)(v[i] - '0');
    }
    return n;
}
