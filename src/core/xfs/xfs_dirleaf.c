/* A directory outgrowing one block: block format becomes leaf format.
 *
 * **The last shape a growing directory passes through here.** Shortform
 * holds about a dozen names inside the inode; one block directory holds
 * about a hundred and twenty; past that the entries need blocks of their
 * own and their hashes need a block of their own. That is leaf format — the
 * shape `mkfs` gives any directory with a few hundred files in it, and the
 * one this kernel could read and not produce.
 *
 * The conversion is mostly a **re-labelling**. The block that was the whole
 * directory becomes data block zero: its magic changes from `XDB3` to
 * `XDD3`, and the space its hash array and tail occupied becomes free. The
 * hashes move to a new block at 32 GiB of logical offset, which is where
 * the format keeps them so that they can never collide with data — and why
 * a leaf directory's *size* counts only its data.
 *
 * Two things a writer must maintain that this reader never looks at, and
 * which are therefore the easy ones to leave wrong. The **tag** at each
 * entry's end, and the leaf's **`bests` array** — one value per data block
 * giving that block's largest free span, with a `bestcount` tail. This
 * reader walks the blocks and uses the hashes instead; `xfs_repair` and
 * Linux read `bests` and will call a filesystem corrupt if it lies.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* Where a leaf block's `bests` array begins, for `n` data blocks. */
static uint32_t bests_at(const struct xfs *fs, uint32_t n) {
    return fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE - n * XFS_DIR2_LEAF_BEST_SIZE;
}

/* The largest free span in a data block, which is what `bests` records. */
static uint32_t largest_free(const struct xfs *fs, const uint8_t *block) {
    uint32_t at = XFS_DIR3_DATA_HDR_SIZE;
    uint32_t best = 0;
    while (at + 8u <= fs->dirblocksize) {
        uint32_t len;
        if (xfs_be16(block + at) != XFS_DIR2_DATA_FREE_TAG) {
            at += xfs_dir_entry_size(fs, block[at + 8u]);
            continue;
        }
        len = xfs_be16(block + at + 2u);
        if (len < XFS_DIR2_DATA_ALIGN || at + len > fs->dirblocksize) {
            break;
        }
        best = len > best ? len : best;
        at += len;
    }
    return best;
}

/* Reads one of the directory's blocks into the filesystem's buffer.
 *
 * There is one buffer, so a step that needs two blocks at once has to write
 * one before reading the other — which is why the conversion below is
 * ordered the way it is rather than the way it reads. */
