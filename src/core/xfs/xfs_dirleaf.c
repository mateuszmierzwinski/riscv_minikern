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

/* Whether another pair fits between the hashes and the `bests` array.
 *
 * `bests` grows downwards as data blocks are added and the hashes grow
 * upwards, so the two meet — which is where leaf format ends and node
 * format begins. */
static uint32_t leaf_hash_room(const struct xfs *fs, uint32_t count, uint32_t bestcount) {
    uint32_t end = bests_at(fs, bestcount);
    uint32_t used = XFS_DA3_HDR_SIZE + (count + 1u) * XFS_DA3_ENTRY_SIZE;
    return used <= end ? end - used + 1u : 0u;
}

/* Makes room in the leaf for `want` bests where there are `have`.
 *
 * The array is anchored at the tail and indexed upwards from its start, so
 * block zero's value sits lowest and growing the array moves every existing
 * value **down**. Getting that direction wrong writes block zero's free
 * span into block one's slot, which no reader here would notice and
 * `xfs_repair` would. */
static void grow_bests(struct xfs *fs, uint32_t have, uint32_t want) {
    uint32_t from = bests_at(fs, have);
    uint32_t to = bests_at(fs, want);
    for (uint32_t i = 0; i < have * XFS_DIR2_LEAF_BEST_SIZE; i++) {
        fs->dirblock[to + i] = fs->dirblock[from + i];
    }
    for (uint32_t i = have; i < want; i++) {
        xfs_put_be16(fs->dirblock + to + i * XFS_DIR2_LEAF_BEST_SIZE, 0);
    }
    xfs_put_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE, want);
}

/* Changes the leaf block: one pair in or out, and one `bests` value.
 *
 * `address` of zero means "do not touch the hashes", which is what a change
 * to free space alone needs. Loading, editing and storing the leaf is one
 * function because the buffer is shared: anything that read a data block in
 * between would overwrite it.
 *
 * Runs to 30 lines: it is one derivation over a single buffer — find the
 * leaf, grow its `bests` if the directory has gained a block, move its hash
 * array, fix the one free span, reseal and store — and the offsets involved
 * are meaningless apart from each other (Rule 11, Single derivation). */
static int edit_leaf(struct xfs *fs, const struct xfs_inode *dir, uint32_t hash,
                     uint32_t address, bool add, uint32_t which, uint32_t best,
                     uint32_t blocks) {
    uint64_t leaf_fsb = 0;
    uint32_t count, bestcount, at;
    int rc = load(fs, dir, fs->dir_leaf_block, &leaf_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != XFS_DIR3_LEAF1_MAGIC) {
        return KERR_NOTSUP; /* node format: its own entry in the roadmap */
    }
    count = xfs_be16(fs->dirblock + XFS_DA3_COUNT_OFF);
    bestcount = xfs_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE);
    if (leaf_hash_room(fs, count, blocks > bestcount ? blocks : bestcount) == 0) {
        return KERR_NOSPC;
    }
    if (blocks > bestcount) {
        grow_bests(fs, bestcount, blocks);
        bestcount = blocks;
    }
    if (address != 0 && add) {
        pairs_insert(fs->dirblock + XFS_DA3_HDR_SIZE, &count, hash, address);
    } else if (address != 0) {
        at = count;
        for (uint32_t i = 0; i < count; i++) {
            at = xfs_be32(fs->dirblock + XFS_DA3_HDR_SIZE + i * XFS_DA3_ENTRY_SIZE + 4u) ==
                         address
                     ? i
                     : at;
        }
        for (uint32_t i = at; i + 1u < count; i++) {
            xfs_copy_bytes(fs->dirblock + XFS_DA3_HDR_SIZE + i * XFS_DA3_ENTRY_SIZE,
                           fs->dirblock + XFS_DA3_HDR_SIZE + (i + 1u) * XFS_DA3_ENTRY_SIZE,
                           XFS_DA3_ENTRY_SIZE);
        }
        count -= at < count ? 1u : 0u;
    }
    xfs_put_be16(fs->dirblock + XFS_DA3_COUNT_OFF, (uint16_t)count);
    xfs_put_be16(fs->dirblock + bests_at(fs, bestcount) + which * XFS_DIR2_LEAF_BEST_SIZE,
                 (uint16_t)best);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return store(fs, leaf_fsb);
}

/* ── a directory with more than one data block ────────────────────────── */

/* How many data blocks the directory has. Its size counts those and not the
 * leaf, which is the one useful consequence of the leaf living at 32 GiB. */
static uint32_t data_blocks(const struct xfs *fs, const struct xfs_inode *dir) {
    return (uint32_t)(dir->size / fs->dirblocksize);
}

