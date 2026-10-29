/* A directory whose hashes outgrow one block: leaf format becomes node.
 *
 * **The last shape a directory has, and the one that does not run out.**
 * Leaf format keeps every hash in one block, so it ends at about five
 * hundred names however many data blocks it has. Node format puts a
 * **node** block above several `LEAFN` blocks and indexes them by hash, so
 * the ceiling becomes the node's own capacity — five hundred leaves of five
 * hundred hashes, which is past any directory a person makes.
 *
 * Three things move in the conversion, and the middle one is the surprise:
 *
 *   - the one `LEAF1` block's hashes are split across **two** `LEAFN`
 *     blocks, chained by `forw`/`back` so that a run of equal hashes can
 *     continue into the next;
 *   - the block that was the leaf becomes the **node root**, and the leaves
 *     move to new addresses. The root has to keep its block number because
 *     that is where a search starts (`fs->dir_leaf_block`) — so the
 *     conversion moves the *contents* out rather than putting the root
 *     somewhere new;
 *   - the `bests` array cannot stay, because a `LEAFN` block is all
 *     hashes. It moves to a **free block** at 64 GiB of logical offset, a
 *     third address space above the data and the leaves.
 *
 * This reader never consults `bests`; it walks the data blocks. It is
 * maintained because `xfs_repair` and Linux read it, which makes it exactly
 * the structure a writer leaves wrong.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* A da-block number, which is in **filesystem** blocks and not directory
 * blocks: a directory block spans `fs->dirblocks` of them, and the reader
 * uses what it finds here directly as a logical block to map. */
static uint64_t leaf_n(const struct xfs *fs, uint32_t which) {
    return fs->dir_leaf_block + (uint64_t)(which + 1u) * fs->dirblocks;
}

/* How many hash pairs a `LEAFN` block can hold. No `bests` and no tail —
 * that is what moving them to the free block bought. */
static uint32_t leafn_capacity(const struct xfs *fs) {
    return (fs->dirblocksize - fs->da_hdr) / XFS_DA3_ENTRY_SIZE;
}

/* How many children the node root can name. */
static uint32_t node_capacity(const struct xfs *fs) {
    return (fs->dirblocksize - fs->da_hdr) / XFS_DA3_ENTRY_SIZE;
}

/* Writes a `da3` header into the buffer: the shape every index block of a
 * directory shares, whatever it then holds. */
static void da3_header(struct xfs *fs, uint16_t magic, uint64_t ino, uint32_t count,
                       uint64_t forw, uint64_t back) {
    for (uint32_t i = 0; i < fs->dirblocksize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be32(fs->dirblock + XFS_DA3_FORW_OFF, (uint32_t)forw);
    xfs_put_be32(fs->dirblock + XFS_DA3_BACK_OFF, (uint32_t)back);
    xfs_put_be16(fs->dirblock + XFS_DA3_MAGIC_OFF, magic);
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)count);
    xfs_stamp_owner(fs, fs->dirblock, 0, XFS_DA3_UUID_OFF, XFS_DA3_OWNER_OFF, 0, ino);
}

/* The highest hash in a run of pairs, which is what a node entry carries
 * for its child: a search takes the first child whose hash is at or above
 * what it wants. */
static uint32_t top_hash(const uint8_t *pairs, uint32_t count) {
    return count == 0 ? 0 : xfs_be32(pairs + (count - 1u) * XFS_DA3_ENTRY_SIZE);
}

/* Where the free block's three counts live: after the v5 header, or
 * straight after the magic on v4. `nth` is 0, 1 or 2 — `firstdb`, `nvalid`
 * and `nused`. These were v5's offsets on both, which on v4 put the counts
 * inside the `bests` array. */
static uint32_t free_count_at(const struct xfs *fs, uint32_t nth) {
    return (fs->v5 ? XFS_DIR3_FREE_FIRSTDB : XFS_DIR2_FREE_FIRSTDB) + 4u * nth;
}

