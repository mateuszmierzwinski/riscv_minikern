/* Quotas, as far as a reader can take them.
 *
 * **What a quota is on disk**: three ordinary files, one per kind, whose
 * contents are a flat array of 136-byte records. A record's *place in the
 * array is its id* — record `n` is at `n * 136` — so there is no index, no
 * search and no tree. Reading a quota is an ordinary file read at a
 * computed offset, which is why these files are sparse: a filesystem with
 * two users has two records and a hole where the other four billion would
 * be.
 *
 * **Accounting and enforcement are separate**, and a reader must not
 * conflate them. `sb_qflags` says which kinds are counted and which are
 * actually refused, and a filesystem can count without refusing — that is
 * what `-m uquota` gives before anybody sets a limit.
 *
 * `mkfs.xfs` creates **no** quota inodes: it records the intent in
 * `sb_qflags` and leaves the inode numbers zero, and the files appear when
 * a kernel first mounts the filesystem with quotas on. So a fresh quota
 * filesystem reads as "user and group quotas requested, no files yet",
 * which is a true answer and the one this returns.
 *
 * **Enforcement is not implemented, and that is a deliberate boundary**
 * rather than an oversight. Refusing a write because a quota is exceeded
 * means every allocation and every inode creation consulting and updating
 * a record, inside the same transaction that does the work — and a count
 * updated outside a transaction is a count that drifts from reality on the
 * first crash, which is worse than no accounting at all. A reader that
 * reports the limits honestly is useful; a half-enforced quota is a lie
 * about who owns what.
 */
#include "core/console.h"
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

/* The inode holding one kind of quota, or zero when there is none. */
static uint64_t quota_inode(const struct xfs *fs, uint8_t type) {
    if (type == XFS_DQTYPE_USER) {
        return fs->uquotino;
    }
    if (type == XFS_DQTYPE_GROUP) {
        return fs->gquotino;
    }
    return type == XFS_DQTYPE_PROJ ? fs->pquotino : 0;
}

/* Which flags say this kind is accounted, and which that it is enforced. */
static void quota_flags(uint8_t type, uint16_t *acct, uint16_t *enfd) {
    if (type == XFS_DQTYPE_USER) {
        *acct = XFS_UQUOTA_ACCT;
        *enfd = XFS_UQUOTA_ENFD;
    } else if (type == XFS_DQTYPE_GROUP) {
        *acct = XFS_GQUOTA_ACCT;
        *enfd = XFS_GQUOTA_ENFD;
    } else {
        *acct = XFS_PQUOTA_ACCT;
        *enfd = XFS_PQUOTA_ENFD;
    }
}

bool xfs_quota_accounted(const struct xfs *fs, uint8_t type) {
    uint16_t acct = 0, enfd = 0;
    quota_flags(type, &acct, &enfd);
    return (fs->qflags & acct) != 0;
}

bool xfs_quota_enforced(const struct xfs *fs, uint8_t type) {
    uint16_t acct = 0, enfd = 0;
    quota_flags(type, &acct, &enfd);
    return (fs->qflags & (acct | enfd)) == (acct | enfd);
}

/* Whether a record is the one that was asked for.
 *
 * Four things, and the last two are the ones a reader is tempted to skip.
 * The **magic** says it is a quota record at all; the **version** that it
 * is the layout this understands. The **type** matters because a record
 * carries its own kind and a user record sitting in the group file is a
 * record in the wrong file — the id cannot tell them apart. And the **id**
 * matters because the offset was computed from it: a record whose id is not
 * the one asked for means the arithmetic and the file disagree, which is
 * exactly what reading a sparse file's hole as data would look like. */
static int record_is(const uint8_t *r, uint8_t type, uint32_t id) {
    if (xfs_be16(r + XFS_DQ_MAGIC) != XFS_DQUOT_MAGIC) {
        return KERR_NOENT; /* a hole, or never written: not an error */
    }
    if (r[XFS_DQ_VERSION] != XFS_DQUOT_VERSION) {
        return KERR_NOTSUP;
    }
    if ((r[XFS_DQ_TYPE] & type) == 0 || xfs_be32(r + XFS_DQ_ID) != id) {
        return KERR_IO;
    }
    return KERR_OK;
}

