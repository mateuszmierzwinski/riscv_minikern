#include "core/tar.h"

#include <stdbool.h>

#include "core/errno.h"

/* Header field offsets and widths: POSIX.1-1988 ustar. */
#define H_NAME     0u
#define H_MODE     100u
#define H_SIZE     124u
#define H_CHKSUM   148u
#define H_TYPE     156u
#define H_LINK     157u
#define H_MAGIC    257u
#define H_PREFIX   345u
#define SIZE_MAX_OCTAL 0x7fffffffu  /* we refuse entries of 2 GiB and up */

/* Parses an octal field of width w (NUL/space terminated). */
static int parse_octal(const uint8_t *f, uint32_t w, uint32_t *out) {
    uint64_t v = 0;
    uint32_t i = 0;
    while (i < w && f[i] == ' ') {
        i++;
    }
    for (; i < w && f[i] >= '0' && f[i] <= '7'; i++) {
        v = v * 8u + (uint64_t)(f[i] - '0');
        if (v > SIZE_MAX_OCTAL) {
            return KERR_INVAL;
        }
    }
    if (i < w && f[i] != '\0' && f[i] != ' ') {
        return KERR_INVAL;
    }
    *out = (uint32_t)v;
    return KERR_OK;
}

/* The header checksum: the byte sum with the checksum field as spaces. */
static bool checksum_ok(const uint8_t *h) {
    uint32_t want, sum = 0;
    if (parse_octal(h + H_CHKSUM, 8, &want) != KERR_OK) {
        return false;
    }
    for (uint32_t i = 0; i < TAR_BLOCK; i++) {
        sum += (i >= H_CHKSUM && i < H_CHKSUM + 8u) ? (uint32_t)' ' : h[i];
    }
    return sum == want;
}

static bool all_zero(const uint8_t *h) {
    for (uint32_t i = 0; i < TAR_BLOCK; i++) {
        if (h[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Appends at most n bytes of src (stopping at NUL) to out at *at. */
static void append(char *out, uint32_t *at, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n && src[i] != '\0' && *at + 1u < TAR_NAME_MAX; i++) {
        out[(*at)++] = (char)src[i];
    }
    out[*at] = '\0';
}

/* True when the next component of path is exactly `name`. */
static bool component_is(const char *path, const char *name) {
    uint32_t i = 0;
    for (; name[i] != '\0'; i++) {
        if (path[i] != name[i]) {
            return false;
        }
    }
    return path[i] == '/' || path[i] == '\0';
}

/* Drops the last component already written to out. */
static uint32_t drop_last(char *out, uint32_t n) {
    while (n > 0 && out[n - 1] != '/') {
        n--;
    }
    return n > 0 ? n - 1u : 0; /* and the slash before it */
}

/* Normalises a path: no leading slashes, no "." components, and ".."
 * resolved against what came before it.
 *
 * Resolving ".." here rather than refusing it is what makes a relative path
 * usable — `../etc/motd` is an ordinary thing for a program to open — and
 * doing it **before** anything looks at the name is what stops it being a
 * way out of the root: ".." at the top has nothing to remove, so it folds
 * away rather than escaping. A path that is all ".." is the root itself. */
int tar_normalise(const char *path, char *out) {
    uint32_t n = 0;
    while (*path == '/') {
        path++;
    }
    while (*path != '\0') {
        if (component_is(path, ".")) {
            path += path[1] == '/' ? 2 : 1;
            continue;
        }
        if (component_is(path, "..")) {
            n = drop_last(out, n);
            path += path[2] == '/' ? 3 : 2;
            continue;
        }
        if (n > 0) {
            if (n + 1u >= TAR_NAME_MAX) {
                return KERR_NAMETOOLONG;
            }
            out[n++] = '/';
        }
        while (*path != '\0' && *path != '/') {
            if (n + 1u >= TAR_NAME_MAX) {
                return KERR_NAMETOOLONG;
            }
            out[n++] = *path++;
        }
        while (*path == '/') {
            path++;
        }
    }
    out[n] = '\0';
    return KERR_OK;
}

/* Builds the entry's normalised full name from prefix and name. */
static int header_name(const uint8_t *h, char *out) {
    char raw[TAR_NAME_MAX];
    uint32_t at = 0;
    raw[0] = '\0';
    if (h[H_PREFIX] != '\0') {
        append(raw, &at, h + H_PREFIX, 155);
        append(raw, &at, (const uint8_t *)"/", 1);
    }
    append(raw, &at, h + H_NAME, 100);
    return tar_normalise(raw, out);
}

/* Decodes one header (already bounds- and checksum-checked). */
static int decode(const uint8_t *h, struct tar_entry *out) {
    uint32_t at = 0;
    out->type = h[H_TYPE] == '\0' ? TAR_FILE : (char)h[H_TYPE];
    if (parse_octal(h + H_SIZE, 12, &out->size) != KERR_OK || parse_octal(h + H_MODE, 8, &out->mode) != KERR_OK) {
        return KERR_INVAL;
    }
    out->link[0] = '\0';
    for (; at < 100u && h[H_LINK + at] != '\0'; at++) {
        out->link[at] = (char)h[H_LINK + at];
    }
    out->link[at] = '\0';
    return header_name(h, out->name) == KERR_OK ? KERR_OK : KERR_INVAL;
}

/* Extension records describe the next entry; we read only plain headers. */
static bool is_extension(char type) {
    return type == 'x' || type == 'g' || type == 'L' || type == 'K';
}

/* Reads the header at *offset, whatever its type. */
static int read_one(const uint8_t *ar, size_t len, size_t *offset, struct tar_entry *out) {
    const uint8_t *h = ar + *offset;
    uint32_t blocks;
    if (len - *offset < TAR_BLOCK || all_zero(h)) {
        return KERR_NOENT;
    }
    if (!checksum_ok(h) || h[H_MAGIC] != 'u' || decode(h, out) != KERR_OK) {
        return KERR_INVAL;
    }
    blocks = (out->size + TAR_BLOCK - 1u) / TAR_BLOCK;
    if ((size_t)blocks * TAR_BLOCK > len - *offset - TAR_BLOCK) {
        return KERR_INVAL;
    }
    out->data = h + TAR_BLOCK;
    *offset += TAR_BLOCK + (size_t)blocks * TAR_BLOCK;
    return KERR_OK;
}

int tar_next(const uint8_t *ar, size_t len, size_t *offset, struct tar_entry *out) {
    int rc;
    if (ar == NULL || *offset > len) {
        return KERR_NOENT;
    }
    do {
        rc = read_one(ar, len, offset, out);
    } while (rc == KERR_OK && is_extension(out->type));
    return rc;
}

static bool str_eq(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int tar_lookup(const uint8_t *ar, size_t len, const char *path, struct tar_entry *out) {
    char want[TAR_NAME_MAX];
    size_t off = 0;
    int rc;
    if (tar_normalise(path, want) != KERR_OK) {
        return KERR_NAMETOOLONG;
    }
    while ((rc = tar_next(ar, len, &off, out)) == KERR_OK) {
        if (str_eq(out->name, want)) {
            return KERR_OK;
        }
    }
    return rc;
}
