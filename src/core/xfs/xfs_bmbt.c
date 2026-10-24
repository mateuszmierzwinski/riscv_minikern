/* The extent map, when it has outgrown the inode: `XFS_DINODE_FMT_BTREE`.
 *
 * **The biggest gap this reader had.** A file's extents live inside the
 * inode until they stop fitting; then XFS moves the map into a b-tree and
 * leaves only its root in the fork. That is not an exotic case — a
 * 5,000-entry *directory* is enough, so a filesystem that has been used for
 * a while is full of them, and until now every one of them was refused
 * outright: `inode 96 has a btree fork`, and the root directory of such a
 * filesystem could not be listed at all.
 *
 * The shape, which is two different shapes for the same tree:
 *
 *   - **The root, inside the inode.** A four-byte header (level, numrecs)
 *     followed by an array of keys and an array of pointers, each sized by
 *     how many fit in the *fork*, not by how many are used. So the pointers
 *     begin after `maxrecs` keys, and finding them means computing that.
 *   - **Every other block**, read from the disk: a 72-byte v5 header, then
 *     either records (at level 0) or the same keys-then-pointers pair, sized
 *     by how many fit in a filesystem *block*.
 *
 * A record is the same sixteen bytes as an inline extent, so the leaf scan
 * is the inline scan with a different source.
 *
 * It is a keyed descent rather than a walk of every leaf: the question asked
 * is always "which extent covers this block", a tree over a large file has
 * thousands of records, and answering it by reading all of them would make
 * every block of a large file cost the whole map.
 *
 * **Everything here is on-disk data from a filesystem the kernel did not
 * make**, so every number is checked before it is used: the magic, that a
 * level is below its parent's, that a count fits the block it came from,
 * that a pointer is inside the filesystem, and a hard cap on depth. A
 * corrupt tree must come back as `KERR_IO`, never as a loop or a read
 * somewhere else.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* How many key/pointer pairs fit in a space of `bytes`. */
static uint32_t pairs_in(uint32_t bytes) {
    return bytes / (XFS_BMBT_KEY_BYTES + XFS_BMBT_PTR_BYTES);
}

/* The child to descend into for `off`: the last key at or below it.
 *
 * Index 0 when `off` is below every key, which is a hole before the first
 * extent — the leaf scan then finds nothing covering it and says so. Taking
 * the first child rather than refusing keeps "a hole" and "a corrupt tree"
 * as different answers. */
static uint32_t pick_child(const uint8_t *keys, uint32_t nrecs, uint64_t off) {
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < nrecs; i++) {
        if (xfs_be64(keys + i * XFS_BMBT_KEY_BYTES) > off) {
            break;
        }
        chosen = i;
    }
    return chosen;
}

/* The record covering `off` among `nrecs` of them. */
static int scan_leaf(const struct xfs *fs, const uint8_t *recs, uint32_t nrecs, uint64_t off,
                     struct xfs_extent *out) {
    for (uint32_t i = 0; i < nrecs; i++) {
        struct xfs_extent e;
        xfs_extent_unpack(recs + i * XFS_EXTENT_BYTES, &e);
        if (e.blockcount == 0 || !xfs_fsb_valid(fs, e.startblock, e.blockcount)) {
            return KERR_IO;
        }
        if (off >= e.startoff && off - e.startoff < e.blockcount) {
            *out = e;
            return KERR_OK;
        }
    }
    return KERR_NOENT; /* a hole, or past the last extent */
}

/* Checks a block the tree pointed at, and reports its level and count.
 *
 * `below` is the level of the block that pointed here: a child must be
 * strictly lower, which is what makes the descent terminate even on a tree
 * that points back at itself.
 *
 * **Only the v5 magic is accepted.** `BMAP` is the pre-v5 block, whose
 * header is 24 bytes rather than 72 — and this reader mounts v5 filesystems
 * only, so such a block cannot be reached from one. Accepting it would mean
 * carrying a second header size for a case that does not arise, and reading
 * records at the wrong offset is a worse failure than refusing. */