/* Recounts `nused`: the entries that name a data block at all.
 *
 * **Not the ones with room.** A best of zero is a data block that is
 * entirely full, which is a block in use; only `NULLDATAOFF` means none.
 * This counted the non-zero ones, so a full block went uncounted — and
 * once the free block existed nothing recounted it as blocks were added. */
static void free_recount(struct xfs *fs) {
    uint32_t n = xfs_be32(fs->dirblock + free_count_at(fs, 1u)), used = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = fs->dir_free_hdr + i * XFS_DIR2_LEAF_BEST_SIZE;
        used += xfs_be16(fs->dirblock + at) != XFS_DIR2_NULL_DATAOFF ? 1u : 0u;
    }
    xfs_put_be32(fs->dirblock + free_count_at(fs, 2u), used);
}

/* Ends `nvalid` at the last data block there is, past any trailing slots a
 * given-back block left as `NULLDATAOFF`. */
static void free_trim(struct xfs *fs) {
    uint32_t n = xfs_be32(fs->dirblock + free_count_at(fs, 1u));
    while (n > 0 && xfs_be16(fs->dirblock + fs->dir_free_hdr +
                             (n - 1u) * XFS_DIR2_LEAF_BEST_SIZE) == XFS_DIR2_NULL_DATAOFF) {
        xfs_put_be16(fs->dirblock + fs->dir_free_hdr + (n - 1u) * XFS_DIR2_LEAF_BEST_SIZE, 0);
        n--;
    }
    xfs_put_be32(fs->dirblock + free_count_at(fs, 1u), n);
}

/* Writes the free block: the `bests` array, evicted from the leaf.
 * `nvalid` is how many data blocks it describes and `nused` how many of
 * those exist. */
static void build_free(struct xfs *fs, uint64_t ino, const uint8_t *bests, uint32_t n) {
    for (uint32_t i = 0; i < fs->dirblocksize; i++) {
        fs->dirblock[i] = 0;
    }
    xfs_put_be32(fs->dirblock, xfs_magic_for(fs, XFS_BLK_DIR_FREE));
    /* The UUID at 24 and the owner at 40, as every v5 directory header has
     * them. The owner went in at 32, over the UUID's second half, and the
     * field `xfs_repair` reads stayed zero. */
    xfs_stamp_owner(fs, fs->dirblock, 0, XFS_DIR3_HDR_UUID, XFS_DIR3_HDR_OWNER, 0, ino);
    xfs_put_be32(fs->dirblock + free_count_at(fs, 0), 0);
    xfs_put_be32(fs->dirblock + free_count_at(fs, 1u), n);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t best = (uint16_t)xfs_be16(bests + i * XFS_DIR2_LEAF_BEST_SIZE);
        xfs_put_be16(fs->dirblock + fs->dir_free_hdr + i * XFS_DIR2_LEAF_BEST_SIZE,
                     best);
    }
    free_recount(fs);
}

/* Records one data block's largest free span in the free block.
 *
 * Loaded, edited and stored in one function because the directory buffer is
 * shared: anything that touched a data block in between would overwrite it.
 * The same reason `edit_leaf` is one function in the leaf format. */
static int free_set(struct xfs *fs, const struct xfs_inode *dir, uint32_t which,
                    uint32_t best, uint32_t blocks) {
    uint64_t fsb = 0;
    uint32_t at = fs->dir_free_hdr + which * XFS_DIR2_LEAF_BEST_SIZE;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_free_block, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be32(fs->dirblock) != xfs_magic_for(fs, XFS_BLK_DIR_FREE) || at + 2u > fs->dirblocksize) {
        return KERR_IO;
    }
    xfs_put_be16(fs->dirblock + at, (uint16_t)best);
    if (xfs_be32(fs->dirblock + free_count_at(fs, 1u)) < blocks) {
        xfs_put_be32(fs->dirblock + free_count_at(fs, 1u), blocks);
    }
    free_trim(fs);
    free_recount(fs);
    return xfs_dir_raw_store(fs, fsb);
}

