#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

/* Inodes: where one is, what it says, and which block holds a given part
 * of its file. */

void xfs_extent_unpack(const uint8_t *p, struct xfs_extent *out) {
    uint64_t hi = xfs_be64(p);
    uint64_t lo = xfs_be64(p + 8);
    out->unwritten = (hi >> 63) != 0;
    out->startoff = (hi >> 9) & (((uint64_t)1 << 54) - 1u);
    out->startblock = ((hi & 0x1ffu) << 43) | (lo >> 21);
    out->blockcount = (uint32_t)(lo & 0x1fffffu);
}

/* An inode number encodes where the inode is: which group, which block of
 * it, and which slot in that block. */
static void locate(const struct xfs *fs, uint64_t ino, uint64_t *fsb, uint32_t *slot) {
    uint64_t agbno_mask = ((uint64_t)1 << fs->agblklog) - 1u;
    uint64_t agno = ino >> (fs->agblklog + fs->inopblog);
    uint64_t agbno = (ino >> fs->inopblog) & agbno_mask;
    *fsb = (agno << fs->agblklog) | agbno;
    *slot = (uint32_t)(ino & (((uint64_t)1 << fs->inopblog) - 1u));
}

/* The inverse of xfs_extent_unpack: the same four fields back into their
 * sixteen packed bytes. Written as a bit loop rather than as shifts across
 * byte boundaries, because the boundaries are where the fields do not fall
 * and a shift that is one bit out survives most tests. */
void xfs_extent_pack(uint8_t *p, const struct xfs_extent *e) {
    uint8_t bits[128];
    for (uint32_t i = 0; i < 128; i++) {
        bits[i] = 0;
    }
    bits[127] = e->unwritten ? 1u : 0u;
    for (uint32_t i = 0; i < 54u; i++) {
        bits[73u + i] = (uint8_t)((e->startoff >> i) & 1u);
    }
    for (uint32_t i = 0; i < 52u; i++) {
        bits[21u + i] = (uint8_t)((e->startblock >> i) & 1u);
    }
    for (uint32_t i = 0; i < 21u; i++) {
        bits[i] = (uint8_t)((e->blockcount >> i) & 1u);
    }
    for (uint32_t byte = 0; byte < 16u; byte++) {
        uint8_t v = 0;
        for (uint32_t bit = 0; bit < 8u; bit++) {
            /* The record is big-endian: bit 127 is the top bit of byte 0. */
            v |= (uint8_t)(bits[127u - (byte * 8u + bit)] << (7u - bit));
        }
        p[byte] = v;
    }
}

void xfs_inode_place(const struct xfs *fs, uint64_t ino, uint64_t *device_block, uint32_t *boffset) {
    uint64_t fsb;
    uint32_t slot;
    locate(fs, ino, &fsb, &slot);
    *device_block = xfs_fsb_to_block(fs, fsb);
    *boffset = slot * fs->inodesize;
}

/* The inline fork's extent: it ends where the attribute fork begins, or at
 * the end of the inode when there is no attribute fork. */
static uint32_t fork_bytes(const struct xfs *fs, const uint8_t *di) {
    uint32_t forkoff = di[XFS_DI_FORKOFF];
    uint32_t area = fs->inodesize - XFS_DI_V3_SIZE;
    return forkoff == 0 ? area : forkoff * 8u;
}

/* Everything that must be true of an inode before its contents are used:
 * it says it is an inode, in the layout we read, it knows its own number,
 * and its checksum and UUID tie it to this filesystem. */
static int verify(struct xfs *fs, const uint8_t *di, uint64_t ino) {
    if (xfs_be16(di + XFS_DI_MAGIC) != XFS_DINODE_MAGIC) {
        return KERR_IO;
    }
    if (di[XFS_DI_VERSION] != XFS_DI_VERSION_3 || xfs_be64(di + XFS_DI_INO) != ino) {
        return KERR_IO;
    }
    if (!xfs_crc_ok(di, fs->inodesize, XFS_DI_CRC) || !xfs_uuid_ok(fs, di + XFS_DI_UUID)) {
        return KERR_IO;
    }
    return KERR_OK;
}

