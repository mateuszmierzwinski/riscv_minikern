#include <stddef.h>

#include "core/console.h"
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
            /* **The numbers, not just the refusal.** Every way this goes
             * wrong — a writer using the wrong entry size, a size field
             * that was not updated, an `i8count` disagreement — produces
             * this one branch, and they want different fixes. */
            kprintf("xfs: inode %u's shortform entry %u of %u runs past its size: "
                    "at %u + %u > %u (namelen %u, ftype %u, inolen %u)\n",
                    (uint32_t)dir->ino, i, count, at, need, (uint32_t)dir->size, name_len,
                    fs->ftype ? 1u : 0u, inolen);
            return KERR_IO;
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
    uint32_t size = fs->dirblocksize;
    uint32_t count;
    /* **The directory block's size, not the filesystem's.** They are the
     * same in the default geometry, which is why saying `blocksize` here
     * worked for as long as `-n size=` was refused at mount: a tail read at
     * the wrong offset is a count of leaf entries taken from the middle of
     * somebody's filename. */
    if (xfs_be32(block + XFS_DIR3_HDR_MAGIC) != xfs_magic_for(fs, XFS_BLK_DIR_BLOCK)) {
        return size;
    }
    count = xfs_be32(block + size - XFS_DIR2_BLOCK_TAIL_SIZE);
    if (count > (size - fs->dir_data_hdr) / XFS_DIR2_LEAF_ENTRY_SIZE) {
        return fs->dir_data_hdr; /* a tail that cannot be true: read nothing */
    }
    return size - XFS_DIR2_BLOCK_TAIL_SIZE - count * XFS_DIR2_LEAF_ENTRY_SIZE;
}

/* Walks the entries of one directory block. */
static int list_block(struct xfs *fs, const uint8_t *block, xfs_dirent_fn fn, void *self) {
    uint32_t end = data_end(fs, block);
    uint32_t at = fs->dir_data_hdr;
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
/* One directory block's bytes, however many filesystem blocks it took.
 *
 * `held` is the cache buffer to give back, and it is `NULL` when the block
 * was gathered into the filesystem's buffer instead — which is the whole
 * reason this is a struct rather than a pointer. The single-block case, the
 * default geometry, hands back the cached block directly and copies
 * nothing.
 */
void xfs_dir_block_put(struct xfs *fs, struct xfs_dirblock *b) {
    if (b->held != NULL) {
        bcache_put(fs->cache, b->held);
        b->held = NULL;
    }
}

/* Reads the filesystem blocks of one directory block into `fs->dirblock`.
 *
 * Each is found through `xfs_bmap`, so a directory whose extent map is a
 * b-tree gathers exactly like one whose extents are in the inode. A hole
 * at its first block is a directory block that is not there — `KERR_NOENT`,
 * which a directory that has given back an emptied block has — and a hole
 * part way through one is corruption: the block's own checksum covers all
 * of it. */
static int gather(struct xfs *fs, const struct xfs_inode *dir, uint64_t first) {
    for (uint32_t i = 0; i < fs->dirblocks; i++) {
        struct xfs_extent e;
        struct bbuf *b;
        uint64_t at = first + i;
        int rc = xfs_bmap(fs, dir, at, &e);
        if (rc != KERR_OK) {
            return rc == KERR_NOENT && i > 0 ? KERR_IO : rc;
        }
        rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, e.startblock + (at - e.startoff)), &b);
        if (rc != KERR_OK) {
            return rc;
        }
        xfs_copy_bytes(fs->dirblock + (uint64_t)i * fs->blocksize, b->data, fs->blocksize);
        bcache_put(fs->cache, b);
    }
    return KERR_OK;
}

/* The bytes of one directory block, gathered if it spans several, and **not
 * verified** — its caller knows which kind of block it asked for and
 * therefore where the magic, the checksum, the UUID and the owner are. */
static int raw_dir_block(struct xfs *fs, const struct xfs_inode *dir, uint64_t first,
                         struct xfs_dirblock *out) {
    struct xfs_extent e;
    int rc;
    out->at = NULL;
    out->held = NULL;
    if (fs->dirblocks > 1u) {
        rc = gather(fs, dir, first);
        out->at = rc == KERR_OK ? fs->dirblock : NULL;
        return rc;
    }
    rc = xfs_bmap(fs, dir, first, &e);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, e.startblock + (first - e.startoff)),
                    &out->held);
    out->at = rc == KERR_OK ? out->held->data : NULL;
    return rc;
}

