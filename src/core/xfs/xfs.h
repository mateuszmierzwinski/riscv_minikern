/* xfs — a reader for XFS v5, over a buffer cache over a block device.
 *
 * Why XFS at all, for a kernel this small: **Linux can check our work.**
 * `mkfs.xfs` makes the images, `xfs_repair -n` says whether what we wrote
 * is still a valid filesystem, and `xfs_db` prints any structure we doubt.
 * No format we invented could be held to that standard.
 *
 * What is supported, deliberately narrowly:
 *
 *   - **v5 only** (CRC metadata), and the feature flags in
 *     XFS_FEAT_*_SUPPORTED. Anything else is **refused at mount**, which is
 *     the whole of this reader's safety argument: a filesystem it does not
 *     fully understand is one it must not touch, because misreading a btree
 *     is how a reader becomes a corrupter.
 *   - Inodes in the classic layout: 256- or 512-byte inodes, 32-bit extent
 *     counts. An inode with XFS_DIFLAG2_NREXT64 is refused.
 *   - Data in **extents** (XFS_DINODE_FMT_EXTENTS) or inside the inode
 *     (LOCAL). A fork in **btree** format is refused: that is a file with
 *     more extents than fit in an inode, which this cannot yet walk.
 *   - Directories in all three forms — shortform, one block, many blocks —
 *     by walking their data blocks. The hash structures above 32 GiB of
 *     logical offset are never read, so a lookup is a linear scan: right
 *     for a board reading a few hundred entries, wrong for a million.
 *   - **Writing file data, into blocks the file already has.** XFS does not
 *     journal file data, so overwriting an allocated extent changes no
 *     metadata and needs no transaction — which is why it is possible here
 *     at all. Anything that *would* change metadata is refused: a hole, an
 *     extent that was never written, growing a file, creating one. Those
 *     need the log, which is P9.4.
 *   - **Writing file data, into blocks the file already has.** XFS does not
 *     journal file data, so overwriting an allocated extent changes no
 *     metadata and needs no transaction — which is why it is possible here
 *     at all. Anything that *would* change metadata is refused: a hole, an
 *     extent that was never written, growing a file, creating one. Those
 *     need the log, which is P9.4.
 *   - **A dirty log is refused.** Replaying one is not implemented, and
 *     reading a filesystem whose metadata is older than its log is reading
 *     something that is not there. Linux's xfs_repair is what fixes a
 *     crashed filesystem until P9.4 implements recovery.
 *
 * Every structure read is verified: the CRC32C it carries, the UUID that
 * ties it to this filesystem, and the self-identifying field (an inode
 * knows its own number, a directory block knows its owner). A block that
 * fails is KERR_IO, never data.
 *
 * Nothing here is derived from Linux source. The on-disk format is an ABI
 * (xfs_format.h), measured from real images.
 */
#ifndef MINIKERN_CORE_XFS_XFS_H
#define MINIKERN_CORE_XFS_XFS_H

#include <stdbool.h>
#include <stdint.h>

#include "core/bcache.h"

/* The largest inode this reader accepts, and so the largest inline fork it
 * ever copies. 512 is what mkfs.xfs makes by default. */
#define XFS_INODE_MAX  512u
#define XFS_FORK_MAX   (XFS_INODE_MAX - 176u)

/* The largest directory block this reader will mount.
 *
 * A directory block is `1 << sb_dirblklog` filesystem blocks, and the format
 * allows it up to 64 KiB — `mkfs.xfs -n size=` picks it, and a filesystem
 * built for large directories uses more than one block. Entries do not
 * straddle a directory block, but they **do** straddle the filesystem blocks
 * inside one, and the checksum covers the whole thing, so reading such a
 * directory means having all of it contiguous. Hence a buffer, sized for the
 * largest the format permits rather than for the largest seen so far. */
#define XFS_DIRBLK_MAX 65536u

/* The deepest path this will walk, and the longest name it will match. */
#define XFS_PATH_DEPTH 16u
#define XFS_NAME_MAX   255u