static int check_node(const struct xfs *fs, const struct bbuf *b, uint32_t below, uint32_t max,
                      uint32_t *level, uint32_t *nrecs) {
    /* **The same three checks every other metadata block gets here**, done
     * by hand rather than through `xfs_meta_block` only because that takes a
     * mutable filesystem and this path is reached from a `const` one. A
     * b-tree block is as untrustworthy as a directory block, and the
     * consequence of believing a bad one is worse: it is a pointer to
     * follow, not a record to report. */
    if (xfs_be32(b->data) != xfs_magic_for(fs, XFS_BLK_BMBT) ||
        !xfs_crc_ok(fs, b->data, fs->blocksize, XFS_BMBT_CRC_OFF) ||
        !xfs_uuid_ok(fs, b->data + XFS_BMBT_UUID_OFF)) {
        return KERR_IO;
    }
    *level = xfs_be16(b->data + 4u);
    *nrecs = xfs_be16(b->data + 6u);
    if (*level >= below || *nrecs == 0 || *nrecs > max) {
        return KERR_IO;
    }
    return KERR_OK;
}

/* The pointer to follow out of a node block.
 *
 * The pointer array begins after **`max` keys**, not after the keys in use,
 * because the arrays are sized by what the block can hold. Getting that
 * wrong reads a key as a pointer and descends into the file's own data. */
static uint64_t child_pointer(const uint8_t *keys, uint32_t max, uint32_t nrecs, uint64_t off) {
    uint32_t at = pick_child(keys, nrecs, off);
    return xfs_be64(keys + max * XFS_BMBT_KEY_BYTES + at * XFS_BMBT_PTR_BYTES);
}

/* One step down: reads the block, checks it, and either answers from its
 * records or reports the pointer to follow next.
 *
 * **A record and a key/pointer pair are both sixteen bytes**, so one count
 * bounds either kind of block. That is a coincidence of the format rather
 * than a rule, and it is said here because the alternative is a reader that
 * looks like it confused them. */
static int step_down(const struct xfs *fs, uint64_t fsb, uint32_t below, uint64_t off,
                     struct xfs_extent *out, uint64_t *next) {
    struct bbuf *b;
    uint32_t level = 0, nrecs = 0;
    uint32_t max = (fs->blocksize - XFS_BMBT_BLOCK_BYTES) / XFS_EXTENT_BYTES;
    int rc;
    *next = 0;
    if (!xfs_fsb_valid(fs, fsb, 1)) {
        return KERR_IO;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = check_node(fs, b, below, max, &level, &nrecs);
    if (rc == KERR_OK && level == 0) {
        rc = scan_leaf(fs, b->data + XFS_BMBT_BLOCK_BYTES, nrecs, off, out);
    } else if (rc == KERR_OK) {
        *next = child_pointer(b->data + XFS_BMBT_BLOCK_BYTES, max, nrecs, off);
    }
    bcache_put(fs->cache, b);
    return rc;
}

/* The descent, over **whichever fork's root** it is given.
 *
 * Parameterised by the fork rather than reading the inode's data fork,
 * because an inode has two and both can be b-trees: a file with hundreds of
 * extended attributes has an attribute fork of exactly this shape, and the
 * walk is identical — same root header, same keys, same pointers, same
 * blocks. The two facades below name the two callers, which is what keeps
 * the call sites readable (Rule 5.1/5.2). */
int xfs_bmbt_lookup_in(const struct xfs *fs, const uint8_t *fork, uint32_t fork_len,
                       uint64_t off, struct xfs_extent *out) {
    uint32_t level = xfs_be16(fork);
    uint32_t nrecs = xfs_be16(fork + 2u);
    uint32_t mx = fork_len > XFS_BMDR_HDR_BYTES ? pairs_in(fork_len - XFS_BMDR_HDR_BYTES) : 0;
    const uint8_t *keys = fork + XFS_BMDR_HDR_BYTES;
    uint64_t fsb;
    uint32_t at;
    /* The root is a node by construction — a tree with a leaf for a root is
     * the extents format, which never reaches here — so level 0 is corrupt
     * rather than a one-block tree. */
    if (level == 0 || level > XFS_BMBT_MAX_LEVELS || nrecs == 0 || nrecs > mx) {
        return KERR_IO;
    }
    at = pick_child(keys, nrecs, off);
    fsb = xfs_be64(keys + mx * XFS_BMBT_KEY_BYTES + at * XFS_BMBT_PTR_BYTES);
    for (uint32_t depth = 0; depth <= XFS_BMBT_MAX_LEVELS; depth++) {
        uint64_t next = 0;
        int rc = step_down(fs, fsb, level, off, out, &next);
        if (rc != KERR_OK || next == 0) {
            return rc;
        }
        level--;
        fsb = next;
    }
    return KERR_IO; /* deeper than any real tree: refuse rather than loop */
}

int xfs_bmbt_lookup(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off,
                    struct xfs_extent *out) {
    return xfs_bmbt_lookup_in(fs, ip->fork, ip->fork_len, off, out);
}
