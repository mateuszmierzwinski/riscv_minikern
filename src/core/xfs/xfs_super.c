#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"
#include "lib/crc32c.h"

/* The superblock, the feature gate, and the shared block verifier. */

bool xfs_fsb_valid(const struct xfs *fs, uint64_t fsb, uint64_t count) {
    uint64_t block = xfs_fsb_to_block(fs, fsb);
    return count > 0 && xfs_fsb_ag(fs, fsb) < fs->agcount && block < fs->dblocks &&
           count <= fs->dblocks - block;
}

bool xfs_crc_ok(const uint8_t *buf, uint32_t len, uint32_t crc_off) {
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    uint32_t crc;
    if (crc_off + 4u > len) {
        return false;
    }
    crc = crc32c_update(CRC32C_SEED, buf, crc_off);
    crc = crc32c_update(crc, zeros, sizeof zeros);
    crc = crc32c_update(crc, buf + crc_off + 4u, len - crc_off - 4u);
    return ~crc == xfs_le32(buf + crc_off);
}

bool xfs_uuid_ok(const struct xfs *fs, const uint8_t *uuid) {
    for (uint32_t i = 0; i < 16u; i++) {
        if (uuid[i] != fs->uuid[i]) {
            return false;
        }
    }
    return true;
}

int xfs_meta_block(struct xfs *fs, uint64_t fsb, uint32_t magic, uint32_t magic2, uint32_t crc_off,
                   uint32_t uuid_off, struct bbuf **out) {
    struct bbuf *b;
    uint32_t found;
    int rc;
    if (!xfs_fsb_valid(fs, fsb, 1)) {
        return KERR_INVAL;
    }
    rc = bcache_get(fs->cache, xfs_fsb_to_block(fs, fsb), &b);
    if (rc != KERR_OK) {
        return rc;
    }
    found = xfs_be32(b->data);
    if ((found != magic && (magic2 == 0 || found != magic2)) || !xfs_crc_ok(b->data, fs->blocksize, crc_off) ||
        !xfs_uuid_ok(fs, b->data + uuid_off)) {
        bcache_put(fs->cache, b);
        return KERR_IO;
    }
    *out = b;
    return KERR_OK;
}

/* Reads the geometry out of the superblock image at sb. */
static void take_geometry(struct xfs *fs, const uint8_t *sb) {
    fs->blocksize = xfs_be32(sb + XFS_SB_BLOCKSIZE);
    fs->dblocks = xfs_be64(sb + XFS_SB_DBLOCKS);
    fs->logstart = xfs_be64(sb + XFS_SB_LOGSTART);
    fs->rootino = xfs_be64(sb + XFS_SB_ROOTINO);
    fs->agblocks = xfs_be32(sb + XFS_SB_AGBLOCKS);
    fs->agcount = xfs_be32(sb + XFS_SB_AGCOUNT);
    fs->logblocks = xfs_be32(sb + XFS_SB_LOGBLOCKS);
    fs->sectsize = xfs_be16(sb + XFS_SB_SECTSIZE);
    fs->inodesize = xfs_be16(sb + XFS_SB_INODESIZE);
    fs->inopblock = xfs_be16(sb + XFS_SB_INOPBLOCK);
    fs->blocklog = sb[XFS_SB_BLOCKLOG];
    fs->inopblog = sb[XFS_SB_INOPBLOG];
    fs->agblklog = sb[XFS_SB_AGBLKLOG];
    fs->dirblocks = 1u << sb[XFS_SB_DIRBLKLOG];
    fs->dir_leaf_block = XFS_DIR2_LEAF_SPACE_BYTES >> fs->blocklog;
}

/* The geometry has to be self-consistent before anything is read with it:
 * a bad block size or inode count would turn every later calculation into
 * a wild address. */
static int check_geometry(const struct xfs *fs) {
    bool sane = fs->blocksize >= 512u && fs->blocksize <= 65536u &&
                fs->blocksize == (1u << fs->blocklog) && fs->sectsize >= 512u &&
                fs->inopblock == fs->blocksize / fs->inodesize &&
                fs->inopblock == (1u << fs->inopblog) && fs->agcount > 0 && fs->agblocks > 0 &&
                fs->agblocks <= ((uint64_t)1 << fs->agblklog) && fs->dblocks > 0 &&
                fs->dblocks <= (uint64_t)fs->agcount * fs->agblocks;
    if (!sane) {
        kprintf("xfs: the superblock's geometry does not add up\n");
        return KERR_INVAL;
    }
    if (fs->inodesize > XFS_INODE_MAX || (fs->inodesize != 256u && fs->inodesize != 512u)) {
        kprintf("xfs: %u-byte inodes are not supported (256 or 512)\n", fs->inodesize);
        return KERR_NOTSUP;
    }
    if (fs->sectsize > fs->cache->dev->block_size || (fs->sectsize & (fs->sectsize - 1u)) != 0 ||
        fs->sectsize > fs->blocksize) {
        kprintf("xfs: a %u-byte sector does not fit this device's blocks\n", fs->sectsize);
        return KERR_NOTSUP;
    }
    if (fs->dirblocks != 1u) {
        kprintf("xfs: multi-block directory blocks are not supported\n");
        return KERR_NOTSUP;
    }
    return KERR_OK;
}