/* One mounted filesystem: its geometry, taken from the superblock. */
struct xfs {
    struct bcache *cache;
    uint64_t dblocks;     /* blocks in the filesystem */
    uint64_t rootino;
    uint64_t logstart;    /* the internal log's first block, 0 when external */
    uint64_t dir_leaf_block; /* directory data lives below this logical block */
    uint32_t blocksize;
    uint32_t sectsize;
    uint32_t inodesize;
    uint32_t inopblock;
    uint32_t agblocks;
    uint32_t agcount;
    uint32_t logblocks;
    uint32_t dirblocks;    /* blocks per directory block (1 << sb_dirblklog) */
    uint32_t dirblocksize; /* and that in bytes, which is what parsing uses */
    uint8_t blocklog;
    uint8_t inopblog;
    uint8_t agblklog;
    uint8_t uuid[16];     /* what metadata blocks must carry */
    bool ftype;           /* directory entries carry a type byte */
    bool mounted;
    /* The filesystem uses read-only-compatible features this reader does not
     * implement — reverse mapping, reflink — so it can be read but must
     * never be written: a writer is what those features place obligations
     * on. `xfs_writable` refuses on it, which is the enforcement of the
     * promise `check_features` makes when it allows the mount. */
    bool write_refused;
    /* The inode's extent counts are 64-bit and live at different offsets
     * (`XFS_DI_BIG_NEXTENTS`). From the superblock, because it is a property
     * of the filesystem and not of an inode. */
    bool big_extent_counts;
    /* **How each lookup was answered**, because the two paths are
     * indistinguishable from their results. A name found through the hash
     * index and a name found by walking every entry return the same inode,
     * so a test of the index that does not look at these numbers passes
     * just as happily when the index is never used — which is how it was
     * nearly shipped unexercised. */
    uint64_t hash_lookups;
    uint64_t walked_lookups;
    /* Where a directory block that spans several filesystem blocks is
     * gathered. Per filesystem rather than a module static, so that nothing
     * about reading a directory depends on there being one mount or one
     * thread — and unused when a directory block is a single filesystem
     * block, which is the default geometry. */
    uint8_t dirblock[XFS_DIRBLK_MAX];
};

/* One inode, parsed, with its inline fork copied out so no cached block
 * stays pinned while a caller walks it. */
struct xfs_inode {
    uint64_t ino;
    uint64_t size;
    uint64_t nblocks;
    uint32_t mode;
    /* **Who owns it**, which this reader did not parse at all until there
     * was something to check it against. The permission bits in `mode` are
     * meaningless without them: "the owner may write" needs to know who the
     * owner is. */
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint32_t nextents;
    /* This inode's extent counts are 64-bit and sit at different offsets.
     * **Per inode, not per filesystem**: the superblock feature permits the
     * layout and each inode's `di_flags2` says whether it uses it, so an
     * inode written before the feature was enabled still reads correctly. */
    bool big_extent_counts;
    uint8_t format;
    uint8_t fork[XFS_FORK_MAX];
    uint32_t fork_len;
};

/* The type byte a directory entry carries when the filesystem has the
 * ftype feature; XFS_DIR3_FT_UNKNOWN when it does not, and a caller must
 * then read the inode to find out. These are the format's numbers. */
#define XFS_DIR3_FT_UNKNOWN 0u
#define XFS_DIR3_FT_REG     1u
#define XFS_DIR3_FT_DIR     2u
#define XFS_DIR3_FT_SYMLINK 7u

/* One directory entry, as a listing sees it. name points into a cached
 * block and is **not** NUL-terminated: copy it if it must outlive the
 * callback. */
struct xfs_dirent {
    uint64_t ino;
    const char *name;
    uint32_t name_len;
    uint8_t type; /* XFS_DIR3_FT_*, UNKNOWN when the filesystem has no types */
};

/* A listing callback. Returning anything but KERR_OK stops the walk and
 * becomes xfs_dir_list's result, which is how a lookup stops early. */
typedef int (*xfs_dirent_fn)(void *self, const struct xfs_dirent *entry);

/* Reads the superblock and checks every feature flag against what this
 * reader understands, then checks the log is clean.
 *
 * KERR_OK, or: KERR_INVAL for something that is not XFS or not v5,
 * KERR_NOTSUP for a feature we refuse (the log says which), KERR_IO for a
 * structure that failed its checksum, KERR_BUSY-shaped KERR_NOTSUP for a
 * dirty log. The filesystem is left unmounted on any failure. */
