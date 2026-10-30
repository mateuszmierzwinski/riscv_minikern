/* Changing an inode's mode and its owner.
 *
 * **Why these are one file and one write.** A mode and an owner live three
 * fields apart in the same inode, both are a single integer, and both have
 * to be followed by a reseal — so implementing them separately would mean
 * two copies of "find the inode, change one number, reseal, mark the buffer
 * dirty", and the second copy is where the reseal gets forgotten. An inode
 * written without its checksum recomputed is one `xfs_repair` offers to
 * clear, and the filesystem will not say so until the next mount.
 *
 * **The file type is preserved, not trusted to the caller.** `chmod` sets
 * the permission bits; the top four bits of `di_mode` say whether this is a
 * file, a directory or a symlink, and a caller passing a whole mode word
 * could silently turn a directory into a regular file. Those bits are taken
 * from what is on disk and the caller's are masked away — which is what
 * `chmod(2)` means and what makes a wrong argument harmless.
 */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

#include <stddef.h>

/* Everything but the file-type bits: the permissions, setuid, setgid and the
 * sticky bit, which is exactly what `chmod` is allowed to change. */
#define MODE_PERMISSION_BITS 0007777u
/* The file-type bits, which only a creation may set. */
#define MODE_TYPE_BITS 0170000u

/* Applies a change to one inode and reseals it.
 *
 * `want_mode` and `want_owner` say which fields the caller is changing, so
 * that "set the group and leave the owner" is expressible — `chown` with -1
 * for one of them is the ordinary case, and a function that always wrote
 * both would turn that into a silent change of the other.
 */
static int change_inode(struct xfs *fs, uint64_t ino, bool want_mode, uint32_t mode,
                        bool want_owner, uint32_t uid, uint32_t gid, uint64_t now_s) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    if (want_mode) {
        uint16_t on_disk = xfs_be16(di + XFS_DI_MODE);
        xfs_put_be16(di + XFS_DI_MODE,
                     (uint16_t)((on_disk & MODE_TYPE_BITS) | (mode & MODE_PERMISSION_BITS)));
    }
    if (want_owner) {
        xfs_put_be32(di + XFS_DI_UID, uid);
        xfs_put_be32(di + XFS_DI_GID, gid);
    }
    /* **The change time, because something changed.** A tool that compares
     * `ctime` to decide whether to re-read an inode — every backup program —
     * would otherwise miss a permission change entirely. */
    xfs_put_time(di, XFS_DI_CTIME, now_s);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_chmod(struct xfs *fs, const struct xfs_inode *ip, uint32_t mode, uint64_t now_s) {
    if (ip == NULL) {
        return KERR_INVAL;
    }
    return change_inode(fs, ip->ino, true, mode, false, 0, 0, now_s);
}

int xfs_chown(struct xfs *fs, const struct xfs_inode *ip, uint32_t uid, uint32_t gid,
              uint64_t now_s) {
    if (ip == NULL) {
        return KERR_INVAL;
    }
    /* **`-1` keeps what is there**, which is `chown(2)`'s convention for
     * "change only the other one". Resolving it here rather than in the
     * syscall layer means every caller gets it, including a future one that
     * forgets. */
    return change_inode(fs, ip->ino, false, 0, true,
                        uid == XFS_OWNER_UNCHANGED ? ip->uid : uid,
                        gid == XFS_OWNER_UNCHANGED ? ip->gid : gid, now_s);
}

/* The counters `statfs` reports, read from the superblock each time.
 *
 * **Read rather than cached.** The allocator keeps `sb_fdblocks` current on
 * every allocation and free, so the superblock is the truth; a copy on
 * `struct xfs` would be a second place to forget to update, and the
 * difference would show as a filesystem that appears full or empty for no
 * reason a user could see. One block read on a `statfs` is not worth a
 * synchronisation bug.
 */
int xfs_counters(struct xfs *fs, struct xfs_counters *out) {
    struct bbuf *b;
    int rc = bcache_get(fs->cache, 0, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    out->blocks = fs->dblocks;
    out->free_blocks = xfs_be64(b->data + XFS_SB_FDBLOCKS);
    out->inodes = xfs_be64(b->data + XFS_SB_ICOUNT);
    out->free_inodes = xfs_be64(b->data + XFS_SB_IFREE);
    out->blocksize = fs->blocksize;
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_times(struct xfs *fs, const struct xfs_inode *ip, uint64_t *atime_s, uint64_t *mtime_s,
              uint64_t *ctime_s) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    const uint8_t *di;
    int rc;
    if (ip == NULL) {
        return KERR_INVAL;
    }
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    *atime_s = xfs_get_time(di, XFS_DI_ATIME);
    *mtime_s = xfs_get_time(di, XFS_DI_MTIME);
    *ctime_s = xfs_get_time(di, XFS_DI_CTIME);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

int xfs_utimes(struct xfs *fs, const struct xfs_inode *ip, uint64_t atime_s, uint64_t mtime_s) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    if (ip == NULL) {
        return KERR_INVAL;
    }
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    xfs_inode_place(fs, ip->ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    /* **The three timestamps are eight bytes apart and only two are the
     * caller's.** `atime` at 32, `mtime` at 40, `ctime` at 48 — and `ctime`
     * is not settable by anyone: it records when the inode last changed,
     * which is *now*, because this call is a change to it. A `utimensat`
     * that let a caller backdate `ctime` would let a modified file claim it
     * had not been touched. */
    xfs_put_time(di, XFS_DI_ATIME, atime_s);
    xfs_put_time(di, XFS_DI_MTIME, mtime_s);
    xfs_put_time(di, XFS_DI_CTIME, atime_s > mtime_s ? atime_s : mtime_s);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}