/* A leaf entry's address: the byte offset of the entry within the
 * directory's *data*, divided by eight.
 *
 * Not within its own block — a second data block makes the two differ, and
 * the reader has always decoded it this way (`name_at` divides by the
 * directory block size to find which block to read). Block-relative
 * offsets worked for exactly as long as there was one block to be relative
 * to, which is the kind of agreement that only breaks once there is a
 * second of something. */
static uint32_t address_of(const struct xfs *fs, uint32_t which, uint32_t at) {
    return (which * fs->dirblocksize + at) / XFS_DIR2_DATA_ALIGN;
}

/* Writes an empty data block into the buffer: a header and one free span. */
static void empty_data(struct xfs *fs, uint64_t ino) {
    for (uint32_t i = 0; i < fs->dirblocksize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be32(fs->dirblock + XFS_DIR3_HDR_MAGIC, XFS_DIR3_DATA_MAGIC);
    xfs_put_be64(fs->dirblock + 40u, ino);
    for (uint32_t i = 0; i < 16; i++) {
        fs->dirblock[24u + i] = fs->uuid[i];
    }
    xfs_dir_put_free(fs->dirblock, XFS_DIR3_DATA_HDR_SIZE,
                     fs->dirblocksize - XFS_DIR3_DATA_HDR_SIZE);
    xfs_put_be16(fs->dirblock + 48u, XFS_DIR3_DATA_HDR_SIZE);
    xfs_put_be16(fs->dirblock + 50u, (uint16_t)(fs->dirblocksize - XFS_DIR3_DATA_HDR_SIZE));
}

/* Adds a data extent to the fork, keeping it sorted and merging where it
 * can, and moves the size and block count with it.
 *
 * The leaf's extent is last and must stay last: the fork is read in order
 * and a reader stops at the first `startoff` above what it wants. Merging
 * matters for more than tidiness — a 256-byte inode's literal area holds
 * five extents, so a directory that never merged would run out of *inode*
 * long before it ran out of disk.
 *
 * Runs to 30 lines: the fork is one array that must stay ordered while two
 * inode fields move with it, and extracting either half would need the
 * array, its length, the insertion point and the inode buffer passed across
 * the boundary (Rule 11, Parameters/Single derivation). */
static int add_data_extent(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                           uint64_t fsb, uint64_t now_s) {
    struct xfs_extent e[XFS_DIR_FORK_EXTENTS];
    uint32_t count = dir->nextents;
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    if (count == 0 || count + 1u > sizeof e / sizeof e[0]) {
        return KERR_NOSPC; /* a b-tree fork for a directory: not written */
    }
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    for (uint32_t i = 0; i < count; i++) {
        xfs_extent_unpack(di + XFS_DI_V3_SIZE + i * XFS_EXTENT_BYTES, &e[i]);
    }
    /* The last *data* extent is the one before the leaf's. */
    if (count >= 2u && e[count - 2u].startoff + e[count - 2u].blockcount == logical &&
        e[count - 2u].startblock + e[count - 2u].blockcount == fsb) {
        e[count - 2u].blockcount += fs->dirblocks;
    } else {
        e[count] = e[count - 1u]; /* push the leaf's extent up */
        e[count - 1u].startoff = logical;
        e[count - 1u].startblock = fsb;
        e[count - 1u].blockcount = fs->dirblocks;
        count++;
    }
    for (uint32_t i = 0; i < count; i++) {
        xfs_extent_pack(di + XFS_DI_V3_SIZE + i * XFS_EXTENT_BYTES, &e[i]);
    }
    xfs_put_be32(di + XFS_DI_NEXTENTS, count);
    xfs_put_be64(di + XFS_DI_SIZE, dir->size + fs->dirblocksize);
    xfs_put_be64(di + XFS_DI_NBLOCKS, dir->nblocks + fs->dirblocks);
    xfs_put_be32(di + XFS_DI_MTIME, (uint32_t)now_s);
    xfs_put_be32(di + XFS_DI_CTIME, (uint32_t)now_s);
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
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
    pairs_insert(pairs, &count, xfs_dir_hashname(name, namelen), address_of(fs, 0, at));
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

/* Puts the entry in a data block that has room, growing the directory by a
 * block when none has. Returns which block took it, where in it, and that
 * block's largest remaining free span. */
static int place_entry(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                       uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s,
                       uint32_t *which, uint32_t *at, uint32_t *best, uint32_t *blocks) {
    uint32_t need = xfs_dir_entry_size(fs, namelen);
    uint32_t have = data_blocks(fs, dir);
    uint64_t fsb = 0;
    uint32_t len;
    int rc = KERR_OK;
    *blocks = have;
    for (*which = 0; *which < have; (*which)++) {
        rc = load(fs, dir, (uint64_t)*which * fs->dirblocks, &fsb);
        if (rc != KERR_OK) {
            return rc;
        }
        *at = xfs_dir_find_room(fs, fs->dirblock, fs->dirblocksize, need);
        if (*at != 0) {
            break;
        }
    }
    if (*which == have) {
        /* **Every block is full, so the directory gains one.** Its logical
         * offset is the next one up, which is below the leaf's 32 GiB by a
         * wide margin — that gap is the whole reason the format puts the
         * leaf up there. */
        rc = take_block(fs, dir->ino >> fs->inopblog, &fsb);
        if (rc != KERR_OK) {
            return rc;
        }
        empty_data(fs, dir->ino);
        xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
        rc = store(fs, fsb);
        rc = rc != KERR_OK ? rc
                           : add_data_extent(fs, dir, (uint64_t)have * fs->dirblocks, fsb,
                                             now_s);
        if (rc != KERR_OK) {
            return rc;
        }
        *blocks = have + 1u;
        *at = XFS_DIR3_DATA_HDR_SIZE;
        fs->dir_blocks_added++;
    }
    len = xfs_be16(fs->dirblock + *at + 2u);
    len = *at + len > fs->dirblocksize ? fs->dirblocksize - *at : len;
    (void)xfs_dir_put_entry(fs, fs->dirblock, *at, ino, name, namelen, ftype);
    if (len - need >= XFS_DIR2_DATA_ALIGN) {
        xfs_dir_put_free(fs->dirblock, *at + need, len - need);
    }
    *best = largest_free(fs, fs->dirblock);
    xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
    return store(fs, fsb);
}

int xfs_dir_leaf_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    uint32_t which = 0, at = 0, best = 0, blocks = 0;
    int rc = place_entry(fs, dir, name, namelen, ino, ftype, now_s, &which, &at, &best,
                         &blocks);
    rc = rc != KERR_OK ? rc
                       : edit_leaf(fs, dir, xfs_dir_hashname(name, namelen),
                                   address_of(fs, which, at), true, which, best, blocks);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, dir, now_s);
}

