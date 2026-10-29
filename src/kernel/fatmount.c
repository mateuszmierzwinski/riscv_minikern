/* A FAT volume behind the kernel's path-based filesystem calls (fatmount.h). */
#include "kernel/fatmount.h"

#include "core/errno.h"
#include "kernel/mem.h"

#define FAT_DIR_MODE  0040755u
#define FAT_FILE_MODE 0100644u

static uint32_t length(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int lookup(struct fat *fs, const char *within, struct fat_node *out) {
    return fat_path_lookup(fs, within[0] == '\0' ? "/" : within, out);
}

/* The directory holding the last component of `path`, and that component.
 * `path` is volume-relative; the parent is looked up from its own prefix. */
static int split(struct fat *fs, const char *path, struct fat_node *parent, const char **leaf,
                 uint32_t *leaf_len) {
    static char dir[256];
    uint32_t n = length(path), slash = 0;
    while (n > 1u && path[n - 1u] == '/') {
        n--;
    }
    for (uint32_t i = 0; i < n; i++) {
        slash = path[i] == '/' ? i : slash;
    }
    if (slash >= sizeof dir) {
        return KERR_NAMETOOLONG;
    }
    for (uint32_t i = 0; i < slash; i++) {
        dir[i] = path[i];
    }
    dir[slash] = '\0';
    *leaf = path + slash + (path[slash] == '/' ? 1u : 0u);
    *leaf_len = n - (uint32_t)(*leaf - path);
    return *leaf_len == 0 ? KERR_INVAL : lookup(fs, dir, parent);
}

int fatmount_stat_full(struct fat *fs, const char *within, uint32_t *mode, uint64_t *size,
                       uint32_t *nlink) {
    struct fat_node n;
    int rc = lookup(fs, within, &n);
    if (rc != KERR_OK) {
        return rc;
    }
    *mode = n.dir ? FAT_DIR_MODE : FAT_FILE_MODE;
    *size = n.size;
    *nlink = n.dir ? 2u : 1u;
    return KERR_OK;
}

/* What a listing hands on: the caller's callback, in XFS's entry shape. */
struct relay {
    xfs_dirent_fn fn;
    void *self;
};

/* An entry's number: where its directory entry is, which no two entries
 * share — FAT has no inode numbers, and a listing that gave every entry the
 * same one would make a program take them all for one file. */
static int relay_one(void *self, const struct fat_dirent *e) {
    const struct relay *r = self;
    struct xfs_dirent d = {.ino = e->node.entry == 0 ? 1u : e->node.entry / 32u,
                           .name = e->name, .name_len = length(e->name),
                           .type = e->node.dir ? XFS_DIR3_FT_DIR : XFS_DIR3_FT_REG};
    return r->fn(r->self, &d);
}

int fatmount_list(struct fat *fs, const char *within, xfs_dirent_fn fn, void *self) {
    struct fat_node dir;
    struct relay r = {fn, self};
    int rc = lookup(fs, within, &dir);
    return rc != KERR_OK ? rc : (dir.dir ? fat_list(fs, &dir, relay_one, &r) : KERR_NOTDIR);
}

int fatmount_read_file(struct fat *fs, const char *within, uint32_t limit, uint8_t **out,
                       uint32_t *size) {
    struct fat_node n;
    uint32_t got = 0;
    uint8_t *buf;
    int rc = lookup(fs, within, &n);
    if (rc != KERR_OK || n.dir || n.size > limit) {
        return rc != KERR_OK ? rc : (n.dir ? KERR_ISDIR : KERR_FBIG);
    }
    buf = kzalloc(n.size == 0 ? 1u : n.size);
    if (buf == NULL) {
        return KERR_NOMEM;
    }
    rc = fat_read(fs, &n, 0, buf, n.size, &got);
    if (rc != KERR_OK || got != n.size) {
        kfree(buf);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    *out = buf;
    *size = n.size;
    return KERR_OK;
}

int fatmount_write_at(struct fat *fs, const char *dir, const char *leaf, uint64_t off,
                      const void *bytes, uint32_t len, uint64_t now_s) {
    struct fat_node parent, file;
    uint32_t put = 0;
    int rc = lookup(fs, dir, &parent);
    if (rc != KERR_OK) {
        return rc;
    }
    fs->now_s = now_s;
    rc = fat_lookup(fs, &parent, leaf, length(leaf), &file);
    if (rc == KERR_NOENT) {
        rc = fat_create(fs, &parent, leaf, length(leaf), false, now_s, &file);
    }
    rc = rc != KERR_OK || file.dir ? (rc != KERR_OK ? rc : KERR_ISDIR) : KERR_OK;
    rc = rc != KERR_OK || len == 0 ? rc : fat_write(fs, &file, off, bytes, len, &put);
    return rc != KERR_OK ? rc : fat_flush(fs);
}

int fatmount_mkdir_p(struct fat *fs, const char *within, uint64_t now_s) {
    struct fat_node at;
    uint32_t i = 0;
    int rc = fat_root(fs, &at);
    while (rc == KERR_OK && within[i] != '\0') {
        uint32_t start, len = 0;
        struct fat_node next;
        while (within[i] == '/') {
            i++;
        }
        for (start = i; within[i] != '\0' && within[i] != '/'; i++) {
            len++;
        }
        if (len == 0) {
            break;
        }
        rc = fat_lookup(fs, &at, within + start, len, &next);
        if (rc == KERR_NOENT) {
            rc = fat_create(fs, &at, within + start, len, true, now_s, &next);
        }
        rc = rc == KERR_OK && !next.dir ? KERR_NOTDIR : rc;
        at = next;
    }
    return rc != KERR_OK ? rc : fat_flush(fs);
}

int fatmount_unlink_path(struct fat *fs, const char *within) {
    struct fat_node parent;
    const char *leaf = NULL;
    uint32_t len = 0;
    int rc = split(fs, within, &parent, &leaf, &len);
    rc = rc != KERR_OK ? rc : fat_unlink(fs, &parent, leaf, len);
    return rc != KERR_OK ? rc : fat_flush(fs);
}

int fatmount_rename_path(struct fat *fs, const char *from, const char *to, uint64_t now_s) {
    static struct fat_node from_dir, to_dir; /* static: one rename at a time, and two nodes */
    const char *a = NULL, *b = NULL;
    uint32_t alen = 0, blen = 0;
    int rc = split(fs, from, &from_dir, &a, &alen);
    rc = rc != KERR_OK ? rc : split(fs, to, &to_dir, &b, &blen);
    rc = rc != KERR_OK ? rc : fat_rename(fs, &from_dir, a, alen, &to_dir, b, blen, now_s);
    return rc != KERR_OK ? rc : fat_flush(fs);
}

int fatmount_truncate_path(struct fat *fs, const char *within, uint64_t size, uint64_t now_s) {
    struct fat_node n;
    int rc = size > 0xffffffffu ? KERR_FBIG : lookup(fs, within, &n);
    rc = rc != KERR_OK ? rc : fat_truncate(fs, &n, (uint32_t)size, now_s);
    return rc != KERR_OK ? rc : fat_flush(fs);
}

int fatmount_counters(struct fat *fs, struct xfs_counters *out) {
    *out = (struct xfs_counters){.blocks = fs->clusters, .free_blocks = fs->free_clusters,
                                 .inodes = 0, .free_inodes = 0, .blocksize = fs->cluster};
    return KERR_OK;
}