/* ── the conversion ───────────────────────────────────────────────────── */

/* Moves the leaf's `bests` array into a free block of its own.
 *
 * A pass of its own, re-reading the leaf, rather than holding both halves
 * at once: the array can be eight kilobytes and the frame guard refused it
 * on a stack — the third time that rule has caught this work and the third
 * time it was right. The buffer is shared, so the cost is one cached read
 * and the gain is that no copy of the directory lives on a stack. */
static int free_block_from_leaf(struct xfs *fs, const struct xfs_inode *dir, uint64_t f_fsb) {
    uint8_t *bests = fs->dirpairs;
    uint64_t leaf_fsb = 0;
    uint32_t bestcount;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &leaf_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    bestcount = xfs_be32(fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE);
    if (bestcount * XFS_DIR2_LEAF_BEST_SIZE > XFS_DIRPAIRS_BYTES) {
        return KERR_NOSPC;
    }
    xfs_copy_bytes(bests,
                   fs->dirblock + fs->dirblocksize - XFS_DIR2_LEAF_TAIL_SIZE -
                       bestcount * XFS_DIR2_LEAF_BEST_SIZE,
                   bestcount * XFS_DIR2_LEAF_BEST_SIZE);
    build_free(fs, dir->ino, bests, bestcount);
    return xfs_dir_raw_store(fs, f_fsb);
}

/* Writes one of the two leaves the conversion splits into. */
static int write_leafn(struct xfs *fs, const struct xfs_inode *dir, const uint8_t *pairs,
                       uint32_t count, uint64_t forw, uint64_t back, uint64_t fsb) {
    da3_header(fs, xfs_magic_for(fs, XFS_BLK_DIR_LEAFN), dir->ino, count, forw, back);
    xfs_copy_bytes(fs->dirblock + fs->da_hdr, pairs, count * XFS_DA3_ENTRY_SIZE);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, fsb);
}

/* Turns the block that was the leaf into the node root naming both leaves.
 *
 * **The root keeps its block number**, which is why the conversion moves
 * the leaf's contents out rather than putting the root somewhere new: a
 * search starts at `dir_leaf_block` and would have no way to find a root
 * that had moved. */
static int write_root(struct xfs *fs, const struct xfs_inode *dir, const uint8_t *pairs,
                      uint32_t half, uint32_t count, uint64_t root_fsb) {
    uint8_t *e = fs->dirblock + fs->da_hdr;
    da3_header(fs, xfs_magic_for(fs, XFS_BLK_DA_NODE), dir->ino, 2u, 0, 0);
    xfs_put_be16(fs->dirblock + fs->da_level, 1u);
    xfs_put_be32(e, top_hash(pairs, half));
    xfs_put_be32(e + 4u, (uint32_t)leaf_n(fs, 0));
    xfs_put_be32(e + XFS_DA3_ENTRY_SIZE, top_hash(pairs, count));
    xfs_put_be32(e + XFS_DA3_ENTRY_SIZE + 4u, (uint32_t)leaf_n(fs, 1));
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, root_fsb);
}

