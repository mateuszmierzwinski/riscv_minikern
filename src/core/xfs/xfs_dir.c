#include <stddef.h>

#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

/* Directories, in all three shapes XFS uses.
 *
 * Only the *data* is read. A big directory also carries hash structures
 * above 32 GiB of logical offset, which make a lookup O(1); walking the
 * data blocks instead makes it O(entries), and for a board reading tens or
 * hundreds of names that is the right trade for not implementing (and
 * trusting) three more btree formats. */

/* Rounds an entry's length up to the format's eight-byte grain. */
static uint32_t align8(uint32_t n) {
    return (n + XFS_DIR2_DATA_ALIGN - 1u) & ~(XFS_DIR2_DATA_ALIGN - 1u);
}

/* The space one entry occupies: the inode, the name and its length, the
 * type byte when the filesystem has them, and the trailing tag. */
static uint32_t entry_bytes(const struct xfs *fs, uint32_t name_len) {
    return align8(8u + 1u + name_len + (fs->ftype ? 1u : 0u) + 2u);
}

/* Shortform: the directory's whole content sits inside its inode. "." and
 * ".." are not stored — the parent is in the header and the directory's own
 * number is the inode's — so both are synthesized here, which is what a
 * reader of this listing expects to see. */
static int list_shortform(struct xfs *fs, const struct xfs_inode *dir, xfs_dirent_fn fn, void *self) {
    const uint8_t *p = dir->fork;
    uint32_t count = p[XFS_DIR_SF_COUNT];
    uint32_t inolen = p[XFS_DIR_SF_I8COUNT] != 0 ? 8u : 4u;
    uint32_t at = XFS_DIR_SF_PARENT + inolen;
    struct xfs_dirent e = {.ino = dir->ino, .name = ".", .name_len = 1u, .type = XFS_DIR3_FT_DIR};
    int rc;
    if (dir->size < at) {
        return KERR_IO;
    }
    rc = fn(self, &e);
    if (rc == KERR_OK) {
        e.name = "..";
        e.name_len = 2u;
        e.ino = inolen == 8u ? xfs_be64(p + XFS_DIR_SF_PARENT) : xfs_be32(p + XFS_DIR_SF_PARENT);
        rc = fn(self, &e);
    }
    for (uint32_t i = 0; i < count && rc == KERR_OK; i++) {
        uint32_t name_len = at < dir->size ? p[at] : 0u;
        uint32_t need = 1u + 2u + name_len + (fs->ftype ? 1u : 0u) + inolen;
        if (name_len == 0 || at + need > dir->size) {
            return KERR_IO; /* the entries run past what the inode holds */
        }
        e.name = (const char *)p + at + 3u;
        e.name_len = name_len;
        e.type = fs->ftype ? p[at + 3u + name_len] : XFS_DIR3_FT_UNKNOWN;
        e.ino = inolen == 8u ? xfs_be64(p + at + need - 8u) : xfs_be32(p + at + need - 4u);
        rc = fn(self, &e);
        at += need;
    }
    return rc;
}

/* Where the entries stop in one directory block. A single-block directory
 * keeps hash entries and a tail at the end of the same block, and those
 * must not be parsed as entries; a data block of a larger directory has
 * nothing after them. */
static uint32_t data_end(const struct xfs *fs, const uint8_t *block) {
    uint32_t count;
    if (xfs_be32(block + XFS_DIR3_HDR_MAGIC) != XFS_DIR3_BLOCK_MAGIC) {
        return fs->blocksize;
    }
    count = xfs_be32(block + fs->blocksize - XFS_DIR2_BLOCK_TAIL_SIZE);
    if (count > (fs->blocksize - XFS_DIR3_DATA_HDR_SIZE) / XFS_DIR2_LEAF_ENTRY_SIZE) {
        return XFS_DIR3_DATA_HDR_SIZE; /* a tail that cannot be true: read nothing */
    }
    return fs->blocksize - XFS_DIR2_BLOCK_TAIL_SIZE - count * XFS_DIR2_LEAF_ENTRY_SIZE;
}

/* Walks the entries of one directory block. */
static int list_block(struct xfs *fs, const uint8_t *block, xfs_dirent_fn fn, void *self) {
    uint32_t end = data_end(fs, block);
    uint32_t at = XFS_DIR3_DATA_HDR_SIZE;
    while (at + 8u <= end) {
        struct xfs_dirent e;
        uint32_t name_len, len;
        if (xfs_be16(block + at) == XFS_DIR2_DATA_FREE_TAG) {
            len = xfs_be16(block + at + 2u);
            if (len < XFS_DIR2_DATA_ALIGN || at + len > end) {
                return KERR_IO;
            }
            at += len;
            continue;
        }
        name_len = block[at + 8u];
        len = entry_bytes(fs, name_len);
        if (name_len == 0 || at + len > end) {
            return KERR_IO;
        }
        e.ino = xfs_be64(block + at);
        e.name = (const char *)block + at + 9u;
        e.name_len = name_len;
        e.type = fs->ftype ? block[at + 9u + name_len] : XFS_DIR3_FT_UNKNOWN;
        int rc = fn(self, &e);
        if (rc != KERR_OK) {
            return rc;
        }
        at += len;
    }
    return KERR_OK;
}

