/* Symbolic links: creating one, and the bound on how long its target may be.
 *
 * **Why this is a file of its own and not a mode passed to `xfs_create`.**
 * A symlink is the one inode whose *data is its name for something else*,
 * and XFS stores a short target inside the inode's literal area with the
 * data fork in `LOCAL` format. `xfs_create` leaves a new inode in `EXTENTS`
 * format with no data, which for a regular file is exactly right and for a
 * symlink is an inode that `xfs_repair` calls a zero-length symlink and
 * offers to delete. The conversion is the whole of the work here.
 *
 * **Reading needs nothing new.** A `LOCAL` fork is already copied out of the
 * inode by `xfs_inode_read` into `ip->fork`, and `xfs_file_read` already
 * accepts a link inode — so the target comes back through the ordinary read
 * path. That was discovered by reading the code rather than assumed: the
 * read side of this feature was finished before the write side was started.
 *
 * **The long-target case is refused, not half-implemented.** A target that
 * does not fit the literal area lives in a data block, and on v5 that block
 * carries its own `XSLM` header and checksum — a different piece of work
 * from this one. `KERR_NAMETOOLONG` says so, which is the same choice
 * `xfs_create` makes for a directory that has outgrown its inode: a refusal
 * a caller can act on beats a structure `xfs_repair` has to clean up.
 */
#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs_internal.h"

#include <stddef.h>

/* S_IFLNK, and the mode a symlink always carries.
 *
 * `0777` because the permission bits on a symlink are not consulted on any
 * Unix — access is decided by the target — and every other value is a
 * difference that means nothing but that two systems disagree. */
#define SYMLINK_MODE 0120777u

/* Turns a freshly created inode into a local-format symlink holding `target`.
 *
 * Separate from `xfs_symlink` so the inode write is one idea: find the
 * inode, say its fork is local, put the target in it, say how long it is,
 * and reseal. A caller reading those five lines does not also have to hold
 * the directory update in their head. */
static int store_target(struct xfs *fs, uint64_t ino, const char *target, uint32_t len) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_LOCAL;
    xfs_copy_bytes(di + fs->dinode_size, (const uint8_t *)target, len);
    xfs_put_be64(di + XFS_DI_SIZE, len);
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* The longest target that fits inside an inode on this filesystem.
 *
 * Exported because a caller that is about to fail should be able to say by
 * how much, and because the number depends on the inode size rather than
 * being a constant anyone can quote. */
uint32_t xfs_symlink_max(const struct xfs *fs) {
    uint32_t area = fs->inodesize - fs->dinode_size;
    return area < XFS_FORK_MAX ? area : XFS_FORK_MAX; /* a longer one would not be read back */
}

int xfs_symlink(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
                const char *target, uint32_t targetlen, uint64_t now_s, uint64_t *ino_out) {
    uint64_t ino = 0;
    int rc;
    if (targetlen == 0) {
        return KERR_INVAL; /* a link to nothing is not a link */
    }
    if (targetlen > xfs_symlink_max(fs)) {
        return KERR_NAMETOOLONG;
    }
    rc = xfs_create(fs, dir, name, namelen, SYMLINK_MODE, now_s, &ino);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **The target goes in before anybody can look.** Between `xfs_create`
     * and this, the directory names an inode whose mode says symlink and
     * whose size says zero — which is the shape `xfs_repair` deletes. The
     * window cannot be closed entirely without a transaction, and this
     * kernel has no transactions for creation, so it is kept to two calls
     * with nothing between them. */
    rc = store_target(fs, ino, target, targetlen);
    if (rc != KERR_OK) {
        return rc;
    }
    if (ino_out != NULL) {
        *ino_out = ino;
    }
    return KERR_OK;
}