int xfs_quota_get(struct xfs *fs, uint8_t type, uint32_t id, struct xfs_dquot *out) {
    uint8_t record[XFS_DQUOT_BYTES];
    struct xfs_inode qip;
    uint64_t ino = quota_inode(fs, type);
    uint64_t at = (uint64_t)id * XFS_DQUOT_BYTES;
    uint32_t got = 0;
    int rc;
    if (!xfs_quota_accounted(fs, type)) {
        return KERR_NOTSUP; /* this filesystem does not count this kind */
    }
    if (ino == 0) {
        return KERR_NODEV; /* counted, but the file has never been made */
    }
    rc = xfs_inode_read(fs, ino, &qip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (at + XFS_DQUOT_BYTES > qip.size) {
        return KERR_NOENT; /* past the end: this id has no record */
    }
    rc = xfs_file_read(fs, &qip, at, record, XFS_DQUOT_BYTES, &got);
    if (rc != KERR_OK || got != XFS_DQUOT_BYTES) {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    rc = record_is(record, type, id);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The checksum and the UUID, as every other v5 structure gets: a quota
     * record is metadata and a torn one would otherwise read as a limit
     * nobody set. */
    if (!xfs_crc_ok(fs, record, XFS_DQUOT_BYTES, XFS_DQ_CRC_OFF) ||
        !xfs_uuid_ok(fs, record + XFS_DQ_UUID_OFF)) {
        return KERR_IO;
    }
    *out = (struct xfs_dquot){
        .id = id,
        .type = record[XFS_DQ_TYPE],
        .blocks_hard = xfs_be64(record + XFS_DQ_BLK_HARD),
        .blocks_soft = xfs_be64(record + XFS_DQ_BLK_SOFT),
        .inodes_hard = xfs_be64(record + XFS_DQ_INO_HARD),
        .inodes_soft = xfs_be64(record + XFS_DQ_INO_SOFT),
        .blocks_used = xfs_be64(record + XFS_DQ_BCOUNT),
        .inodes_used = xfs_be64(record + XFS_DQ_ICOUNT),
        .rt_blocks_hard = xfs_be64(record + XFS_DQ_RTB_HARD),
        .rt_blocks_soft = xfs_be64(record + XFS_DQ_RTB_SOFT),
        .rt_blocks_used = xfs_be64(record + XFS_DQ_RTBCOUNT),
    };
    return KERR_OK;
}

/* Reads one record for updating, and says where it is so it can be written
 * back. `KERR_NOENT` when the id has no record, which is not an error: an
 * owner nobody set a limit for has no allowance to exceed. */
static int dquot_for_update(struct xfs *fs, uint8_t type, uint32_t id, struct xfs_inode *qip,
                            uint8_t *record, uint64_t *at) {
    uint32_t got = 0;
    uint64_t ino = quota_inode(fs, type);
    int rc;
    *at = (uint64_t)id * XFS_DQUOT_BYTES;
    if (!xfs_quota_accounted(fs, type) || ino == 0) {
        return KERR_NOENT;
    }
    rc = xfs_inode_read(fs, ino, qip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (*at + XFS_DQUOT_BYTES > qip->size) {
        return KERR_NOENT;
    }
    rc = xfs_file_read(fs, qip, *at, record, XFS_DQUOT_BYTES, &got);
    if (rc != KERR_OK || got != XFS_DQUOT_BYTES) {
        return rc != KERR_OK ? rc : KERR_IO;
    }
    return record_is(record, type, id) == KERR_OK ? KERR_OK : KERR_NOENT;
}

/* Whether taking `want` more would pass a limit.
 *
 * **A hard limit of zero means no limit**, which is the format's
 * convention and the opposite of what the number looks like: reading it as
 * "nothing allowed" would refuse every write on a filesystem that merely
 * counts. Only the *hard* limit refuses; the soft limit starts a grace
 * period, which needs a clock this does not keep, so it is counted and
 * reported rather than enforced. */
static bool over_limit(uint64_t used, uint64_t want, uint64_t hard) {
    return hard != 0 && used + want > hard;
}

int xfs_quota_charge(struct xfs *fs, uint8_t type, uint32_t id, int64_t blocks, int64_t inodes) {
    uint8_t record[XFS_DQUOT_BYTES];
    struct xfs_inode qip;
    uint64_t at = 0, bcount, icount;
    uint32_t put = 0;
    int rc;
    /* **The quota file's own writes are not charged.** This rewrites a
     * record in place, so it asks the allocator for nothing and cannot
     * recurse today; the guard is here because "cannot recurse" is a fact
     * about the current code and the failure if it changed would be a
     * stack overflow rather than a wrong answer. */
    if (fs->charging_quota) {
        return KERR_OK;
    }
    fs->charging_quota = true;
    rc = dquot_for_update(fs, type, id, &qip, record, &at);
    if (rc != KERR_OK) {
        fs->charging_quota = false;
        /* Nothing counts this owner, so there is nothing to enforce: not
         * an error, and most ids on a real quota file are holes. */
        return rc == KERR_NOENT ? KERR_OK : rc;
    }
    bcount = xfs_be64(record + XFS_DQ_BCOUNT);
    icount = xfs_be64(record + XFS_DQ_ICOUNT);
    /* **Checked only when enforcement is on, and only upwards.** A
     * filesystem that accounts without enforcing must still count, so the
     * refusal and the update are separate decisions; and giving space back
     * can never exceed a limit. */
    if (xfs_quota_enforced(fs, type)) {
        if ((blocks > 0 &&
             over_limit(bcount, (uint64_t)blocks, xfs_be64(record + XFS_DQ_BLK_HARD))) ||
            (inodes > 0 &&
             over_limit(icount, (uint64_t)inodes, xfs_be64(record + XFS_DQ_INO_HARD)))) {
            fs->charging_quota = false;
            return KERR_DQUOT;
        }
    }
    /* Clamped at zero: a count that went negative would read as enormous
     * and lock the owner out of a filesystem they have nothing on. */
    bcount = blocks < 0 && (uint64_t)(-blocks) > bcount ? 0 : (uint64_t)((int64_t)bcount + blocks);
    icount = inodes < 0 && (uint64_t)(-inodes) > icount ? 0 : (uint64_t)((int64_t)icount + inodes);
    xfs_put_be64(record + XFS_DQ_BCOUNT, bcount);
    xfs_put_be64(record + XFS_DQ_ICOUNT, icount);
    xfs_reseal(fs, record, XFS_DQUOT_BYTES, XFS_DQ_CRC_OFF);
    rc = xfs_file_write(fs, &qip, at, record, XFS_DQUOT_BYTES, &put);
    fs->charging_quota = false;
    return rc != KERR_OK ? rc : (put == XFS_DQUOT_BYTES ? KERR_OK : KERR_IO);
}

int xfs_quota_charge_ids(struct xfs *fs, uint32_t uid, uint32_t gid, int64_t blocks,
                         int64_t inodes) {
    static const uint8_t KINDS[] = {XFS_DQTYPE_USER, XFS_DQTYPE_GROUP, XFS_DQTYPE_PROJ};
    if (fs->qflags == 0) {
        return KERR_OK; /* no quotas at all: the common case, and free */
    }
    for (uint32_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
        uint32_t id = KINDS[i] == XFS_DQTYPE_GROUP ? gid : uid;
        int rc = xfs_quota_charge(fs, KINDS[i], id, blocks, inodes);
        if (rc != KERR_OK) {
            /* **Give back what was already charged.** Three kinds are
             * charged in turn and the third may refuse; leaving the first
             * two charged would make an owner's usage drift upwards every
             * time a write was refused. */
            for (uint32_t j = 0; j < i; j++) {
                uint32_t back = KINDS[j] == XFS_DQTYPE_GROUP ? gid : uid;
                (void)xfs_quota_charge(fs, KINDS[j], back, -blocks, -inodes);
            }
            return rc;
        }
    }
    return KERR_OK;
}

int xfs_quota_charge_all(struct xfs *fs, const struct xfs_inode *ip, int64_t blocks,
                         int64_t inodes) {
    return xfs_quota_charge_ids(fs, ip->uid, ip->gid, blocks, inodes);
}

void xfs_quota_report(const struct xfs *fs) {
    static const struct {
        uint8_t type;
        const char *name;
    } KINDS[] = {{XFS_DQTYPE_USER, "user"},
                 {XFS_DQTYPE_GROUP, "group"},
                 {XFS_DQTYPE_PROJ, "project"}};
    if (fs->qflags == 0) {
        return; /* no quotas asked for: nothing to say */
    }
    for (uint32_t i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++) {
        uint64_t ino = quota_inode(fs, KINDS[i].type);
        if (!xfs_quota_accounted(fs, KINDS[i].type)) {
            continue;
        }
        kprintf("xfs: %s quotas are counted%s, file %s\n", KINDS[i].name,
                xfs_quota_enforced(fs, KINDS[i].type) ? " and enforced" : " but not enforced",
                ino == 0 ? "not yet made" : "present");
    }
}