int xfs_dir_to_node(struct xfs *fs, const struct xfs_inode *dir, uint64_t now_s) {
    uint8_t *pairs = fs->dirpairs;
    uint32_t count, half;
    uint64_t root_fsb = 0, a_fsb = 0, b_fsb = 0, f_fsb = 0;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != xfs_magic_for(fs, XFS_BLK_DIR_LEAF1)) {
        return KERR_NOTSUP; /* already a node, or not an index at all */
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    if (count * XFS_DA3_ENTRY_SIZE > XFS_DIRPAIRS_BYTES) {
        return KERR_NOSPC;
    }
    rc = xfs_dir_take_block(fs, dir->ino >> fs->inopblog, &a_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_take_block(fs, a_fsb, &b_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_take_block(fs, a_fsb, &f_fsb);
    /* The free block first, while the leaf still has the `bests` array in
     * it: the next step overwrites the leaf and the buffer is shared. */
    rc = rc != KERR_OK ? rc : free_block_from_leaf(fs, dir, f_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_copy_bytes(pairs, fs->dirblock + fs->da_hdr, count * XFS_DA3_ENTRY_SIZE);
    half = count / 2u;
    /* Chained forward, so that a run of equal hashes split across the
     * boundary is followed through `forw` — which the reader already does. */
    rc = write_leafn(fs, dir, pairs, half, leaf_n(fs, 1), 0, a_fsb);
    rc = rc != KERR_OK ? rc
                       : write_leafn(fs, dir, pairs + half * XFS_DA3_ENTRY_SIZE,
                                     count - half, 0, leaf_n(fs, 0), b_fsb);
    rc = rc != KERR_OK ? rc : write_root(fs, dir, pairs, half, count, root_fsb);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_insert(fs, dir, leaf_n(fs, 0), a_fsb, false, now_s);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_insert(fs, dir, leaf_n(fs, 1), b_fsb, false, now_s);
    return rc != KERR_OK ? rc
                         : xfs_dir_fork_insert(fs, dir, fs->dir_free_block, f_fsb, false,
                                               now_s);
}

uint16_t xfs_dir_index_shape(struct xfs *fs, const struct xfs_inode *dir) {
    uint64_t fsb = 0;
    if (xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &fsb) != KERR_OK) {
        return 0;
    }
    return (uint16_t)xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF);
}

/* ── adding and removing once it is a node ───────────────────────────── */

/* Which child of the root a hash belongs in, and that child's address.
 *
 * One level only. The root of a directory this kernel writes holds a few
 * hundred children of a few hundred hashes each, so a second level is tens
 * of thousands of names away; a deeper tree is refused rather than
 * half-descended. The *reader* walks any depth (`find_leaf`), so a
 * filesystem made elsewhere is still read — this is a limit on what is
 * written, which is the safer way round. */
static int child_for(struct xfs *fs, const struct xfs_inode *dir, uint32_t hash,
                     uint32_t *index, uint64_t *child, uint32_t *children) {
    uint64_t root_fsb = 0;
    uint32_t count;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16(fs->dirblock + XFS_DA3_MAGIC_OFF) != xfs_magic_for(fs, XFS_BLK_DA_NODE)) {
        return KERR_NOTSUP;
    }
    if (xfs_be16(fs->dirblock + fs->da_level) != 1u) {
        return KERR_NOTSUP; /* deeper than one level: read, not written */
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    if (count == 0 || fs->da_hdr + count * XFS_DA3_ENTRY_SIZE > fs->dirblocksize) {
        return KERR_IO;
    }
    *children = count;
    *index = count - 1u; /* past every child's top hash: the last one takes it */
    for (uint32_t i = 0; i < count; i++) {
        if (xfs_be32(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE) >= hash) {
            *index = i;
            break;
        }
    }
    *child = xfs_be32(fs->dirblock + fs->da_hdr + *index * XFS_DA3_ENTRY_SIZE + 4u);
    return KERR_OK;
}

/* Rewrites one of the root's entries, and may add one.
 *
 * `child` of zero leaves the address alone and changes only the hash, which
 * is what an insertion at the end of a leaf needs: the leaf's top hash has
 * moved up and the root has to agree, or a later search for that name stops
 * at the child before it. */
static int node_set(struct xfs *fs, const struct xfs_inode *dir, uint32_t index,
                    uint32_t hash, uint64_t child, bool insert) {
    uint64_t root_fsb = 0;
    uint32_t count;
    uint8_t *e;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    if (insert) {
        if (count + 1u > node_capacity(fs)) {
            return KERR_NOSPC; /* a second level: read, not written */
        }
        for (uint32_t i = count; i > index; i--) {
            xfs_copy_bytes(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE,
                           fs->dirblock + fs->da_hdr + (i - 1u) * XFS_DA3_ENTRY_SIZE,
                           XFS_DA3_ENTRY_SIZE);
        }
        count++;
        xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)count);
    }
    if (index >= count) {
        return KERR_IO;
    }
    e = fs->dirblock + fs->da_hdr + index * XFS_DA3_ENTRY_SIZE;
    xfs_put_be32(e, hash);
    if (child != 0) {
        xfs_put_be32(e + 4u, (uint32_t)child);
    }
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, root_fsb);
}

