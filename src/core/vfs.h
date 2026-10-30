/* vfs — the filesystem port and the mount table.
 *
 * The root XFS is the kernel's own and stays outside it; everything else
 * (FAT volumes, netfs exports, whatever comes next) is a `vfs_ops` mounted at
 * a path. A path is answered by the mount with the **longest** matching
 * prefix, so `/boot` on FAT still wins over a netfs mounted at `/`.
 *
 * An operation a filesystem does not have is NULL, and the caller answers
 * for it the way Linux would (`EPERM` for links on FAT, `EINVAL` for
 * readlink of something that is not a link). Paths handed to an operation
 * are inside the mount: "/" is the mount point itself.
 */
#ifndef MINIKERN_CORE_VFS_H
#define MINIKERN_CORE_VFS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/xfs/xfs.h"

#define VFS_MOUNTS 16u
#define VFS_AT_MAX 64u

struct xfs_counters;

struct vfs_ops {
    const char *name;
    int (*stat_full)(void *self, const char *in, uint32_t *mode, uint64_t *size,
                     uint32_t *nlink);
    int (*owner)(void *self, const char *in, uint32_t *uid, uint32_t *gid);
    int (*list)(void *self, const char *in, xfs_dirent_fn fn, void *ctx);
    int (*read_file)(void *self, const char *in, uint32_t limit, uint8_t **out, uint32_t *size);
    /* Up to `len` bytes at `off`; `*got` short only at the end of the file.
     * NULL: the whole file is read and the range taken from it, which is
     * right for a filesystem of small generated files and wrong for a
     * database. */
    int (*read_at)(void *self, const char *in, uint64_t off, void *buf, uint32_t len,
                   uint32_t *got);
    int (*write_at)(void *self, const char *dir, const char *leaf, uint64_t off, const void *bytes,
                    uint32_t len);
    int (*mkdir_p)(void *self, const char *in);
    int (*unlink)(void *self, const char *in);
    int (*truncate)(void *self, const char *in, uint64_t size);
    int (*rename)(void *self, const char *from, const char *to);
    int (*counters)(void *self, struct xfs_counters *out); /* NULL: NOTSUP */
    /* Access, modification and change times in Unix seconds; a filesystem
     * that keeps one time reports it for all three. NULL: it keeps none. */
    int (*times)(void *self, const char *in, uint64_t *atime, uint64_t *mtime, uint64_t *ctime);
    /* A symbolic link's target (no terminator, cut to max). NULL: the
     * filesystem has no links, and readlink answers EINVAL. */
    int (*readlink)(void *self, const char *in, char *out, uint32_t max, uint32_t *got);
};

struct vfs_mount {
    char at[VFS_AT_MAX];
    uint32_t at_len;
    const struct vfs_ops *ops;
    void *self;
};

/* Mounts ops/self at `at` (absolute; trailing slashes ignored). KERR_INVAL
 * for a bad path, KERR_BUSY when something is already mounted exactly
 * there, KERR_NOMEM when the table is full. */
int vfs_mount(const char *at, const struct vfs_ops *ops, void *self);

/* Removes the mount at exactly `at`. KERR_INVAL when nothing is there. */
int vfs_umount(const char *at);

/* The mount a path is on and the path inside it, or NULL (the root XFS). */
const struct vfs_mount *vfs_resolve(const char *path, const char **within);

/* Two paths on one mount (KERR_OK, *a and *b inside it), on two different
 * filesystems (KERR_XDEV), or neither mounted (KERR_NOENT: the root's). */
int vfs_resolve_pair(const char *from, const char *to, const struct vfs_mount **m,
                     const char **a, const char **b);

/* The mount table, for listing it (`/proc/mounts`). */
uint32_t vfs_count(void);
const struct vfs_mount *vfs_at(uint32_t i);

/* Empties the table (the host tests). */
void vfs_reset(void);

#endif