/* A directory block must also say it belongs to this directory: a block
 * that names another owner is a block we were sent to by mistake. */
static int block_owner_ok(const struct xfs *fs, const uint8_t *block, uint64_t ino) {
    (void)fs;
    return xfs_be64(block + XFS_DIR3_HDR_OWNER) == ino ? KERR_OK : KERR_IO;
}

/* Walks every data block of a directory held in extents. */
/* Walks a directory's data blocks by file offset.
 *
 * Named for what it does rather than for the fork it came from: it asks
 * `xfs_bmap` where each block is, so it reads a directory whose extents are
 * in the inode and one whose extents are in a b-tree without knowing the
 * difference. It was called `list_extents` while the b-tree case was
 * refused, which made the name a statement about the format. */
static int list_data_blocks(struct xfs *fs, const struct xfs_inode *dir, xfs_dirent_fn fn,
                            void *self) {
    uint64_t blocks = (dir->size + fs->blocksize - 1u) / fs->blocksize;
    int rc = KERR_OK;
    for (uint64_t i = 0; i < blocks && rc == KERR_OK; i++) {
        struct xfs_extent e;
        struct bbuf *b;
        if (i >= fs->dir_leaf_block) {
            break; /* the hash structures live above this: never data */
        }
        rc = xfs_bmap(fs, dir, i, &e);
        if (rc == KERR_NOENT) {
            rc = KERR_OK; /* a hole in a directory: nothing to list */
            continue;
        }
        if (rc != KERR_OK) {
            return rc;
        }
        rc = xfs_meta_block(fs, e.startblock + (i - e.startoff), XFS_DIR3_BLOCK_MAGIC, XFS_DIR3_DATA_MAGIC,
                            XFS_DIR3_HDR_CRC, XFS_DIR3_HDR_UUID, &b);
        if (rc != KERR_OK) {
            return rc;
        }
        rc = block_owner_ok(fs, b->data, dir->ino);
        if (rc == KERR_OK) {
            rc = list_block(fs, b->data, fn, self);
        }
        bcache_put(fs->cache, b);
    }
    return rc;
}

int xfs_dir_list(struct xfs *fs, const struct xfs_inode *dir, xfs_dirent_fn fn, void *self) {
    if (!xfs_is_dir(dir)) {
        return KERR_NOTDIR;
    }
    if (dir->format == XFS_DINODE_FMT_LOCAL) {
        return list_shortform(fs, dir, fn, self);
    }
    if (dir->format == XFS_DINODE_FMT_EXTENTS || dir->format == XFS_DINODE_FMT_BTREE) {
        return list_data_blocks(fs, dir, fn, self);
    }
    return KERR_NOTSUP;
}

/* What a lookup carries through the walk: the name wanted, and the inode
 * found. KERR_EXIST is the signal that stops the listing early — it is not
 * an error, just the only way to say "stop, I have it". */
struct lookup {
    const char *name;
    uint32_t len;
    uint64_t ino;
};

static bool name_equal(const struct xfs_dirent *e, const char *name, uint32_t len) {
    if (e->name_len != len) {
        return false;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (e->name[i] != name[i]) {
            return false;
        }
    }
    return true;
}

static int lookup_visit(void *self, const struct xfs_dirent *e) {
    struct lookup *l = self;
    if (!name_equal(e, l->name, l->len)) {
        return KERR_OK;
    }
    l->ino = e->ino;
    return KERR_EXIST;
}

int xfs_dir_lookup(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t len,
                   uint64_t *ino) {
    struct lookup l = {.name = name, .len = len};
    int rc;
    if (len == 0 || len > XFS_NAME_MAX) {
        return KERR_INVAL;
    }
    rc = xfs_dir_list(fs, dir, lookup_visit, &l);
    if (rc == KERR_EXIST) {
        *ino = l.ino;
        return KERR_OK;
    }
    return rc == KERR_OK ? KERR_NOENT : rc;
}

/* The next component of a path, or 0 at its end. */
static uint32_t next_component(const char *path, uint32_t at, uint32_t *start) {
    uint32_t len = 0;
    while (path[at] == '/') {
        at++;
    }
    *start = at;
    while (path[at + len] != '\0' && path[at + len] != '/') {
        len++;
    }
    return len;
}

int xfs_path_lookup(struct xfs *fs, const char *path, struct xfs_inode *out) {
    uint32_t at = 0;
    int rc;
    if (path == NULL || path[0] != '/') {
        return KERR_INVAL;
    }
    rc = xfs_inode_read(fs, fs->rootino, out);
    for (uint32_t depth = 0; rc == KERR_OK; depth++) {
        uint32_t start, len = next_component(path, at, &start);
        uint64_t ino;
        if (len == 0) {
            return KERR_OK; /* the path ended: out is what it named */
        }
        if (depth == XFS_PATH_DEPTH) {
            return KERR_NAMETOOLONG;
        }
        if (!xfs_is_dir(out)) {
            return KERR_NOTDIR;
        }
        rc = xfs_dir_lookup(fs, out, path + start, len, &ino);
        if (rc == KERR_OK) {
            rc = xfs_inode_read(fs, ino, out);
        }
        at = start + len;
    }
    return rc;
}