/* Splits a full leaf in two and gives the new half its own node entry.
 *
 * The new leaf takes the **upper** hashes and goes after the old one in the
 * chain, so `forw`/`back` stay in hash order — a run of equal hashes
 * crossing the boundary is still followed correctly, which is the one
 * property the chain exists for.
 *
 * Runs to 30 lines: it is one derivation over three blocks that must agree
 * — the old leaf's count and forward link, the new leaf's contents and both
 * links, and the root's two entries — and no half of it leaves the
 * directory readable (Rule 11, Single derivation). */
static int set_sibling(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                       uint32_t off, uint32_t value);

/* A logical block for a new hash leaf: past the highest the directory has.
 *
 * **Not `leaf_n(children)`.** That was the next leaf's place while leaves
 * only ever came into being; once an emptied one is given back, counting
 * children names a block a live leaf already has, and the fork would map
 * two blocks at one offset. */
static int next_leaf_at(struct xfs *fs, const struct xfs_inode *dir, uint64_t *at) {
    struct xfs_fork_map m;
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    int rc;
    xfs_inode_place(fs, dir->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_fork_load(fs, b->data + boffset, &m);
    bcache_put(fs->cache, b);
    *at = leaf_n(fs, 0);
    for (uint32_t i = 0; rc == KERR_OK && i < m.count; i++) {
        uint64_t end = m.e[i].startoff + m.e[i].blockcount;
        if (m.e[i].startoff >= fs->dir_leaf_block && m.e[i].startoff < fs->dir_free_block &&
            end > *at) {
            *at = end;
        }
    }
    return rc;
}

static int split_leaf(struct xfs *fs, struct xfs_inode *dir, uint32_t index,
                      uint64_t child, uint32_t children, uint64_t now_s) {
    uint8_t *pairs = fs->dirpairs;
    uint32_t count, half;
    uint64_t old_fsb = 0, new_fsb = 0, forw, at = 0;
    int rc = next_leaf_at(fs, dir, &at);
    (void)children;
    rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, child, &old_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    if (count * XFS_DA3_ENTRY_SIZE > XFS_DIRPAIRS_BYTES) {
        return KERR_NOSPC;
    }
    xfs_copy_bytes(pairs, fs->dirblock + fs->da_hdr, count * XFS_DA3_ENTRY_SIZE);
    half = count / 2u;
    rc = xfs_dir_take_block(fs, dir->ino >> fs->inopblog, &new_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The old leaf keeps the lower half and points at the new one. */
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)half);
    xfs_put_be32(fs->dirblock + XFS_DA3_FORW_OFF, (uint32_t)at);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    rc = xfs_dir_raw_store(fs, old_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    da3_header(fs, xfs_magic_for(fs, XFS_BLK_DIR_LEAFN), dir->ino, count - half, forw, child);
    xfs_copy_bytes(fs->dirblock + fs->da_hdr, pairs + half * XFS_DA3_ENTRY_SIZE,
                   (count - half) * XFS_DA3_ENTRY_SIZE);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    rc = xfs_dir_raw_store(fs, new_fsb);
    /* **And the leaf after it points back at the new one.** Left pointing at
     * the old leaf, the chain read differently forwards and backwards, and
     * the first removal to follow it relinked a live leaf out of it. */
    rc = rc != KERR_OK ? rc : set_sibling(fs, dir, forw, XFS_DA3_BACK_OFF, (uint32_t)at);
    rc = rc != KERR_OK ? rc : xfs_dir_fork_insert(fs, dir, at, new_fsb, false, now_s);
    /* The old child's top hash has fallen to the halfway pair, and the new
     * child goes in after it carrying what used to be the old one's. */
    rc = rc != KERR_OK ? rc : node_set(fs, dir, index, top_hash(pairs, half), 0, false);
    return rc != KERR_OK ? rc
                         : node_set(fs, dir, index + 1u, top_hash(pairs, count), at, true);
}

/* Re-reads the inode in place.
 *
 * **Called after anything that changes the fork**, because a caller's copy
 * of an inode is only valid until the inode changes — and growing a
 * directory changes it repeatedly: a data block here, a leaf there, three
 * extents during a conversion. Mapping a block that was just added through
 * a fork that predates it answers `KERR_NOENT` for a block that certainly
 * exists, which on the board read as a *create* reporting "no such file".
 *
 * Twice, in two different functions, before it was given a name. */
static int refresh(struct xfs *fs, struct xfs_inode *ip) {
    return xfs_inode_read(fs, ip->ino, ip);
}

/* Puts a hash into the leaf that owns it, splitting it first if it is full. */
static int leaf_insert(struct xfs *fs, struct xfs_inode *dir, uint32_t hash,
                       uint32_t address, uint64_t now_s) {
    uint32_t index = 0, children = 0, count;
    uint64_t child = 0, leaf_fsb = 0;
    int rc = child_for(fs, dir, hash, &index, &child, &children);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_dir_raw_load(fs, dir, child, &leaf_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_be16(fs->dirblock + fs->da_count) >= leafn_capacity(fs)) {
        rc = split_leaf(fs, dir, index, child, children, now_s);
        fs->leaf_splits += rc == KERR_OK ? 1u : 0u;
        /* The split gave the directory a block, so the fork has moved on
         * from the copy this was called with — and the leaf the hash now
         * belongs in may be the new one, which the old fork cannot map. */
        rc = rc != KERR_OK ? rc : refresh(fs, dir);
        rc = rc != KERR_OK ? rc : child_for(fs, dir, hash, &index, &child, &children);
        rc = rc != KERR_OK ? rc : xfs_dir_raw_load(fs, dir, child, &leaf_fsb);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    xfs_dir_pairs_insert(fs->dirblock + fs->da_hdr, &count, hash, address);
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)count);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    rc = xfs_dir_raw_store(fs, leaf_fsb);
    /* If it went in at the end, this leaf's top hash has moved and the root
     * must say so — otherwise a later search for that name stops at the
     * child before this one and reports the name missing. */
    if (rc == KERR_OK && hash >= top_hash(fs->dirblock + fs->da_hdr, count)) {
        rc = node_set(fs, dir, index, hash, 0, false);
    }
    return rc;
}

