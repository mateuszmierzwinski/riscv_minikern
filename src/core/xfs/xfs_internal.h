/* xfs_internal.h — what the parts of the reader share: the big-endian
 * accessors, the block-number arithmetic, and the one function that gets a
 * metadata block and refuses to hand it over unverified. */
#ifndef MINIKERN_CORE_XFS_XFS_INTERNAL_H
#define MINIKERN_CORE_XFS_XFS_INTERNAL_H

#include "core/xfs/xfs.h"
#include "core/xfs/xfs_format.h"

static inline uint16_t xfs_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t xfs_be32(const uint8_t *p) {
    return ((uint32_t)xfs_be16(p) << 16) | xfs_be16(p + 2);
}

static inline uint64_t xfs_be64(const uint8_t *p) {
    return ((uint64_t)xfs_be32(p) << 32) | xfs_be32(p + 4);
}

/* The checksums are the one little-endian field in the format. */
static inline uint32_t xfs_le32(const uint8_t *p) {
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

/* A filesystem block number is (ag << agblklog) | agbno, and the group's
 * blocks are agblocks apart on the device — which is not the same thing,
 * because agblocks need not be a power of two. */
static inline uint64_t xfs_fsb_to_block(const struct xfs *fs, uint64_t fsb) {
    uint64_t mask = ((uint64_t)1 << fs->agblklog) - 1u;
    return (fsb >> fs->agblklog) * fs->agblocks + (fsb & mask);
}

static inline uint32_t xfs_fsb_ag(const struct xfs *fs, uint64_t fsb) {
    return (uint32_t)(fsb >> fs->agblklog);
}

/* True when fsb names a block inside the filesystem. */
bool xfs_fsb_valid(const struct xfs *fs, uint64_t fsb, uint64_t count);

/* Verifies the CRC32C stored little-endian at crc_off within len bytes,
 * with the field itself counted as zero (which is how it was computed). */
bool xfs_crc_ok(const uint8_t *buf, uint32_t len, uint32_t crc_off);

/* The filesystem's UUID at uuid, or not. */
bool xfs_uuid_ok(const struct xfs *fs, const uint8_t *uuid);

/* One verified metadata block, pinned: its magic, its CRC and its UUID all
 * checked. KERR_IO when any of them is wrong, KERR_INVAL for a block
 * number outside the filesystem. A second magic may be given (directories
 * have two), or 0 for none. */
int xfs_meta_block(struct xfs *fs, uint64_t fsb, uint32_t magic, uint32_t magic2, uint32_t crc_off,
                   uint32_t uuid_off, struct bbuf **out);

/* The clean-log check (xfs_log.c): KERR_OK when the log's newest record is
 * an unmount record, KERR_NOTSUP when it is anything else. */
int xfs_log_check(struct xfs *fs);

/* Where an inode lives: the device block holding its chunk, and its byte
 * offset within that block. The same arithmetic the reader uses, exposed
 * because a write has to put the inode back where it came from. */
void xfs_inode_place(const struct xfs *fs, uint64_t ino, uint64_t *device_block, uint32_t *boffset);

/* One inode in a transaction: which inode, where on the device its chunk
 * is and where in that chunk it sits — what recovery would need to find it
 * — and its on-disk core. */
struct xfs_log_inode_item {
    uint64_t ino;
    uint64_t chunk_block;
    uint32_t boffset;
    const uint8_t *core; /* the on-disk core, big-endian; converted on the way in */
};

#define XFS_LOG_MAX_ITEMS 2u

/* ── the group headers and the btrees (xfs_create.c) ───────────────────── */

/* The group's first block, where its four header sectors live. */
uint64_t xfs_group_block(const struct xfs *fs, uint32_t agno);

/* Recomputes a self-describing structure's checksum over len bytes. */
void xfs_reseal(uint8_t *at, uint32_t len, uint32_t crc_off);

/* The group's inode header, with its magic, checksum and UUID checked. */
int xfs_agi_get(struct xfs *fs, uint32_t agno, struct bbuf **out, uint8_t **agi);

/* One of a group's btree roots, which must be a lone leaf: KERR_NOTSUP for
 * a tree with branches, because walking one is not implemented. */
int xfs_tree_get(struct xfs *fs, uint32_t agno, uint32_t block, uint32_t magic, struct bbuf **out);

/* The inode-btree record whose chunk holds this group-relative inode, or
 * NULL. Found by range: a chunk does not begin at a multiple of its size. */
uint8_t *xfs_tree_record_for(const struct bbuf *b, uint32_t agino);

/* The superblock's free-inode counter, moved by delta. */
int xfs_count_inodes_free(struct xfs *fs, int32_t delta);

/* Big-endian writers, shared by everything that changes metadata. */
void xfs_put_be16(uint8_t *p, uint16_t v);
void xfs_put_be32(uint8_t *p, uint32_t v);
void xfs_put_be64(uint8_t *p, uint64_t v);

/* ── the free-space btrees (xfs_alloc.c) ───────────────────────────────── */

/* Takes one block from a group: `want`, if that block is free, and
 * otherwise the first block of the largest free extent. *got is the block
 * it took. KERR_NOSPC when the group has none, KERR_NOTSUP for a tree with
 * branches. */
int xfs_alloc_block(struct xfs *fs, uint32_t agno, uint32_t want, uint32_t *got);

/* Gives an extent back, joined to whatever it abuts. */
int xfs_free_extent(struct xfs *fs, uint32_t agno, uint32_t start, uint32_t count);

/* Writes these inodes to the log as one committed transaction, then an
 * unmount record after it so the log is clean again (xfs_logwrite.c).
 *
 * Several items rather than one because a change usually is several: a
 * created file is a new inode *and* its parent's directory, and the point
 * of a transaction is that they arrive together or not at all. */
int xfs_log_inodes(struct xfs *fs, const struct xfs_log_inode_item *items, uint32_t n);

/* One extent record, unpacked. */
struct xfs_extent {
    uint64_t startoff;   /* logical block within the file */
    uint64_t startblock; /* filesystem block */
    uint32_t blockcount;
    bool unwritten; /* allocated but never written: reads as zeroes */
};

/* Unpacks the sixteen-byte record at p. */
void xfs_extent_unpack(const uint8_t *p, struct xfs_extent *out);

/* Packs one back, which is the exact inverse (there is a round-trip test). */
void xfs_extent_pack(uint8_t *p, const struct xfs_extent *e);

/* The extent covering logical block off, or KERR_NOENT when off is in a
 * hole. Walks the inode's inline extent list. */
int xfs_bmap(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off, struct xfs_extent *out);

/* The same question for an inode whose fork is a b-tree: the extent
 * covering logical block `off`, by descending from the root in the fork.
 * KERR_NOENT for a hole, KERR_IO for a tree that does not hold together. */
int xfs_bmbt_lookup(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off,
                    struct xfs_extent *out);

#endif
