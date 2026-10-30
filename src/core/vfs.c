#include "core/vfs.h"

#include <stddef.h>

#include "core/errno.h"

static struct vfs_mount table[VFS_MOUNTS];
static uint32_t count;

/* `at` without trailing slashes ("/" stays "/"), into out; its length, or
 * 0 when it is not absolute or does not fit. */
static uint32_t normalise(const char *at, char *out) {
    uint32_t n = 0;
    if (at == NULL || at[0] != '/') {
        return 0;
    }
    while (at[n] != '\0') {
        if (n + 1u >= VFS_AT_MAX) {
            return 0;
        }
        out[n] = at[n];
        n++;
    }
    while (n > 1u && out[n - 1u] == '/') {
        n--;
    }
    out[n] = '\0';
    return n;
}

static bool same(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

static int find_exact(const char *at) {
    for (uint32_t i = 0; i < count; i++) {
        if (same(table[i].at, at)) {
            return (int)i;
        }
    }
    return -1;
}

int vfs_mount(const char *at, const struct vfs_ops *ops, void *self) {
    char clean[VFS_AT_MAX];
    uint32_t n = normalise(at, clean);
    if (n == 0 || ops == NULL) {
        return KERR_INVAL;
    }
    if (find_exact(clean) >= 0) {
        return KERR_BUSY;
    }
    if (count == VFS_MOUNTS) {
        return KERR_NOMEM;
    }
    for (uint32_t i = 0; i <= n; i++) {
        table[count].at[i] = clean[i];
    }
    table[count].at_len = n;
    table[count].ops = ops;
    table[count].self = self;
    count++;
    return KERR_OK;
}

int vfs_umount(const char *at) {
    char clean[VFS_AT_MAX];
    int i = normalise(at, clean) == 0 ? -1 : find_exact(clean);
    if (i < 0) {
        return KERR_INVAL;
    }
    for (uint32_t j = (uint32_t)i; j + 1u < count; j++) {
        table[j] = table[j + 1u];
    }
    count--;
    return KERR_OK;
}

/* How much of `path` the mount covers: its length when path is the mount
 * point or below it ("/netx" is not below "/net"), else 0. "/" covers every
 * absolute path, with length 1. */
static uint32_t covers(const struct vfs_mount *m, const char *path) {
    uint32_t i = 0;
    if (m->at_len == 1u) {
        return 1u;
    }
    while (i < m->at_len && path[i] == m->at[i]) {
        i++;
    }
    return i == m->at_len && (path[i] == '\0' || path[i] == '/') ? i : 0u;
}

const struct vfs_mount *vfs_resolve(const char *path, const char **within) {
    const struct vfs_mount *best = NULL;
    uint32_t best_len = 0;
    if (path == NULL || path[0] != '/') {
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t len = covers(&table[i], path);
        if (len > best_len) {
            best = &table[i];
            best_len = len;
        }
    }
    if (best != NULL) {
        const char *rest = best->at_len == 1u ? path : path + best_len;
        *within = rest[0] == '\0' ? "/" : rest;
    }
    return best;
}

int vfs_resolve_pair(const char *from, const char *to, const struct vfs_mount **m,
                     const char **a, const char **b) {
    const struct vfs_mount *x = vfs_resolve(from, a);
    const struct vfs_mount *y = vfs_resolve(to, b);
    *m = x;
    if (x == NULL && y == NULL) {
        return KERR_NOENT;
    }
    return x == y ? KERR_OK : KERR_XDEV;
}

uint32_t vfs_count(void) {
    return count;
}

const struct vfs_mount *vfs_at(uint32_t i) {
    return i < count ? &table[i] : NULL;
}

void vfs_reset(void) {
    count = 0;
}