/* Refuses every feature this reader does not fully understand — except the
 * ones the **format itself** says a read-only mount may ignore.
 *
 * That exception is not a relaxation of the safety argument; it is what the
 * `ro_compat` field means. XFS divides its feature flags into three sets,
 * and the compatibility each promises is different: an `incompat` flag
 * changes the structures a reader must understand, a `log_incompat` flag the
 * log's contents, and an `ro_compat` flag only the structures a *writer*
 * must maintain. Reverse mapping and reflink are both of the last kind: the
 * reverse-map tree is an auxiliary index that reading never consults, and a
 * shared extent is read exactly like an unshared one — sharing only
 * constrains what writing may do to it.
 *
 * Refusing them outright made this reader usable only on a filesystem
 * formatted specially for it. The first card here was made with
 * `reflink=0,rmapbt=0` and mounted; a drive formatted with `mkfs.xfs`'s
 * **defaults**, which turn both on, was refused — and "your filesystem must
 * be built for my kernel" is not a filesystem reader.
 *
 * The obligation that comes with it is enforced rather than assumed: a
 * filesystem carrying `ro_compat` flags this reader does not implement is
 * marked unwritable, so every write path refuses it (`xfs_writable`). The
 * `incompat` and `log_incompat` sets stay a strict whitelist. */
static int check_features(struct xfs *fs, const uint8_t *sb) {
    uint32_t ro = xfs_be32(sb + XFS_SB_FEAT_RO_COMPAT);
    uint32_t in = xfs_be32(sb + XFS_SB_FEAT_INCOMPAT);
    uint32_t log = xfs_be32(sb + XFS_SB_FEAT_LOG_INCOMPAT);
    if ((ro & ~XFS_FEAT_RO_SUPPORTED) != 0) {
        kprintf("xfs: read-only features %08x not implemented here: no writing\n",
                ro & ~XFS_FEAT_RO_SUPPORTED);
        fs->write_refused = true;
    }
    fs->big_extent_counts = (in & XFS_FEAT_IN_NREXT64) != 0;
    if ((in & ~XFS_FEAT_IN_SUPPORTED) != 0) {
        kprintf("xfs: unsupported features %08x (sparse inodes, nrext64 or needsrepair: refused)\n",
                in & ~XFS_FEAT_IN_SUPPORTED);
        return KERR_NOTSUP;
    }
    if (log != 0) {
        kprintf("xfs: unsupported log features %08x\n", log);
        return KERR_NOTSUP;
    }
    return KERR_OK;
}

/* Checks the superblock image is a v5 one we may use. */
static int check_super(struct xfs *fs, const uint8_t *sb) {
    uint16_t version;
    if (xfs_be32(sb + 0) != XFS_SB_MAGIC) {
        kprintf("xfs: not an XFS filesystem (magic %08x)\n", xfs_be32(sb + 0));
        return KERR_INVAL;
    }
    version = xfs_be16(sb + XFS_SB_VERSIONNUM);
    if ((version & XFS_SB_VERSION_MASK) != XFS_SB_VERSION_5) {
        kprintf("xfs: version %u is not supported (v5 only)\n", version & XFS_SB_VERSION_MASK);
        return KERR_NOTSUP;
    }
    if (sb[XFS_SB_INPROGRESS] != 0) {
        kprintf("xfs: mkfs did not finish with this filesystem\n");
        return KERR_INVAL;
    }
    return check_features(fs, sb);
}

/* The superblock's checksum covers its whole **sector**, not the fields
 * this reader knows about — measured against a real filesystem, where 264
 * bytes gave the wrong answer and 512 the right one (XFS.md §2). The
 * sector therefore has to lie inside the one cached block it was read in,
 * which check_geometry has already established. */
static bool super_crc_ok(const struct xfs *fs, const uint8_t *sb) {
    return xfs_crc_ok(sb, fs->sectsize, XFS_SB_CRC);
}

/* The UUID metadata blocks carry: sb_meta_uuid when the filesystem says
 * its metadata uses a different one, sb_uuid otherwise. */
static void take_uuid(struct xfs *fs, const uint8_t *sb) {
    uint32_t in = xfs_be32(sb + XFS_SB_FEAT_INCOMPAT);
    const uint8_t *from = (in & XFS_FEAT_IN_META_UUID) != 0 ? sb + XFS_SB_META_UUID : sb + XFS_SB_UUID;
    for (uint32_t i = 0; i < 16u; i++) {
        fs->uuid[i] = from[i];
    }
    fs->ftype = (in & XFS_FEAT_IN_FTYPE) != 0;
}

/* Reads block 0 and takes everything the geometry needs from it. The block
 * stays pinned for as long as the superblock is being read out of it. */
static int mount_from_super(struct xfs *fs, const uint8_t *sb) {
    int rc = check_super(fs, sb);
    if (rc != KERR_OK) {
        return rc;
    }
    take_geometry(fs, sb);
    take_uuid(fs, sb);
    rc = check_geometry(fs);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!super_crc_ok(fs, sb)) {
        kprintf("xfs: the superblock's checksum is wrong\n");
        return KERR_IO;
    }
    return KERR_OK;
}

int xfs_mount(struct xfs *fs, struct bcache *cache) {
    struct bbuf *b;
    int rc;
    *fs = (struct xfs){.cache = cache};
    rc = bcache_get(cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = mount_from_super(fs, b->data);
    bcache_put(cache, b);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_log_check(fs);
    if (rc != KERR_OK) {
        return rc;
    }
    fs->mounted = true;
    kprintf("xfs: %u blocks of %u bytes, %u groups, %u-byte inodes, root %u\n", (uint32_t)fs->dblocks,
            fs->blocksize, fs->agcount, fs->inodesize, (uint32_t)fs->rootino);
    return KERR_OK;
}