int xfs_dir_node_add(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                     uint32_t namelen, uint64_t ino, uint8_t ftype, uint64_t now_s) {
    struct xfs_inode here = *dir;
    uint32_t which = 0, at = 0, best = 0, blocks = 0;
    int rc = xfs_dir_place_entry(fs, &here, name, namelen, ino, ftype, now_s, &which, &at,
                                 &best, &blocks);
    /* Placing the entry may have given the directory a data block. */
    rc = rc != KERR_OK ? rc : refresh(fs, &here);
    rc = rc != KERR_OK ? rc
                       : leaf_insert(fs, &here, xfs_dir_hashname(name, namelen),
                                     xfs_dir_address_of(fs, which, at), now_s);
    rc = rc != KERR_OK ? rc : refresh(fs, &here);
    rc = rc != KERR_OK ? rc : free_set(fs, &here, which, best, blocks);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, &here, now_s);
}

/* Takes a hash out of whichever leaf holds it.
 *
 * The address identifies it, not the hash: two names can hash alike and
 * only one of them is being removed. A run of equal hashes may continue
 * into the next leaf through `forw`, so the search follows the chain — the
 * same reason the reader does. */
/* Writes `value` into the sibling pointer at `off` of the leaf at `logical`;
 * nothing for 0, which is "no sibling". */
