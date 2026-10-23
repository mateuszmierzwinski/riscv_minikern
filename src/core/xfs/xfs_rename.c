/* A second name for a file, and moving a name: `link` and `rename`.
 *
 * Both are arrangements of the two operations `create` and `unlink` already
 * had — adding an entry to a directory and removing one — plus the thing
 * that makes them safe: **the link count**. A file exists for as long as a
 * name points at it, and `unlink` had been refusing any file whose count
 * was above one because nothing could produce such a file. Producing them
 * is what `link` does, so that refusal became a bug the moment this file
 * existed, and is fixed in the same change.
 *
 * `rename` is add-then-remove in that order, which is the order that
 * matters: a crash between them leaves the file reachable under **both**
 * names, where remove-then-add would leave it reachable under neither.
 * Losing a name is worse than gaining one, and a filesystem with no log
 * replay cannot undo either.
 *
 * Both are limited to directories that still fit in their inode, which is
 * the whole of this kernel's write path — `create` and `unlink` refuse the
 * same thing, and growing a directory out of its inode is its own entry in
 * `ROADMAP.md` §3.
 */
#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"

int xfs_inode_adjust_nlink(struct xfs *fs, uint64_t ino, int32_t delta) {
    uint64_t block;
    uint32_t boffset;
    struct bbuf *b;
    uint8_t *di;
    uint32_t was;
    int rc;
    xfs_inode_place(fs, ino, &block, &boffset);
    rc = bcache_get(fs->cache, block, &b);
    if (rc != KERR_OK) {
        return rc;
    }
    di = b->data + boffset;
    was = xfs_be32(di + XFS_DI_NLINK);
    if ((delta < 0 && was == 0) || (delta > 0 && was == 0xffffffffu)) {
        bcache_put(fs->cache, b);
        return KERR_IO; /* a count that cannot move is a corrupt inode */
    }
    xfs_put_be32(di + XFS_DI_NLINK, (uint32_t)((int64_t)was + delta));
    xfs_reseal(di, fs->inodesize, XFS_DI_CRC);
    bcache_dirty(b);
    bcache_put(fs->cache, b);
    return KERR_OK;
}

/* The directory-entry type byte for a mode, so a listing can say what a
 * name is without reading its inode. */
static uint8_t ftype_of(const struct xfs_inode *ip) {
    if (xfs_is_dir(ip)) {
        return XFS_DIR3_FT_DIR;
    }
    return xfs_is_link(ip) ? XFS_DIR3_FT_SYMLINK : XFS_DIR3_FT_REG;
}

/* What both operations need to agree about before touching anything. */
static int may_write_into(const struct xfs *fs, const struct xfs_inode *dir) {
    if (!xfs_writable(fs)) {
        return KERR_ROFS;
    }
    if (!xfs_is_dir(dir)) {
        return KERR_NOTDIR;
    }
    return dir->format == XFS_DINODE_FMT_LOCAL ? KERR_OK : KERR_NOTSUP;
}

int xfs_link(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
             uint64_t ino, uint64_t now_s) {
    struct xfs_inode ip;
    uint64_t existing = 0;
    int rc = may_write_into(fs, dir);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_inode_read(fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **Not a directory.** A second name for a directory makes a cycle the
     * walker cannot see the end of, and no filesystem allows it. */
    if (xfs_is_dir(&ip)) {
        return KERR_PERM;
    }
    if (xfs_dir_lookup(fs, dir, name, namelen, &existing) == KERR_OK) {
        return KERR_EXIST;
    }
    rc = xfs_dir_sf_add(fs, dir, name, namelen, ino, ftype_of(&ip), now_s);
    return rc != KERR_OK ? rc : xfs_inode_adjust_nlink(fs, ino, 1);
}

/* Replaces the name `to` in `newdir`, if something is there.
 *
 * Renaming onto an existing name is defined to replace it, and the thing
 * replaced loses a link — which for its last link means the file goes.
 * `xfs_unlink` already knows how to do all of that, including the
 * difference between the two cases. */
static int replace_target(struct xfs *fs, const struct xfs_inode *newdir, const char *to,
                          uint32_t tolen, uint64_t now_s) {
    uint64_t at = 0;
    if (xfs_dir_lookup(fs, newdir, to, tolen, &at) != KERR_OK) {
        return KERR_OK; /* nothing there: nothing to replace */
    }
    return xfs_unlink(fs, newdir, to, tolen, now_s);
}

int xfs_rename(struct xfs *fs, const struct xfs_inode *olddir, const char *from, uint32_t fromlen,
               const struct xfs_inode *newdir, const char *to, uint32_t tolen, uint64_t now_s) {
    uint64_t ino = 0, removed = 0;
    struct xfs_inode ip;
    int rc = may_write_into(fs, olddir);
    rc = rc != KERR_OK ? rc : may_write_into(fs, newdir);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_dir_lookup(fs, olddir, from, fromlen, &ino);
    rc = rc != KERR_OK ? rc : xfs_inode_read(fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **A directory cannot move between directories here**, because its
     * `..` would then name the wrong parent and both parents' link counts
     * would be wrong. Within one directory it is only a name change, which
     * is why that case is allowed. */
    if (xfs_is_dir(&ip) && olddir->ino != newdir->ino) {
        return KERR_NOTSUP;
    }
    if (olddir->ino == newdir->ino && fromlen == tolen) {
        uint32_t i = 0;
        while (i < fromlen && from[i] == to[i]) {
            i++;
        }
        if (i == fromlen) {
            return KERR_OK; /* the same name in the same directory */
        }
    }
    rc = replace_target(fs, newdir, to, tolen, now_s);
    /* Add before remove: a crash in between leaves the file reachable under
     * both names rather than under neither. */
    rc = rc != KERR_OK ? rc : xfs_dir_sf_add(fs, newdir, to, tolen, ino, ftype_of(&ip), now_s);
    return rc != KERR_OK ? rc : xfs_dir_sf_remove(fs, olddir, from, fromlen, now_s, &removed);
}
