#include "core/overlay.h"

#include <stddef.h>

#include "core/errno.h"

static uint32_t length_of(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static bool starts_with(const char *s, uint32_t len, const char *prefix) {
    uint32_t i = 0;
    while (prefix[i] != '\0') {
        if (i >= len || s[i] != prefix[i]) {
            return false;
        }
        i++;
    }
    return true;
}

/* Copies an option's value (s, len) into out, which must still be empty. */
static int take_value(const char *s, uint32_t len, char out[OVERLAY_PATH_MAX]) {
    if (out[0] != '\0' || len == 0 || len >= OVERLAY_PATH_MAX || s[0] != '/') {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < len; i++) {
        out[i] = s[i];
    }
    out[len] = '\0';
    return KERR_OK;
}

/* One "key=value" option. */
static int take_option(const char *s, uint32_t len, char *lower, char *upper, bool *work) {
    if (starts_with(s, len, "lowerdir=")) {
        return take_value(s + 9, len - 9, lower);
    }
    if (starts_with(s, len, "upperdir=")) {
        return take_value(s + 9, len - 9, upper);
    }
    if (starts_with(s, len, "workdir=") && !*work) {
        *work = true;
        return KERR_OK;
    }
    return KERR_INVAL;
}

int overlay_parse_options(const char *data, char lower[OVERLAY_PATH_MAX],
                          char upper[OVERLAY_PATH_MAX]) {
    uint32_t at = 0;
    bool work = false;
    lower[0] = '\0';
    upper[0] = '\0';
    while (data[at] != '\0') {
        uint32_t end = at;
        while (data[end] != '\0' && data[end] != ',') {
            end++;
        }
        int rc = take_option(data + at, end - at, lower, upper, &work);
        if (rc != KERR_OK) {
            return rc;
        }
        at = data[end] == ',' ? end + 1u : end;
    }
    return lower[0] != '\0' && upper[0] != '\0' ? KERR_OK : KERR_INVAL;
}

/* Appends n bytes of s at out[*at], refusing to reach the end. */
static int append(char out[OVERLAY_PATH_MAX], uint32_t *at, const char *s, uint32_t n) {
    if (*at + n >= OVERLAY_PATH_MAX) {
        return KERR_NAMETOOLONG;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[(*at)++] = s[i];
    }
    out[*at] = '\0';
    return KERR_OK;
}

int overlay_join(const char *base, const char *in, char out[OVERLAY_PATH_MAX]) {
    uint32_t b = length_of(base), n = length_of(in), at = 0;
    bool whole = n == 0 || (n == 1u && in[0] == '/');
    int rc;
    while (b > 1u && base[b - 1u] == '/') {
        b--;
    }
    if (whole) {
        return append(out, &at, base, b);
    }
    rc = b == 1u && base[0] == '/' ? KERR_OK : append(out, &at, base, b);
    return rc != KERR_OK ? rc : append(out, &at, in, n);
}

int overlay_whiteout_of(const char *in, char out[OVERLAY_PATH_MAX]) {
    uint32_t n = length_of(in), slash = 0, at = 0;
    const char *prefix = OVERLAY_WHITEOUT_PREFIX;
    for (uint32_t i = 0; i < n; i++) {
        slash = in[i] == '/' ? i : slash;
    }
    if (n < 2u || in[0] != '/' || slash + 1u >= n || n + 4u >= OVERLAY_PATH_MAX) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i <= slash; i++) {
        out[at++] = in[i];
    }
    for (uint32_t i = 0; prefix[i] != '\0'; i++) {
        out[at++] = prefix[i];
    }
    for (uint32_t i = slash + 1u; i < n; i++) {
        out[at++] = in[i];
    }
    out[at] = '\0';
    return KERR_OK;
}

bool overlay_is_whiteout(const char *name, uint32_t len) {
    return len > 4u && starts_with(name, len, OVERLAY_WHITEOUT_PREFIX);
}

bool overlay_path_under(const char *inner, const char *outer) {
    uint32_t o = length_of(outer);
    while (o > 1u && outer[o - 1u] == '/') {
        o--;
    }
    if (o == 1u && outer[0] == '/') {
        return true;
    }
    for (uint32_t i = 0; i < o; i++) {
        if (inner[i] != outer[i]) {
            return false;
        }
    }
    return inner[o] == '\0' || inner[o] == '/';
}