/* Finds a name in one data block and frees its entry. */
static int take_out(struct xfs *fs, const char *name, uint32_t namelen, uint64_t *ino,
                    uint32_t *at) {
    uint32_t cursor = XFS_DIR3_DATA_HDR_SIZE;
    while (cursor + 8u <= fs->dirblocksize) {
        uint32_t size;
        if (xfs_be16(fs->dirblock + cursor) == XFS_DIR2_DATA_FREE_TAG) {
            cursor += xfs_be16(fs->dirblock + cursor + 2u);
            continue;
        }
        size = xfs_dir_entry_size(fs, fs->dirblock[cursor + 8u]);
        if (fs->dirblock[cursor + 8u] == namelen &&
            xfs_dir_name_here(fs->dirblock + cursor + 9u, name, namelen)) {
            *ino = xfs_be64(fs->dirblock + cursor);
            xfs_dir_put_free(fs->dirblock, cursor, size);
            *at = cursor;
            return KERR_OK;
        }
        cursor += size;
    }
    return KERR_NOENT;
}

int xfs_dir_leaf_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t now_s, uint64_t *ino) {
    uint32_t blocks = data_blocks(fs, dir);
    uint64_t fsb = 0;
    uint32_t at = 0;
    int rc = KERR_NOENT;
    uint32_t which;
    for (which = 0; which < blocks; which++) {
        rc = load(fs, dir, (uint64_t)which * fs->dirblocks, &fsb);
        if (rc != KERR_OK) {
            return rc;
        }
        rc = take_out(fs, name, namelen, ino, &at);
        if (rc == KERR_OK) {
            break;
        }
    }
    if (rc != KERR_OK) {
        return rc;
    }
    /* **The block is not given back.** A directory that shrinks keeps its
     * blocks and reuses the space, which is what XFS itself does until a
     * whole block falls empty — and reclaiming one means removing an extent
     * and a `bests` slot, which is a step of its own. */
    {
        uint32_t best = largest_free(fs, fs->dirblock);
        xfs_reseal(fs->dirblock, fs->dirblocksize, XFS_DIR3_HDR_CRC);
        rc = store(fs, fsb);
        rc = rc != KERR_OK ? rc
                           : edit_leaf(fs, dir, 0, address_of(fs, which, at), false, which,
                                       best, blocks);
    }
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, dir, now_s);
}
