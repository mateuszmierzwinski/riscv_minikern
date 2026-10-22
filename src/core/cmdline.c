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

uint64_t cmdline_number(const char *cmdline, const char *key, uint64_t fallback) {
    size_t len = 0;
    const char *v = cmdline_value(cmdline, key, &len);
    uint64_t n = 0;
    if (v == NULL || len == 0) {
        return fallback;
    }
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return fallback;
        }
        n = n * 10u + (uint64_t)(v[i] - '0');
    }
    return n;
}