int xfs_mount(struct xfs *fs, struct bcache *cache);

/* Reads one inode by number. KERR_NOENT when it is outside the
 * filesystem, KERR_IO when it fails its checks, KERR_NOTSUP for a format
 * this reader does not walk. */
int xfs_inode_read(struct xfs *fs, uint64_t ino, struct xfs_inode *out);

/* Walks a directory, calling fn for every entry including "." and "..".
 * KERR_NOTDIR when the inode is not a directory. */
int xfs_dir_list(struct xfs *fs, const struct xfs_inode *dir, xfs_dirent_fn fn, void *self);

/* Finds one name in a directory. KERR_NOENT when it is not there. */
int xfs_dir_lookup(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t len,
                   uint64_t *ino);

/* Resolves an absolute path ("/" is the root). Symbolic links are not
 * followed: a link is returned as itself. */
int xfs_path_lookup(struct xfs *fs, const char *path, struct xfs_inode *out);

/* Reads from a regular file, or a symbolic link's target. *got is how much
 * arrived, which is short at end of file and zero at or past it. A hole, and
 * an extent that was allocated but never written, read as zeroes.
 * KERR_ISDIR for a directory — its bytes are structure, not content, and
 * xfs_dir_list is how they are read. */
int xfs_file_read(struct xfs *fs, const struct xfs_inode *ip, uint64_t off, void *buf, uint32_t len,
                  uint32_t *got);

/* Writes into a regular file, within what it already has allocated.
 *
 * *put is how much was written. A write that would need metadata changed
 * is refused with nothing written: KERR_NOSPC past the end of the file,
 * KERR_NOTSUP into a hole or an extent that was allocated but never
 * written (both would mean allocating or converting, which needs the
 * journal), KERR_ROFS on a read-only device, KERR_ISDIR for a directory.
 *
 * The write goes through the buffer cache and is not durable until
 * xfs_flush. */
/* Creates a regular file called name in dir, and gives back its inode
 * number. mode is the permission bits; now_s is the wall clock, because a
 * filesystem does not have one of its own.
 *
 * It needs a filesystem that already has room: an inode free in a chunk
 * that will still have one afterwards, and a parent directory that still
 * lives inside its own inode. Each limit answers with its own errno rather
 * than being worked around — KERR_NOSPC, KERR_NOTSUP, KERR_EXIST — because
 * the alternative is a btree operation this does not implement, and a
 * half-done btree is worse than a refusal (core/xfs/xfs_create.c). */
int xfs_create(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen, uint16_t mode,
               uint64_t now_s, uint64_t *ino_out);

/* Removes a regular file called name from dir.
 *
 * It refuses what it cannot do cleanly rather than doing it badly: a file
 * that owns blocks (they would be leaked), a directory, a hard link, a
 * parent that has outgrown its inode, and a chunk the free-inode tree does
 * not describe (core/xfs/xfs_free.c). */
/* A second name for an existing file, in a directory that still fits in its
 * inode. KERR_PERM for a directory — a second name for one makes a cycle —
 * and KERR_EXIST when the name is taken. */
int xfs_link(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
             uint64_t ino, uint64_t now_s);

/* Moves a name, replacing whatever is at the destination. A directory may
 * be renamed within its own parent but not moved to another, because its
 * `..` and both parents' link counts would then be wrong (KERR_NOTSUP). */
int xfs_rename(struct xfs *fs, const struct xfs_inode *olddir, const char *from, uint32_t fromlen,
               const struct xfs_inode *newdir, const char *to, uint32_t tolen, uint64_t now_s);

int xfs_unlink(struct xfs *fs, const struct xfs_inode *dir, const char *name, uint32_t namelen,
               uint64_t now_s);

int xfs_file_write(struct xfs *fs, struct xfs_inode *ip, uint64_t off, const void *buf,
                   uint32_t len, uint32_t *put);

/* Makes everything written durable: the cache, then the device. */
int xfs_flush(struct xfs *fs);

/* Whether this filesystem may be written at all (the device decides). */
bool xfs_writable(const struct xfs *fs);

/* What kind of thing the inode is. */
bool xfs_is_dir(const struct xfs_inode *ip);
bool xfs_is_reg(const struct xfs_inode *ip);
bool xfs_is_link(const struct xfs_inode *ip);

#endif