/* The three checks every metadata block gets, at the offsets of its kind.
 *
 * **A directory's data blocks and its index blocks do not agree about where
 * any of this lives**, and that is the mistake this function exists to stop
 * repeating: a data block carries a 32-bit magic at offset 0 with its
 * checksum at 4, while a leaf or node block is an `xfs_da3_blkinfo` whose
 * magic is **16 bits at offset 8** with its checksum at 12. Verifying one as
 * the other reads two bytes of a block number as a magic and rejects every
 * block — which is exactly what it did. */
static int verified(const struct xfs *fs, const uint8_t *at, uint64_t ino, uint32_t crc_off,
                    uint32_t uuid_off, uint32_t owner_off) {
    if (!xfs_crc_ok(fs, at, fs->dirblocksize, crc_off) || !xfs_uuid_ok(fs, at + uuid_off)) {
        return KERR_IO;
    }
    /* **The owner is the third of v5's self-describing fields**, with the
     * checksum and the UUID — all three arrived together and none of them
     * exists on v4, where offset 24 is inside the best-free array. The
     * first two are skipped inside their own helpers; this one is here,
     * which is why it was the one still comparing garbage against an inode
     * number and rejecting every directory block of a sound filesystem. */
    if (!fs->v5) {
        return KERR_OK;
    }
    return xfs_be64(at + owner_off) == ino ? KERR_OK : KERR_IO;
}

int xfs_dir_data_get(struct xfs *fs, const struct xfs_inode *dir, uint64_t first,
                     struct xfs_dirblock *out) {
    uint32_t magic;
    int rc = raw_dir_block(fs, dir, first, out);
    if (rc != KERR_OK) {
        return rc;
    }
    magic = xfs_be32(out->at + XFS_DIR3_HDR_MAGIC);
    rc = magic == xfs_magic_for(fs, XFS_BLK_DIR_BLOCK) || magic == xfs_magic_for(fs, XFS_BLK_DIR_DATA)
             ? verified(fs, out->at, dir->ino, XFS_DIR3_HDR_CRC, XFS_DIR3_HDR_UUID,
                        XFS_DIR3_HDR_OWNER)
             : KERR_IO;
    if (rc != KERR_OK) {
        xfs_dir_block_put(fs, out);
    }
    return rc;
}

int xfs_dir_index_get(struct xfs *fs, const struct xfs_inode *dir, uint64_t first,
                      struct xfs_dirblock *out) {
    uint32_t magic;
    int rc = raw_dir_block(fs, dir, first, out);
    if (rc != KERR_OK) {
        return rc;
    }
    magic = xfs_be16(out->at + XFS_DA3_MAGIC_OFF);
    rc = magic == xfs_magic_for(fs, XFS_BLK_DIR_LEAF1) || magic == xfs_magic_for(fs, XFS_BLK_DIR_LEAFN) ||
                 magic == xfs_magic_for(fs, XFS_BLK_DA_NODE)
             ? verified(fs, out->at, dir->ino, XFS_DA3_CRC_OFF, XFS_DA3_UUID_OFF,
                        XFS_DA3_OWNER_OFF)
             : KERR_IO;
    if (rc != KERR_OK) {
        xfs_dir_block_put(fs, out);
    }
    return rc;
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
    /* **Stepping by directory blocks, not filesystem blocks.** A directory
     * block is the unit with a header, a tail and a checksum; the
     * filesystem blocks inside one are continuation, and entries straddle
     * them freely. */
    for (uint64_t i = 0; i < blocks && rc == KERR_OK; i += fs->dirblocks) {
        struct xfs_dirblock b;
        if (i >= fs->dir_leaf_block) {
            break; /* the hash structures live above this: never data */
        }
        rc = xfs_dir_data_get(fs, dir, i, &b);
        if (rc == KERR_NOENT) {
            rc = KERR_OK; /* a hole in a directory: nothing to list */
            continue;
        }
        if (rc != KERR_OK) {
            return rc;
        }
        rc = list_block(fs, b.at, fn, self);
        xfs_dir_block_put(fs, &b);
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
    /* **The index first, the walk when there is no index.**
     *
     * A shortform or block-format directory has no leaf block to search and
     * a handful of entries to walk, so the fallback is not a lesser path —
     * it is the right one for those. `KERR_NOTSUP` is how the hash lookup
     * says "this directory has no index", which is different from "the name
     * is not here" and must not be confused with it: treating the first as
     * the second would make every lookup in a small directory fail. */
    rc = xfs_dir_hash_lookup(fs, dir, name, len, ino);
    if (rc != KERR_NOTSUP) {
        fs->hash_lookups++;
        return rc;
    }
    fs->walked_lookups++;
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