static int set_sibling(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                       uint32_t off, uint32_t value) {
    uint64_t fsb = 0;
    int rc;
    if (logical == 0) {
        return KERR_OK;
    }
    rc = xfs_dir_raw_load(fs, dir, logical, &fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    xfs_put_be32(fs->dirblock + off, value);
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, fsb);
}

/* The root's entry naming leaf `child`, found by its pointer — which is
 * the one thing that names it unambiguously: a position worked out while
 * following a run of equal hashes along `forw` can be off by one, and
 * dropping by position once took out a live leaf's entry and left the
 * emptied one in place, so a name became unfindable. Leaves the root in
 * the buffer; `count` for none. */
static uint32_t entry_of(const struct xfs *fs, uint32_t count, uint64_t child) {
    for (uint32_t i = 0; i < count; i++) {
        if (xfs_be32(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE + 4u) == child) {
            return i;
        }
    }
    return count;
}

/* Takes the root's entry for leaf `child` out. */
/* Sets the hash of the root's entry for leaf `child`, found by pointer. */
static int node_set_for(struct xfs *fs, const struct xfs_inode *dir, uint64_t child,
                        uint32_t hash) {
    uint64_t root_fsb = 0;
    uint32_t count, index;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    index = entry_of(fs, count, child);
    return index < count ? node_set(fs, dir, index, hash, 0, false) : KERR_IO;
}

static int node_drop(struct xfs *fs, const struct xfs_inode *dir, uint64_t child) {
    uint64_t root_fsb = 0;
    uint32_t count, index;
    int rc = xfs_dir_raw_load(fs, dir, fs->dir_leaf_block, &root_fsb);
    if (rc != KERR_OK) {
        return rc;
    }
    count = xfs_be16(fs->dirblock + fs->da_count);
    index = entry_of(fs, count, child);
    if (index >= count || count < 2u) {
        return KERR_IO;
    }
    for (uint32_t i = index; i + 1u < count; i++) {
        xfs_copy_bytes(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE,
                       fs->dirblock + fs->da_hdr + (i + 1u) * XFS_DA3_ENTRY_SIZE,
                       XFS_DA3_ENTRY_SIZE);
    }
    xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)(count - 1u));
    xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
    return xfs_dir_raw_store(fs, root_fsb);
}

/* A hash leaf left with no hashes leaves the directory, as `xfs_da3_join`
 * takes one out: its entry in the root, its place in the sibling chain, and
 * its block. Kept, it carried a key for hashes it no longer had, which the
 * checker reports for every such leaf. The leaf is in the buffer. */
static int drop_empty_leaf(struct xfs *fs, const struct xfs_inode *dir, uint64_t logical,
                           uint64_t now_s) {
    uint32_t forw = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
    uint32_t back = xfs_be32(fs->dirblock + XFS_DA3_BACK_OFF);
    int rc = node_drop(fs, dir, logical);
    rc = rc != KERR_OK ? rc : set_sibling(fs, dir, back, XFS_DA3_FORW_OFF, forw);
    rc = rc != KERR_OK ? rc : set_sibling(fs, dir, forw, XFS_DA3_BACK_OFF, back);
    return rc != KERR_OK ? rc : xfs_dir_fork_remove(fs, dir, logical, now_s);
}

