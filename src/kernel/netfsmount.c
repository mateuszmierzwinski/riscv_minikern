#include "kernel/netfsmount.h"

#include <stddef.h>

#include "core/errno.h"
#include "core/vfs.h"
#include "kernel/mem.h"

#define JOINED_MAX 512u

static int stat_op(void *nf, const char *within, uint32_t *mode, uint64_t *size, uint32_t *nlink) {
    struct netfs_stat st;
    int rc = netfs_stat(nf, within, &st);
    if (rc != KERR_OK) {
        return rc;
    }
    *mode = st.mode;
    *size = st.size;
    *nlink = st.nlink;
    return KERR_OK;
}

/* The export's files all belong to root: the server's owners are the
 * workstation's users, who mean nothing here. */
static int owner_op(void *nf, const char *within, uint32_t *uid, uint32_t *gid) {
    struct netfs_stat st;
    int rc = netfs_stat(nf, within, &st);
    if (rc == KERR_OK) {
        *uid = 0;
        *gid = 0;
    }
    return rc;
}

/* What a listing hands on, in XFS's entry shape. */
struct relay {
    xfs_dirent_fn fn;
    void *self;
    uint64_t next_ino;
};

static uint8_t type_of(uint32_t mode) {
    switch (mode & 0170000u) {
        case 0040000u: return XFS_DIR3_FT_DIR;
        case 0120000u: return XFS_DIR3_FT_SYMLINK;
        case 0100000u: return XFS_DIR3_FT_REG;
        default: return XFS_DIR3_FT_UNKNOWN;
    }
}

/* Each entry gets its own number: the export has none to give, and a
 * listing that gave every entry the same one reads as one file. */
static int relay_one(void *self, const char *name, uint32_t name_len, uint32_t mode) {
    struct relay *r = self;
    struct xfs_dirent d = {.ino = r->next_ino++, .name = name, .name_len = name_len,
                           .type = type_of(mode)};
    return r->fn(r->self, &d);
}

static int list_op(void *nf, const char *within, xfs_dirent_fn fn, void *self) {
    struct relay r = {.fn = fn, .self = self, .next_ino = 2};
    return netfs_list(nf, within, relay_one, &r);
}

static int read_op(void *nf, const char *within, uint32_t limit, uint8_t **out, uint32_t *size) {
    struct netfs_stat st;
    uint32_t got = 0;
    uint8_t *buf;
    int rc = netfs_stat(nf, within, &st);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((st.mode & 0170000u) == 0040000u || st.size > limit) {
        return (st.mode & 0170000u) == 0040000u ? KERR_ISDIR : KERR_FBIG;
    }
    buf = kzalloc(st.size == 0 ? 1u : (uint32_t)st.size);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    rc = netfs_read(nf, within, 0, buf, (uint32_t)st.size, &got);
    if (rc != KERR_OK || got != st.size) {
        kfree(buf);
        return rc != KERR_OK ? rc : KERR_IO; /* it changed under us */
    }
    *out = buf;
    *size = (uint32_t)st.size;
    return KERR_OK;
}

/* dir + "/" + leaf, refused when it does not fit. */
static int join(const char *dir, const char *leaf, char *out) {
    uint32_t n = 0;
    for (uint32_t i = 0; dir[i] != '\0'; i++) {
        if (n + 2u >= JOINED_MAX) {
            return KERR_NAMETOOLONG;
        }
        out[n++] = dir[i];
    }
    if (n == 0 || out[n - 1u] != '/') {
        out[n++] = '/';
    }
    for (uint32_t i = 0; leaf[i] != '\0'; i++) {
        if (n + 1u >= JOINED_MAX) {
            return KERR_NAMETOOLONG;
        }
        out[n++] = leaf[i];
    }
    out[n] = '\0';
    return KERR_OK;
}

static int write_op(void *nf, const char *dir, const char *leaf, uint64_t off, const void *bytes,
                    uint32_t len) {
    char *path = kzalloc(JOINED_MAX);
    uint32_t wrote = 0;
    int rc;
    if (path == NULL) {
        return KERR_NOMEM;
    }
    rc = join(dir, leaf, path);
    rc = rc != KERR_OK ? rc : netfs_write(nf, path, off, bytes, len, &wrote);
    kfree(path);
    return rc;
}

static int mkdir_op(void *nf, const char *within) {
    return netfs_mkdir(nf, within);
}

static int unlink_op(void *nf, const char *within) {
    return netfs_unlink(nf, within);
}

static int truncate_op(void *nf, const char *within, uint64_t size) {
    return netfs_truncate(nf, within, size);
}

static int rename_op(void *nf, const char *from, const char *to) {
    return netfs_rename(nf, from, to);
}

/* The export keeps a modification time only; it stands for all three. */
static int times_op(void *nf, const char *within, uint64_t *atime, uint64_t *mtime,
                    uint64_t *ctime) {
    struct netfs_stat st;
    int rc = netfs_stat(nf, within, &st);
    if (rc == KERR_OK) {
        *mtime = st.mtime > 0 ? (uint64_t)st.mtime : 0u;
        *atime = *mtime;
        *ctime = *mtime;
    }
    return rc;
}

static int read_at_op(void *nf, const char *within, uint64_t off, void *buf, uint32_t len,
                      uint32_t *got) {
    return netfs_read(nf, within, off, buf, len, got);
}

static int readlink_op(void *nf, const char *within, char *out, uint32_t max, uint32_t *got) {
    return netfs_readlink(nf, within, out, max, got);
}

static const struct vfs_ops netfs_ops = {
    .name = "netfs",
    .stat_full = stat_op,
    .owner = owner_op,
    .list = list_op,
    .read_file = read_op,
    .read_at = read_at_op,
    .write_at = write_op,
    .mkdir_p = mkdir_op,
    .unlink = unlink_op,
    .truncate = truncate_op,
    .rename = rename_op,
    .times = times_op,
    .readlink = readlink_op,
};

int netfsmount_attach(struct netfs *nf, const char *at) {
    return vfs_mount(at, &netfs_ops, nf);
}
