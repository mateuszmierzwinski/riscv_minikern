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
    xfs_reseal(fs, di, fs->inodesize, XFS_DI_CRC);
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
    return dir->format == XFS_DINODE_FMT_LOCAL || dir->format == XFS_DINODE_FMT_EXTENTS
               ? KERR_OK
               : KERR_NOTSUP;
}

/* Whether `ino` may take a second name, and what kind it is.
 *
 * **Its own frame, and never inlined.** An inode is the largest thing this
 * reader puts on a stack — a 1024-byte inode's literal area alone is 924
 * bytes — and a caller that holds one *and* a few locals of its own crosses
 * the 1 KiB frame limit this project holds every function to. Reading the
 * inode here hands the caller a type byte instead of a structure, which is
 * what lets the inode size grow without every caller growing with it.
 *
 * `noinline` is load-bearing rather than a hint: without it the compiler
 * folds this frame into the caller's and the sum is what the limit catches,
 * which is exactly what raising the inode ceiling first produced. */
__attribute__((noinline)) static int may_take_another_name(struct xfs *fs, uint64_t ino,
                                                           uint8_t *ftype) {
    struct xfs_inode ip;
    int rc = xfs_inode_read(fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **Not a directory.** A second name for a directory makes a cycle the
     * walker cannot see the end of, and no filesystem allows it. */
    if (xfs_is_dir(&ip)) {
        return KERR_PERM;
    }
    *ftype = ftype_of(&ip);
    return KERR_OK;
}

int xfs_link(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
             uint64_t ino, uint64_t now_s) {
    uint64_t existing = 0;
    uint8_t ftype = 0;
    int rc = may_write_into(fs, dir);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = may_take_another_name(fs, ino, &ftype);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_dir_lookup(fs, dir, name, namelen, &existing) == KERR_OK) {
        return KERR_EXIST;
    }
    rc = xfs_dir_add(fs, dir, name, namelen, ino, ftype, now_s);
    return rc != KERR_OK ? rc : xfs_inode_adjust_nlink(fs, ino, 1);
}

/* The kind of `ino`, refusing a directory that would change parent.
 *
 * **A directory cannot move between directories here**, because its `..`
 * would then name the wrong parent and both parents' link counts would be
 * wrong. Within one directory it is only a name change, which is why that
 * case is allowed and why the caller has to say which it is.
 *
 * `noinline` for the same reason as `may_take_another_name`: the inode is
 * the largest object this code puts on a stack, and folding this frame into
 * `xfs_rename`'s is what pushed it over the 1 KiB limit. */
__attribute__((noinline)) static int kind_for_rename(struct xfs *fs, uint64_t ino,
                                                      bool changing_parent, uint8_t *ftype) {
    struct xfs_inode ip;
    int rc = xfs_inode_read(fs, ino, &ip);
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfs_is_dir(&ip) && changing_parent) {
        return KERR_NOTSUP;
    }
    *ftype = ftype_of(&ip);
    return KERR_OK;
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
    uint8_t ftype = 0;
    int rc = may_write_into(fs, olddir);
    rc = rc != KERR_OK ? rc : may_write_into(fs, newdir);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfs_dir_lookup(fs, olddir, from, fromlen, &ino);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The kind, and whether moving it between directories is allowed, read
     * in a frame of its own — see `may_take_another_name` for why an inode
     * does not live on this one. */
    rc = kind_for_rename(fs, ino, olddir->ino != newdir->ino, &ftype);
    if (rc != KERR_OK) {
        return rc;
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
    rc = rc != KERR_OK ? rc : xfs_dir_add(fs, newdir, to, tolen, ino, ftype, now_s);
    return rc != KERR_OK ? rc : xfs_dir_remove(fs, olddir, from, fromlen, now_s, &removed);
}