static int leaf_remove(struct xfs *fs, const struct xfs_inode *dir, uint32_t hash,
                       uint32_t address, uint64_t now_s) {
    uint32_t index = 0, children = 0;
    uint64_t child = 0;
    int rc = child_for(fs, dir, hash, &index, &child, &children);
    for (uint32_t hops = 0; rc == KERR_OK && hops <= XFS_DA3_MAX_LEVELS; hops++) {
        uint64_t leaf_fsb = 0;
        uint32_t count, found;
        rc = xfs_dir_raw_load(fs, dir, child, &leaf_fsb);
        if (rc != KERR_OK) {
            return rc;
        }
        count = xfs_be16(fs->dirblock + fs->da_count);
        found = count;
        for (uint32_t i = 0; i < count; i++) {
            found = xfs_be32(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE +
                             4u) == address
                        ? i
                        : found;
        }
        if (found == count) {
            uint64_t next = xfs_be32(fs->dirblock + XFS_DA3_FORW_OFF);
            if (next == 0) {
                return KERR_NOENT;
            }
            child = next;
            index++; /* the root names the leaves in the order `forw` links them */
            continue;
        }
        for (uint32_t i = found; i + 1u < count; i++) {
            xfs_copy_bytes(fs->dirblock + fs->da_hdr + i * XFS_DA3_ENTRY_SIZE,
                           fs->dirblock + fs->da_hdr + (i + 1u) * XFS_DA3_ENTRY_SIZE,
                           XFS_DA3_ENTRY_SIZE);
        }
        xfs_put_be16(fs->dirblock + fs->da_count, (uint16_t)(count - 1u));
        if (count == 1u && children > 1u) {
            return drop_empty_leaf(fs, dir, child, now_s);
        }
        xfs_reseal(fs, fs->dirblock, fs->dirblocksize, XFS_DA3_CRC_OFF);
        rc = xfs_dir_raw_store(fs, leaf_fsb);
        /* The mirror of the add path's rule: when the highest hash in the
         * leaf goes, the root's key for this leaf must come down to the new
         * highest. The checker requires the two to be equal, and this left
         * the old one behind. */
        if (rc == KERR_OK && found + 1u == count && count > 1u) {
            rc = node_set_for(fs, dir, child, top_hash(fs->dirblock + fs->da_hdr, count - 1u));
        }
        return rc;
    }
    return rc == KERR_OK ? KERR_IO : rc;
}

/* Finishes a removal from data block `which`, as the leaf format's does:
 * the hash out of its leaf, and the block either stored or — empty now —
 * given back, its slot in the free block emptied. */
static int node_after_removal(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                              uint32_t namelen, uint64_t fsb, uint32_t which, uint32_t at,
                              uint32_t blocks, uint64_t now_s) {
    bool empty = which != 0 && xfs_dir_block_is_empty(fs, fs->dirblock);
    uint32_t best = empty ? XFS_DIR2_NULL_DATAOFF : xfs_dir_largest_free(fs, fs->dirblock);
    int rc = empty ? KERR_OK : xfs_dir_raw_store(fs, fsb);
    rc = rc != KERR_OK ? rc
                       : leaf_remove(fs, dir, xfs_dir_hashname(name, namelen),
                                     xfs_dir_address_of(fs, which, at), now_s);
    rc = rc != KERR_OK ? rc : free_set(fs, dir, which, best, blocks);
    rc = rc != KERR_OK || !empty ? rc
                                 : xfs_dir_fork_remove(fs, dir, (uint64_t)which * fs->dirblocks,
                                                       now_s);
    return rc != KERR_OK ? rc : xfs_dir_touch(fs, dir, now_s);
}

int xfs_dir_node_remove(struct xfs *fs, const struct xfs_inode *dir, const char *name,
                        uint32_t namelen, uint64_t now_s, uint64_t *ino) {
    uint32_t blocks = xfs_dir_data_count(fs, dir);
    uint64_t fsb = 0;
    uint32_t at = 0, which;
    int rc = KERR_NOENT;
    for (which = 0; which < blocks; which++) {
        rc = xfs_dir_raw_load(fs, dir, (uint64_t)which * fs->dirblocks, &fsb);
        if (rc == KERR_NOENT) {
            continue; /* a block given back earlier: a hole */
        }
        if (rc != KERR_OK) {
            return rc;
        }
        rc = xfs_dir_take_out(fs, name, namelen, ino, &at);
        if (rc == KERR_OK) {
            break;
        }
    }
    if (rc != KERR_OK) {
        return rc;
    }
    return node_after_removal(fs, dir, name, namelen, fsb, which, at, blocks, now_s);
}
