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

bool xfs_crc_ok(const struct xfs *fs, const uint8_t *buf, uint32_t len, uint32_t crc_off) {
    static const uint8_t zeros[4] = {0, 0, 0, 0};
    uint32_t crc;
    if (!fs->v5) {
        return true; /* there is no checksum here to disagree with */
    }
    if (crc_off + 4u > len) {
        return false;
    }
    crc = crc32c_update(CRC32C_SEED, buf, crc_off);
    crc = crc32c_update(crc, zeros, sizeof zeros);
    crc = crc32c_update(crc, buf + crc_off + 4u, len - crc_off - 4u);
    return ~crc == xfs_le32(buf + crc_off);
}

bool xfs_uuid_ok(const struct xfs *fs, const uint8_t *uuid) {
    if (!fs->v5) {
        /* **A v4 metadata block has no UUID**, for the same reason it has
         * no checksum: both arrived with v5. The bytes at `uuid_off` are
         * whatever the shorter structure put there — comparing them would
         * reject every block of a sound filesystem. */
        return true;
    }
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
    if ((found != magic && (magic2 == 0 || found != magic2)) ||
        !xfs_crc_ok(fs, b->data, fs->blocksize, crc_off) ||
        !xfs_uuid_ok(fs, b->data + uuid_off)) {
        /* **Which of the three failed, and what was actually there.** A
         * bare `KERR_IO` from here turns every format question into a
         * bisection: a wrong magic says the wrong kind of block was
         * addressed, a wrong checksum says the right block is damaged, and
         * the two want opposite investigations. */
        kprintf("xfs: block %llu is not the %08x expected: magic %08x, crc %s, uuid %s\n",
                (unsigned long long)fsb, magic, found,
                xfs_crc_ok(fs, b->data, fs->blocksize, crc_off) ? "ok" : "bad",
                xfs_uuid_ok(fs, b->data + uuid_off) ? "ok" : "bad");
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
    fs->dirblocksize = fs->dirblocks * fs->blocksize;
    fs->dir_leaf_block = XFS_DIR2_LEAF_SPACE_BYTES >> fs->blocklog;
    fs->dir_free_block = XFS_DIR2_FREE_SPACE_BYTES >> fs->blocklog;
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
    /* **Every inode size XFS allows is accepted**: 256, 512, 1024 or 2048.
     *
     * The first three of those were refused outright, which made a
     * filesystem formatted with `-i size=1024` unmountable — and the
     * refusal was not about the inode size at all, it was about the fork
     * cache. `struct xfs_inode` carries a copy of the inode's literal area,
     * and that copy is `XFS_FORK_MAX` bytes: enough for a 512-byte inode.
     * A 2048-byte one has 1872 bytes of literal area, and an inode struct
     * big enough to hold it would be two kilobytes — past the kernel's
     * 1 KiB frame limit for a *single* one on a stack, let alone the three
     * functions that hold two.
     *
     * So the filesystem mounts and every inode whose fork fits the cache
     * works normally, which is all of them in practice: 336 bytes is a
     * shortform directory of a dozen names or a file of twenty-one
     * extents. An inode that genuinely needs more is refused **on its own**
     * by `xfs_inode_read`, with its number, rather than taking the whole
     * filesystem down with it.
     *
     * That is a partial and is recorded as one. Lifting it means reading
     * forks on demand instead of caching them — about twenty call sites
     * across the write path — and that is a refactor of code this board
     * has proved, not a line to slip in beside a validation check. */
    if (fs->inodesize < 256u || fs->inodesize > 2048u ||
        (fs->inodesize & (fs->inodesize - 1u)) != 0) {
        kprintf("xfs: %u-byte inodes are not a size XFS makes (256..2048)\n", fs->inodesize);
        return KERR_NOTSUP;
    }
    if (fs->sectsize > fs->cache->dev->block_size || (fs->sectsize & (fs->sectsize - 1u)) != 0 ||
        fs->sectsize > fs->blocksize) {
        kprintf("xfs: a %u-byte sector does not fit this device's blocks\n", fs->sectsize);
        return KERR_NOTSUP;
    }
    /* A directory block of several filesystem blocks is read by gathering
     * it (`xfs_dir.c`); what cannot be read is one larger than the buffer,
     * and the buffer is the largest the format allows. A shift that
     * overflows it is a corrupt superblock rather than an exotic geometry. */
    if (fs->dirblocksize > XFS_DIRBLK_MAX || fs->dirblocksize < fs->blocksize) {
        kprintf("xfs: a %u-byte directory block is not a geometry this reads\n",
                fs->dirblocksize);
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
/* v4's features, which live in `versionnum` and `features2` rather than in
 * the three words v5 added.
 *
 * **The v5 words must not be read here at all.** They sit at offsets 208 to
 * 220, which on a v4 superblock is past the end of the structure as v4
 * defined it — whatever is there was never a feature word. On an image
 * `mkfs` wrote it happens to be zero, which is precisely the kind of luck
 * that makes a wrong read work until it does not. */
static int check_features_v4(struct xfs *fs, const uint8_t *sb) {
    uint16_t version = xfs_be16(sb + XFS_SB_VERSIONNUM);
    uint32_t f2 = (version & XFS_SB_V4_MOREBITS) != 0 ? xfs_be32(sb + XFS_SB_FEATURES2) : 0;
    fs->ftype = (f2 & XFS_SB_V2_FTYPE) != 0;
    fs->big_extent_counts = false; /* `nrext64` is v5's */
    fs->has_finobt = false;        /* so is the free-inode b-tree */
    /* **`attr2` is a write capability, not a read one.** With it the
     * attribute fork's offset inside the inode may move as the fork grows;
     * without it `mkfs` fixed that offset once and nothing may change it.
     * Reading either is the same work, so an `attr1` filesystem is read
     * normally — but writing an attribute to one would have to move a fork
     * that is not allowed to move, so that is refused rather than done. */
    fs->attr_fixed_fork = (f2 & XFS_SB_V2_ATTR2) == 0;
    if (fs->attr_fixed_fork) {
        kprintf("xfs: this filesystem predates attr2; attributes are read-only\n");
    }
    return KERR_OK;
}

static int check_features(struct xfs *fs, const uint8_t *sb) {
    uint32_t ro, in, log;
    if (!fs->v5) {
        return check_features_v4(fs, sb);
    }
    ro = xfs_be32(sb + XFS_SB_FEAT_RO_COMPAT);
    in = xfs_be32(sb + XFS_SB_FEAT_INCOMPAT);
    log = xfs_be32(sb + XFS_SB_FEAT_LOG_INCOMPAT);
    fs->has_finobt = (ro & XFS_FEAT_RO_FINOBT) != 0;
    fs->attr_fixed_fork = false; /* every v5 filesystem has attr2 */
    /* **One owner per field.** The file-type feature is in the incompat
     * word on v5 and in `features2` on v4, so both branches of *this*
     * function set it and nothing else does.
     *
     * It used to be set in `take_uuid`, which runs later — so on v4 that
     * function, named for something else entirely, quietly overwrote what
     * the v4 branch had worked out, and `fs->ftype` came back false on a
     * filesystem that has the byte. The cost was not cosmetic: a shortform
     * entry is a byte shorter without it, so every entry after the first
     * was read at the wrong offset and the root directory became
     * unreadable the moment anything was added to it. This is the fourth
     * field in this tree to be set and then reset by a step that did not
     * know it was set; one owner per field is the only thing that has
     * stopped it recurring. */
    fs->ftype = (in & XFS_FEAT_IN_FTYPE) != 0;
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

/* Checks the superblock image is one we may use, and records which
 * format it is: v4 and v5 are both read. */
static int check_super(struct xfs *fs, const uint8_t *sb) {
    uint16_t version;
    if (xfs_be32(sb + 0) != XFS_SB_MAGIC) {
        kprintf("xfs: not an XFS filesystem (magic %08x)\n", xfs_be32(sb + 0));
        return KERR_INVAL;
    }
    version = xfs_be16(sb + XFS_SB_VERSIONNUM);
    /* **Both formats, and the version decides a handful of sizes.** A v4
     * filesystem is the same filesystem with shorter headers and no
     * checksums; everything downstream takes those from `fs` rather than
     * from a constant, so this is the only place that has to know. */
    switch (version & XFS_SB_VERSION_MASK) {
    case XFS_SB_VERSION_5:
        fs->v5 = true;
        fs->dinode_size = XFS_DI_V3_SIZE;
        fs->dir_data_hdr = XFS_DIR3_DATA_HDR_SIZE;
        fs->da_hdr = XFS_DA3_HDR_SIZE;
        fs->dir_free_hdr = XFS_DIR3_FREE_HDR_SIZE;
        fs->btree_hdr = XFS_BTREE_RECORDS;
        fs->da_count = XFS_DA3_COUNT_OFF;
        fs->da_level = XFS_DA3_LEVEL_OFF;
        fs->attr_base = ATTR_LEAF_COUNT;
        fs->attr_hdr = ATTR_LEAF_HDR;
        break;
    case XFS_SB_VERSION_4:
        if ((version & XFS_SB_V4_DIRV2) == 0) {
            kprintf("xfs: this v4 filesystem has v1 directories, which are a "
                    "different format\n");
            return KERR_NOTSUP;
        }
        if ((version & XFS_SB_V4_SHARED) != 0) {
            /* Not refused: readable is useful and correct. Writing is what
             * another host's cached metadata makes unsafe. */
            fs->write_refused = true;
            kprintf("xfs: this filesystem is shared read-only; mounting read-only\n");
        }
        fs->v5 = false;
        fs->dinode_size = XFS_DI_V2_SIZE;
        fs->dir_data_hdr = XFS_DIR2_DATA_HDR_SIZE;
        fs->da_hdr = XFS_DA_HDR_SIZE;
        fs->dir_free_hdr = XFS_DIR2_FREE_HDR_SIZE;
        fs->btree_hdr = XFS_BTREE_V4_RECORDS;
        fs->da_count = XFS_DA_COUNT_OFF;
        fs->da_level = XFS_DA_LEVEL_OFF;
        fs->attr_base = ATTR_LEAF_V4_BASE;
        fs->attr_hdr = ATTR_LEAF_V4_HDR;
        break;
    default:
        kprintf("xfs: version %u is not supported (v4 and v5 are)\n",
                version & XFS_SB_VERSION_MASK);
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
    return xfs_crc_ok(fs, sb, fs->sectsize, XFS_SB_CRC);
}

/* The UUID metadata blocks carry: sb_meta_uuid when the filesystem says
 * its metadata uses a different one, sb_uuid otherwise. */
static void take_uuid(struct xfs *fs, const uint8_t *sb) {
    uint32_t in = xfs_be32(sb + XFS_SB_FEAT_INCOMPAT);
    const uint8_t *from = (in & XFS_FEAT_IN_META_UUID) != 0 ? sb + XFS_SB_META_UUID : sb + XFS_SB_UUID;
    for (uint32_t i = 0; i < 16u; i++) {
        fs->uuid[i] = from[i];
    }
}

/* Copies the filesystem's label out of the superblock.
 *
 * Twelve bytes, and **not NUL-terminated when all twelve are used** — so it
 * is copied into a thirteenth byte of its own rather than pointed at, which
 * is the difference between a label and a label followed by whatever the
 * superblock keeps at offset 120. */
static void take_label(struct xfs *fs, const uint8_t *sb) {
    uint32_t n = 0;
    while (n < XFS_SB_LABEL_MAX && sb[XFS_SB_FNAME + n] != '\0') {
        fs->label[n] = (char)sb[XFS_SB_FNAME + n];
        n++;
    }
    fs->label[n] = '\0';
}

/* The one copy of the directory scratch, handed to every mount.
 *
 * See `struct xfs`: it is live only inside a directory operation, and this
 * kernel holds one lock around all of them. */
static uint8_t dir_scratch[XFS_DIRBLK_MAX];
static uint8_t pair_scratch[XFS_DIRPAIRS_BYTES];

/* Reads block 0 and takes everything the geometry needs from it. The block
 * stays pinned for as long as the superblock is being read out of it. */
static int mount_from_super(struct xfs *fs, const uint8_t *sb) {
    int rc = check_super(fs, sb);
    if (rc != KERR_OK) {
        return rc;
    }
    take_geometry(fs, sb);
    take_uuid(fs, sb);
    take_label(fs, sb);
    fs->rblocks = xfs_be64(sb + XFS_SB_RBLOCKS);
    fs->rextents = xfs_be64(sb + XFS_SB_REXTENTS);
    fs->rbmino = xfs_be64(sb + XFS_SB_RBMINO);
    fs->rsumino = xfs_be64(sb + XFS_SB_RSUMINO);
    fs->rextsize = xfs_be32(sb + XFS_SB_REXTSIZE);
    fs->uquotino = xfs_be64(sb + XFS_SB_UQUOTINO);
    fs->gquotino = xfs_be64(sb + XFS_SB_GQUOTINO);
    fs->pquotino = xfs_be64(sb + XFS_SB_PQUOTINO);
    fs->qflags = (uint16_t)xfs_be16(sb + XFS_SB_QFLAGS);
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

/* Gives the filesystem the device its external log is on, and re-asks the
 * question the mount could not answer without it.
 *
 * The check and the replay are the same ones the mount runs; the only thing
 * that has changed is that the log can now be read. So this is the mount's
 * last step, arriving late — and if the log turns out clean, or replays,
 * the read-only restriction it imposed is lifted. */
int xfs_attach_rt(struct xfs *fs, struct bcache *rt) {
    if (fs == NULL || rt == NULL || !fs->mounted) {
        return KERR_INVAL;
    }
    if (fs->rblocks == 0) {
        return KERR_NOTSUP; /* this filesystem has no realtime subvolume */
    }
    /* **No check, and nothing becomes writable.** Unlike the log, the
     * realtime device carries file data and no metadata: there is nothing
     * on it to verify and nothing about the filesystem's consistency that
     * depends on it. Attaching it makes realtime files readable and
     * changes nothing else. */
    fs->rt_cache = rt;
    kprintf("xfs: the realtime device is attached, %llu extents of %u block(s)\n",
            (unsigned long long)fs->rextents, fs->rextsize);
    return KERR_OK;
}

int xfs_attach_log(struct xfs *fs, struct bcache *log) {
    int rc;
    if (fs == NULL || log == NULL || !fs->mounted) {
        return KERR_INVAL;
    }
    if (fs->logstart != 0) {
        return KERR_NOTSUP; /* this filesystem's log is inside it */
    }
    fs->log_cache = log;
    fs->log_base = 0; /* an external log starts at its device's first byte */
    fs->log_detached = false;
    rc = xfs_log_check(fs);
    if (rc == KERR_DIRTY) {
        rc = xfs_check(fs);
        rc = rc != KERR_OK ? rc : (fs->no_replay ? KERR_NOTSUP : xfs_log_replay(fs));
        if (rc != KERR_OK) {
            kprintf("xfs: the external log is dirty and could not be replayed\n");
            return rc;
        }
        fs->log_replayed = true;
    } else if (rc != KERR_OK) {
        return rc;
    }
    fs->write_refused = false;
    kprintf("xfs: the external log is attached and %s; the filesystem is writable\n",
            fs->log_replayed ? "was replayed" : "clean");
    return KERR_OK;
}

int xfs_mount(struct xfs *fs, struct bcache *cache) {
    struct bbuf *b;
    int rc;
    /* **Policy set by the caller survives the reset; what the filesystem
     * knows about itself does not.**
     *
     * The reset is here so that a re-mount cannot inherit stale geometry
     * from the last one — and it wiped these two in the same breath, which
     * is how `no_replay` came to be set by a test and ignored by the mount
     * it was set for. That happened three times over in one feature, in
     * three different places, each one looking like a different bug: the
     * caller zeroed the struct, this zeroed it again, and the one knob that
     * had to cross the boundary was zeroed with it. A reset that is
     * deliberate about what it keeps is the only way that stops recurring. */
    bool keep_no_replay = fs->no_replay;
    bool keep_skip_unmount = fs->skip_unmount_record;
    *fs = (struct xfs){.cache = cache};
    fs->no_replay = keep_no_replay;
    fs->skip_unmount_record = keep_skip_unmount;
    rc = bcache_get(cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    fs->dirblock = dir_scratch;
    fs->dirpairs = pair_scratch;
    rc = mount_from_super(fs, b->data);
    bcache_put(cache, b);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **A dirty log is checked, not refused.** A filesystem whose log holds
     * uncommitted work has metadata that is behind the log, which is a
     * reason not to *write* to it and not in itself a reason to refuse it:
     * the data and the metadata that were already committed are intact, and
     * a machine that cannot read its own disk after a power cut is worse
     * than one that reads it carefully. So the structures this reader walks
     * are checked first, and the mount is read-only if they are sound.
     *
     * This is what Linux's `norecovery` does, and the rule it follows is
     * the one `MADV_DONTNEED` taught this project: a promise about what the
     * caller will observe is the specification. A reader may observe a
     * filesystem as of its last commit; nobody may write over a log that
     * says something newer happened. */
    /* **Where the log is, decided once.** An internal log is a run inside
     * this filesystem, so its device is this one and its base is
     * `logstart` — which is AG-encoded and goes through `xfs_fsb_to_block`
     * like any other filesystem block, a conversion the read path once
     * missed while the write path did it (`CLAUDE.md` §5). An external log
     * has `sb_logstart` of zero and lives on a device nothing here has been
     * given yet. */
    if (fs->logstart != 0) {
        fs->log_cache = cache;
        fs->log_base = xfs_fsb_to_block(fs, fs->logstart) * (uint64_t)fs->blocksize;
    } else {
        fs->log_cache = NULL;
        fs->log_base = 0;
        fs->log_detached = true;
    }
    rc = xfs_log_check(fs);
    /* **A log that cannot be reached is not a log that is dirty.** There is
     * nothing to replay and nothing to check a replay against, so the
     * filesystem mounts read-only and waits for its device: everything
     * committed before the last unmount is intact and readable, and writing
     * without being able to read the log would be worse than refusing or
     * waiting.
     *
     * This block was first written into `xfs_attach_log` by a textual edit
     * that matched the wrong one of two identical lines — where it is
     * unreachable, because attaching is what makes the device present. The
     * test went on failing and the code looked right in isolation, which is
     * the whole hazard of editing by pattern in a file with two copies of a
     * call. */
    if (rc == KERR_NODEV) {
        fs->write_refused = true;
        kprintf("xfs: mounting read-only until the log's device is attached\n");
        rc = KERR_OK;
    } else if (rc == KERR_DIRTY) {
        fs->log_was_dirty = true;
        rc = xfs_check(fs);
        if (rc != KERR_OK) {
            kprintf("xfs: the log is dirty and the check found faults: refusing to mount\n");
            return rc;
        }
        /* **Checked first, then replayed.** Replaying into metadata that is
         * already broken multiplies the damage, so the sound-enough
         * verdict is a precondition and not a nicety.
         *
         * A replay that cannot finish is **not** a failure to mount: it
         * leaves the filesystem exactly as the log found it and the mount
         * falls back to read-only, which is where this filesystem was
         * before replay existed. The only thing lost is the ability to
         * write, and a kernel that refused the disk outright over an item
         * type it did not recognise would be strictly worse. */
        rc = fs->no_replay || fs->log_cache == NULL ? KERR_NOTSUP : xfs_log_replay(fs);
        if (rc != KERR_OK) {
            fs->write_refused = true;
            kprintf("xfs: the log could not be replayed; mounting read-only\n");
        } else {
            fs->log_replayed = true;
            kprintf("xfs: the log was replayed; the filesystem is clean\n");
        }
    } else if (rc != KERR_OK) {
        return rc;
    }
    fs->mounted = true;
    xfs_quota_report(fs);
    if (fs->rblocks != 0) {
        kprintf("xfs: a realtime subvolume of %llu block(s) in %llu extent(s) of %u; "
                "attach its device to read those files\n",
                (unsigned long long)fs->rblocks, (unsigned long long)fs->rextents,
                fs->rextsize);
    }
    /* **The format is named at mount**, because almost nothing else
     * distinguishes the two from the outside: a v4 filesystem read with v5
     * header sizes does not announce itself, it just returns the wrong
     * bytes. A test that asserts this line is asserting which reader ran. */
    kprintf("xfs: v%u, %u blocks of %u bytes, %u groups, %u-byte inodes, root %u, label \"%s\"\n",
            fs->v5 ? 5u : 4u, (uint32_t)fs->dblocks, fs->blocksize, fs->agcount, fs->inodesize,
            (uint32_t)fs->rootino, fs->label);
    return KERR_OK;
}

/* The magic for a kind of block, in whichever format this filesystem is.
 *
 * A table rather than a chain of comparisons, and indexed by the enum so
 * that adding a kind without giving it both magics does not compile. The
 * superblock's version has already been checked, so there is no third
 * possibility to fall through to.
 *
 * **The pairs are not always a changed digit.** v5 renumbered the
 * directory and attribute magics rather than extending them — `XD2D`
 * became `XDD3`, `0xfbee` became `0x3bee` — which is why each one is
 * written out instead of derived. */
uint32_t xfs_magic_for(const struct xfs *fs, enum xfs_blk kind) {
    static const uint32_t MAGICS[XFS_BLK_KINDS][2] = {
        /* v5                         v4 */
        {XFS_DIR3_BLOCK_MAGIC, XFS_DIR2_BLOCK_MAGIC},
        {XFS_DIR3_DATA_MAGIC, XFS_DIR2_DATA_MAGIC},
        {XFS_DIR3_FREE_MAGIC, XFS_DIR2_FREE_MAGIC},
        {XFS_DIR3_LEAF1_MAGIC, XFS_DIR2_LEAF1_MAGIC},
        {XFS_DIR3_LEAFN_MAGIC, XFS_DIR2_LEAFN_MAGIC},
        {XFS_DA3_NODE_MAGIC, XFS_DA_NODE_MAGIC},
        {XFS_ATTR_LEAF_MAGIC, XFS_ATTR_LEAF_V4_MAGIC},
        {XFS_BNOBT_MAGIC, XFS_BNOBT_V4_MAGIC},
        {XFS_CNTBT_MAGIC, XFS_CNTBT_V4_MAGIC},
        {XFS_INOBT_MAGIC, XFS_INOBT_V4_MAGIC},
        {XFS_FINOBT_MAGIC, XFS_FINOBT_V4_MAGIC},
        {XFS_BMBT_CRC_MAGIC, XFS_BMBT_V4_MAGIC},
    };
    if ((uint32_t)kind >= XFS_BLK_KINDS) {
        return 0; /* no block carries zero, so this matches nothing */
    }
    return MAGICS[kind][fs->v5 ? 0 : 1];
}

void xfs_stamp_owner(const struct xfs *fs, uint8_t *at, uint32_t blkno_off, uint32_t uuid_off,
                     uint32_t owner_off, uint64_t blkno, uint64_t owner) {
    if (!fs->v5) {
        return; /* none of these fields exists; the bytes are the block's */
    }
    if (blkno_off != 0) {
        xfs_put_be64(at + blkno_off, blkno);
    }
    if (uuid_off != 0) {
        for (uint32_t i = 0; i < 16u; i++) {
            at[uuid_off + i] = fs->uuid[i];
        }
    }
    if (owner_off != 0) {
        xfs_put_be64(at + owner_off, owner);
    }
}