static int load(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical, uint64_t *fsb) {
    struct xfs_extent e;
    struct bbuf *b;
    int rc = xfs_bmap(fs, dir, logical, &e);
    if (rc != KERR_OK) {
        return rc;
    }
    *fsb = e.startblock + (logical - e.startoff);
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, *fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(fs->dirblock, b->data, fs->blocksize);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

static int store(struct xfs *fs, uint64_t fsb) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(b->data, fs->dirblock, fs->blocksize);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* A fresh block in the same group as the directory. */
static int take_block(struct xfs *fs, uint64_t near, uint64_t *fsb) {
    uint32_t agno = xfs_fsb_ag(fs, near);
    uint32_t agbno = 0;
    int rc = xfs_alloc_extent(fs, agno, 1u, 0, &agbno);
    if (rc != KERR_OK) {
        return rc;
    }
    *fsb = ((uint64_t)agno << fs->agblklog) | agbno;
    return KERR_OK;
}

/* Inserts into a sorted array of `{hash, address}` pairs held in bytes. */
static void pairs_insert(uint8_t *pairs, uint32_t *count, uint32_t hash, uint32_t address) {
    uint32_t at = *count;
    for (uint32_t i = 0; i < *count; i++) {
        if (xfs_be32(pairs + i * XFS_DA3_ENTRY_SIZE) > hash) {
            at = i;
            break;
        }
    }
    for (uint32_t i = *count; i > at; i--) {
        xfs_copy_bytes(pairs + i * XFS_DA3_ENTRY_SIZE, pairs + (i - 1u) * XFS_DA3_ENTRY_SIZE,
                       XFS_DA3_ENTRY_SIZE);
    }
    xfs_put_be32(pairs + at * XFS_DA3_ENTRY_SIZE, hash);
    xfs_put_be32(pairs + at * XFS_DA3_ENTRY_SIZE + 4u, address);
    (*count)++;
}

/* ── the leaf block ────────────────────────────────────────────────────── */

/* Builds a leaf block in the buffer from a sorted set of pairs. */
static void build_leaf(struct xfs *fs, uint64_t ino, const uint8_t *pairs, uint32_t count,
                       uint32_t best) {
    for (uint32_t i = 0; i < fs->dirblocksize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be16(fs->dirblock + XFS_DA3_MAGIC_OFF, XFS_DIR3_LEAF1_MAGIC);
    xfs_put_be16(fs->dirblock + XFS_DA3_COUNT_OFF, (uint16_t)count);
    xfs_put_be64(fs->dirblock + XFS_DA3_OWNER_OFF, ino);
    for (uint32_t i = 0; i < 16; i++) {
        fs->dirblock[XFS_DA3_UUID_OFF + i] = fs->uuid[i];
    }
    xfs_copy_bytes(fs->dirblock + XFS_DA3_HDR_SIZE, pairs, count * XFS_DA3_ENTRY_SIZE);
    xfs_put_be16(fs->dirblock + bests_at(fs, 1u), (uint16_t)best);
    xfs_put_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE, 1u);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
}

/* Whether another pair fits between the hashes and the `bests` array. */
static uint32_t leaf_hash_room(const struct xfs *fs, uint32_t count) {
    uint32_t end = bests_at(fs, 1u);
    uint32_t used = XFS_DA3_HDR_SIZE + (count + 1u) * XFS_DA3_ENTRY_SIZE;
    return used <= end ? end - used + 1u : 0u;
}

/* Changes the leaf block: one pair in or out, and one `bests` value.
 *
 * `address` of zero means "do not touch the hashes", which is what a change
 * to free space alone needs. Loading, editing and storing the leaf is one
 * function because the buffer is shared: anything that reads a data block
 * in between would overwrite it.
 *
 * Runs to 29 lines: it is one derivation — find the leaf, move its array,
 * fix its `bests`, reseal and store — and the intermediate values are
 * offsets into a buffer that only make sense together (Rule 11, Single
 * derivation). */
static int edit_leaf(struct xfs *fs, const struct xfs_inode *dir, uint32_t hash,
                     uint32_t address, bool add, uint32_t best) {
    uint64_t leaf_fsb = 0;
    uint32_t count, at;
    int rc = load(fs, dir, fs->dir_leaf_block, &leaf_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != XFS_DIR3_LEAF1_MAGIC) {
        return KERR_NOTSUP; /* node format: its own entry in the roadmap */
    }
    count = xfs_be16(fs->dirblock + XFS_DA3_COUNT_OFF);
    if (leaf_hash_room(fs, count) == 0) {
        return KERR_NOSPC;
    }
    if (address != 0 && add) {
        pairs_insert(fs->dirblock + XFS_DA3_HDR_SIZE, &count, hash, address);
    } else if (address != 0) {
        at = count;
        for (uint32_t i = 0; i < count; i++) {
            uint8_t *e = fs->dirblock + XFS_DA3_HDR_SIZE + i * XFS_DA3_ENTRY_SIZE;
            at = xfs_be32(e + 4u) == address ? i : at;
        }
        for (uint32_t i = at; i + 1u < count; i++) {
            xfs_copy_bytes(fs->dirblock + XFS_DA3_HDR_SIZE + i * XFS_DA3_ENTRY_SIZE,
                           fs->dirblock + XFS_DA3_HDR_SIZE + (i + 1u) * XFS_DA3_ENTRY_SIZE,
                           XFS_DA3_ENTRY_SIZE);
        }
        count -= at < count ? 1u : 0u;
    }
    xfs_put_be16(fs->dirblock + XFS_DA3_COUNT_OFF, (uint16_t)count);
    xfs_put_be16(fs->dirblock + bests_at(fs, 1u), (uint16_t)best);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return store(fs, leaf_fsb);
}


/* ── the conversion, and adding and removing afterwards ───────────────── */

/* Gives the inode its second extent: the leaf block, above the data.
 *
 * The directory's size does not count it. A leaf directory's size is its
 * data, and the hashes sit at 32 GiB of logical offset precisely so that
 * they are outside it — a walk that went up to the size would otherwise
 * walk into them. */
static int add_leaf_extent(struct xfs *fs, const struct xfs_inode *dir, uint64_t leaf_fsb,
                           uint64_t now_s) {
    struct xfs_extent e = {.startoff = fs->dir_leaf_block, .startblock = leaf_fsb,
                           .blockcount = 1};
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    xfs_extent_pack(di + XFS_DI_V3_SIZE + XFS_EXTENT_BYTES, &e);
    xfs_put_be32(di + XFS_DI_NEXTENTS, 2u);
    xfs_put_be64(di + XFS_DI_NBLOCKS, 2u);
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_dir_to_leaf(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                    uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    uint8_t *pairs = fs->dirpairs;
    uint32_t count, old_end, at, best;
    uint64_t data_fsb = 0, leaf_fsb = 0;
    int rc = load(fs, dir, 0, &data_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(fs->dirblock + XFS_DIR3_HDR_MAGIC) != XFS_DIR3_BLOCK_MAGIC) {
        return KERR_NOTSUP;
    }
    count = xfs_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE);
    old_end = fs->dirblocksize - XFS_DIR2_BLOCK_TAIL_SIZE - count * XFS_DA3_ENTRY_SIZE;
    if ((count + 1u) * XFS_DA3_ENTRY_SIZE > sizeof fs->dirpairs) {
        return KERR_NOSPC;
    }
    xfs_copy_bytes(pairs, fs->dirblock + old_end, count * XFS_DA3_ENTRY_SIZE);
    /* **The hashes and the tail become free space**, and the block becomes
     * pure data: it now reaches the end of itself. */
    xfs_put_be32(fs->dirblock + XFS_DIR3_HDR_MAGIC, XFS_DIR3_DATA_MAGIC);
    xfs_dir_put_free(fs->dirblock, old_end, fs->dirblocksize - old_end);
    at = xfs_dir_find_room(fs, fs->dirblock, fs->dirblocksize,
                           xfs_dir_entry_size(fs, namelen));
    if (at == 0) {
        return KERR_NOSPC;
    }
    {
        uint32_t need = xfs_dir_entry_size(fs, namelen);
        uint32_t len = xfs_be16(fs->dirblock + at + 2u);
        len = at + len > fs->dirblocksize ? fs->dirblocksize - at : len;
        (void)xfs_dir_put_entry(fs, fs->dirblock, at, ino, name, namelen, ftype);
        if (len - need >= XFS_DIR2_DATA_ALIGN) {
            xfs_dir_put_free(fs->dirblock, at + need, len - need);
        }
    }
    pairs_insert(pairs, &count, xfs_dir_hashname(name, namelen), at / XFS_DIR2_DATA_ALIGN);
    xfs_put_be16(fs->dirblock + 48u, 0);
    xfs_put_be16(fs->dirblock + 50u, 0);
    best = largest_free(fs, fs->dirblock);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
    rc = store(fs, data_fsb);
    rc = rc != KERR_OK ? rc : take_block(fs, data_fsb, &leaf_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    build_leaf(fs, dir->ino, pairs, count, best);
    rc = store(fs, leaf_fsb);
    return rc != KERR_OK ? rc : add_leaf_extent(fs, dir, leaf_fsb, now_s);
}

int xfs_dir_leaf_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    uint32_t need = xfs_dir_entry_size(fs, namelen);
    uint64_t data_fsb = 0;
    uint32_t at, len, best;
    int rc = load(fs, dir, 0, &data_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **One data block.** Adding a second means extending the directory's
     * size, allocating at the next logical offset and growing `bests` — the
     * next step after this one, and `KERR_NOSPC` until then rather than a
     * block written where the format does not expect one. */
    at = xfs_dir_find_room(fs, fs->dirblock, fs->dirblocksize, need);
    if (at == 0) {
        return KERR_NOSPC;
    }
    len = xfs_be16(fs->dirblock + at + 2u);
    len = at + len > fs->dirblocksize ? fs->dirblocksize - at : len;
    (void)xfs_dir_put_entry(fs, fs->dirblock, at, ino, name, namelen, ftype);
    if (len - need >= XFS_DIR2_DATA_ALIGN) {
        xfs_dir_put_free(fs->dirblock, at + need, len - need);
    }
    best = largest_free(fs, fs->dirblock);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
    rc = store(fs, data_fsb);
    rc = rc != KERR_OK ? rc
                       : edit_leaf(fs, dir, xfs_dir_hashname(name, namelen),
                                   at / XFS_DIR2_DATA_ALIGN, true, best);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, dir, now_s);
}

int xfs_dir_leaf_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t now_s, uint64_t *ino) {
    uint64_t data_fsb = 0;
    uint32_t at = XFS_DIR3_DATA_HDR_SIZE;
    uint32_t best, found = 0;
    int rc = load(fs, dir, 0, &data_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    while (at + 8u <= fs->dirblocksize) {
        uint32_t size;
        if (xfs_be16(fs->dirblock + at) == XFS_DIR2_DATA_FREE_TAG) {
            at += xfs_be16(fs->dirblock + at + 2u);
            continue;
        }
        size = xfs_dir_entry_size(fs, fs->dirblock[at + 8u]);
        if (fs->dirblock[at + 8u] == namelen &&
            xfs_dir_name_here(fs->dirblock + at + 9u, name, namelen)) {
            *ino = xfs_be64(fs->dirblock + at);
            xfs_dir_put_free(fs->dirblock, at, size);
            found = at;
            break;
        }
        at += size;
    }
    if (found == 0) {
        return KERR_NOENT;
    }
    best = largest_free(fs, fs->dirblock);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
    rc = store(fs, data_fsb);
    rc = rc != KERR_OK ? rc
                       : edit_leaf(fs, dir, 0, found / XFS_DIR2_DATA_ALIGN, false, best);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, dir, now_s);
}