/* Copies the core fields and the inline fork out of a verified inode. */
static int take(struct xfs *fs, const uint8_t *di, uint64_t ino, struct xfs_inode *out) {
    uint32_t avail = fork_bytes(fs, di);
    /* **The inode says, not the superblock.** The superblock's `NREXT64`
     * permits the layout; each inode's `di_flags2` says whether it actually
     * uses it, so a filesystem that gained the feature later still has
     * inodes in the old shape and both must read correctly. */
    bool big = (xfs_be64(di + XFS_DI_FLAGS2) & XFS_DIFLAG2_NREXT64) != 0;
    *out = (struct xfs_inode){
        .ino = ino,
        .size = xfs_be64(di + XFS_DI_SIZE),
        .nblocks = xfs_be64(di + XFS_DI_NBLOCKS),
        .mode = xfs_be16(di + XFS_DI_MODE),
        .uid = xfs_be32(di + XFS_DI_UID),
        .gid = xfs_be32(di + XFS_DI_GID),
        .nlink = xfs_be32(di + XFS_DI_NLINK),
        .nextents = big ? (uint32_t)xfs_be64(di + XFS_DI_BIG_NEXTENTS)
                        : xfs_be32(di + XFS_DI_NEXTENTS),
        .big_extent_counts = big,
        .format = di[XFS_DI_FORMAT],
        .aformat = di[XFS_DI_AFORMAT],
        .forkoff = di[XFS_DI_FORKOFF],
        .fork_len = avail < XFS_FORK_MAX ? avail : XFS_FORK_MAX,
    };
    if (out->format == XFS_DINODE_FMT_BTREE && out->fork_len <= XFS_BMDR_HDR_BYTES) {
        return KERR_IO; /* a btree root with no room for even one pointer */
    }
    if (out->format == XFS_DINODE_FMT_LOCAL && out->size > out->fork_len) {
        return KERR_IO; /* says it holds more than fits inside it */
    }
    if (out->format == XFS_DINODE_FMT_EXTENTS &&
        (uint64_t)out->nextents * XFS_EXTENT_BYTES > out->fork_len) {
        return KERR_IO; /* more extents than the inode has room for */
    }
    for (uint32_t i = 0; i < out->fork_len; i++) {
        out->fork[i] = di[XFS_DI_V3_SIZE + i];
    }
    return KERR_OK;
}

int xfs_inode_read(struct xfs *fs, uint64_t ino, struct xfs_inode *out) {
    struct bbuf *b;
    uint64_t fsb;
    uint32_t slot;
    int rc;
    locate(fs, ino, &fsb, &slot);
    if (!xfs_fsb_valid(fs, fsb, 1) || slot >= fs->inopblock) {
        return KERR_NOENT;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = verify(fs, b->data + slot * fs->inodesize, ino);
    if (rc == KERR_OK) {
        rc = take(fs, b->data + slot * fs->inodesize, ino, out);
    }
    bcache_put(fs->cache, b);
    return rc;
}

int xfs_bmap(const struct xfs *fs, const struct xfs_inode *ip, uint64_t off, struct xfs_extent *out) {
    /* **The one place the two shapes of extent map meet.** Everything that
     * asks where a block is — reading a file, listing a directory, following
     * a symlink — comes through here, so a fork in a b-tree is handled once
     * rather than at each caller. */
    if (ip->format == XFS_DINODE_FMT_BTREE) {
        return xfs_bmbt_lookup(fs, ip, off, out);
    }
    if (ip->format != XFS_DINODE_FMT_EXTENTS) {
        return KERR_NOTSUP;
    }
    for (uint32_t i = 0; i < ip->nextents; i++) {
        struct xfs_extent e;
        xfs_extent_unpack(ip->fork + i * XFS_EXTENT_BYTES, &e);
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

bool xfs_is_dir(const struct xfs_inode *ip) {
    return (ip->mode & 0170000u) == 0040000u;
}

bool xfs_is_reg(const struct xfs_inode *ip) {
    return (ip->mode & 0170000u) == 0100000u;
}

bool xfs_is_link(const struct xfs_inode *ip) {
    return (ip->mode & 0170000u) == 0120000u;
}
