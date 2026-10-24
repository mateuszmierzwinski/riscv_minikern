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
 * ever copies. 512 is what mkfs.xfs makes by default.
 *
 * **The subtraction is the v4 core, not the v5 one.** A v5 inode core is
 * 176 bytes and a v4 core is 100, so the *same* 512-byte inode presents a
 * 412-byte literal area on v4 against 336 on v5 — the older format leaves
 * more room, not less. This was `- 176u`, so every 512-byte v4 inode was
 * refused with "has a 412-byte fork; this reader caches 336", which is a
 * reader built for one format telling the truth about the wrong one. */
#define XFS_INODE_MAX  512u
#define XFS_FORK_MAX   (XFS_INODE_MAX - 100u)

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
/* The label, terminated. Twelve bytes on disk plus the NUL this adds; the
 * on-disk offset is `XFS_SB_FNAME`, which belongs to the format header
 * rather than here. */
#define XFS_LABEL_BYTES 13u

/* The largest attribute fork this reads: the literal area of the largest
 * inode it accepts (512 bytes, less the 176-byte core). */
#define XFS_AFORK_MAX 336u

/* The hash pairs of a block directory under conversion: one pair per entry,
 * and an entry is at least sixteen bytes, so a third of the block is past
 * the ceiling. */
#define XFS_DIRPAIRS_BYTES (XFS_DIRBLK_MAX / 3u)

struct xfs {
    struct bcache *cache;
    uint64_t dblocks;     /* blocks in the filesystem */
    uint64_t rootino;
    uint64_t logstart;    /* the internal log's first block, 0 when external */
    /* **Where the log actually is**, which is not always this filesystem's
     * own device.
     *
     * An *internal* log is a run of blocks inside the filesystem, and
     * `log_cache` is the filesystem's own cache with `log_base` the byte
     * offset of `logstart`. An **external** log is a whole separate device
     * — `sb_logstart` reads zero and the log begins at that device's first
     * byte — so `log_cache` is somebody else's cache and `log_base` is 0.
     *
     * Keeping both as a cache and an offset means every reader of the log
     * is written once: the check, the replay and the writer all take the
     * device from here and none of them knows which kind it is. The
     * alternative — a branch at each of the four places that computes a log
     * address — is four places to forget. */
    struct bcache *log_cache;
    uint64_t log_base;
    /* The log is on a device that has not been attached, so it cannot be
     * read: the filesystem is still readable and must not be written, for
     * the same reason a dirty log makes it read-only. */
    bool log_detached;
    /* Quotas: which kinds this filesystem counts and enforces, and the
     * three files that hold the records. The inode numbers are **zero on a
     * fresh quota filesystem** — `mkfs.xfs` records the intent and a kernel
     * makes the files on first mount — so zero means "not yet", not "no". */
    uint64_t uquotino;
    uint64_t gquotino;
    uint64_t pquotino;
    uint16_t qflags;
    /* **Charging a quota writes the quota file**, and writing a file
     * charges a quota. Today that cannot recurse, because a charge only
     * ever rewrites a record that already exists inside the file and so
     * never asks the allocator for a block — but that is a property of the
     * current code, not of the design, and the day a quota file has to grow
     * it would recurse until the stack ran out. This says no instead. */
    bool charging_quota;
    /* The **realtime subvolume**: a second device holding file data only,
     * with no metadata of its own. A file marked realtime has its extents
     * on it, and they look exactly like any other extents — so a reader
     * that ignored the flag would fetch the right block numbers from the
     * wrong device and hand back whatever the data device holds there.
     *
     * `rt_cache` is NULL until the device is attached, which makes a
     * realtime file's *data* unreadable while leaving the rest of the
     * filesystem entirely usable: unlike the log, this is per-file and so
     * does not justify refusing writes everywhere. */
    struct bcache *rt_cache;
    uint64_t rblocks;
    uint64_t rextents;
    uint64_t rbmino;
    uint64_t rsumino;
    uint32_t rextsize;
    /* **Which on-disk format this is**, and the sizes that follow from it.
     *
     * A v4 filesystem is the same filesystem with shorter headers and no
     * checksums, so these are the handful of numbers the whole difference
     * reduces to. They are fields rather than macros because every reader
     * already has the filesystem in hand, and a macro cannot ask. */
    bool v5;
    uint32_t dinode_size;     /* the inode core: 176 on v5, 100 on v4 */
    uint32_t dir_data_hdr;    /* a directory data block's header */
    uint32_t da_hdr;          /* a hash-tree node or leaf block's */
    uint32_t dir_free_hdr;    /* a directory free block's */
    uint32_t btree_hdr;       /* a short-form b-tree block's: 56 or 16 */
    /* Where a hash-tree block's count and level sit: **immediately after
     * the blkinfo**, which is 56 bytes on v5 and 12 on v4. Not derivable
     * from `da_hdr`, because the v5 header pads to 64 and the v4 one does
     * not — so they are their own numbers rather than an arithmetic trap. */
    uint32_t da_count;
    uint32_t da_level;
    /* An attribute leaf's header. Every field in it sits at a fixed offset
     * from the end of the blkinfo, so one base carries them all: `count`
     * there, `usedbytes` +2, `firstused` +4, `holes` +6, the free map +8.
     * Only the header's total length needs its own number, because v5 pads
     * it to a multiple of eight and v4 does not. */
    uint32_t attr_base;
    uint32_t attr_hdr;
    /* This filesystem predates `attr2`, so the attribute fork's offset
     * inside an inode was fixed by `mkfs` and may not move. Reading is
     * unaffected; growing an attribute fork is refused. */
    bool attr_fixed_fork;
    /* **This filesystem has a free-inode b-tree.** It is a v5 feature, and
     * on v4 `agi_free_root` is not merely zero — it is *past the end of the
     * AGI as v4 defined it* (the structure stops at 296; the field is at
     * 328), so reading it returns whatever follows in the sector. Every
     * path that touches the tree has to ask first. */
    bool has_finobt;
    uint64_t dir_leaf_block; /* directory data lives below this logical block */
    uint64_t dir_free_block; /* and the free blocks of a node directory above */
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
    /* The filesystem's label (`sb_fname`), NUL-terminated here.
     *
     * On disk it is twelve bytes and **not terminated when all twelve are
     * used**, so it is copied rather than pointed at. It is what decides
     * where a volume is mounted: `ROOT` is the root filesystem and anything
     * else is a volume under `/volumes`. */
    char label[XFS_LABEL_BYTES];
    /* The log held uncommitted work when this was mounted, so the newest
     * transactions are not in the metadata. The filesystem is readable and
     * **must not be written**: replaying the log is what would make it
     * writable, and until then a write would build on a state the log says
     * is already superseded. */
    bool log_was_dirty;
    /* The log was dirty and **was replayed**, so the filesystem is clean
     * and writable. Distinct from `log_was_dirty`, which stays true: one
     * says what was found, the other what was done about it. */
    bool log_replayed;
    /* Refuses to replay, so that a test can show the read-only fallback is
     * still reached — the behaviour every unclean filesystem had until
     * replay existed, and the one a kernel falls back to when it meets a
     * log item it does not understand. */
    bool no_replay;
    /* Leaves the next logged transaction **without** its unmount record,
     * which is what a power cut does — the transaction is in the log and
     * nothing says the filesystem was put down.
     *
     * Here so that a test can produce a genuinely dirty log on a filesystem
     * `mkfs.xfs` made, rather than one hand-damaged by a fixture script. A
     * hand-damaged log proves the parser; a log this kernel wrote and then
     * failed to close proves the path a real machine takes. */
    bool skip_unmount_record;
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
    /* Chunks of inodes added because every existing one was full. Counted
     * for the same reason the lookup paths are: a test that creates enough
     * files to need one passes identically if the allocator happened not to
     * need one, so the number is the only thing that says the path ran. */
    uint64_t inode_chunks_added;
    /* Directories that changed shape, by the shape they reached. Counted
     * for the third time for the same reason: a directory that grew and one
     * that never had to look identical from outside, so a test of the
     * growth passes either way unless it reads these. */
    uint64_t grew_to_block;
    uint64_t grew_to_leaf;
    uint64_t grew_to_node;
    /* Leaves split because they filled, which is the step that makes node
     * format grow rather than merely exist. The conversion alone makes two
     * leaves, so a directory that never needed a third never ran it. */
    uint64_t leaf_splits;
    /* Attribute forks moved out of the inode into a leaf block. Counted
     * because an attribute set that happened to fit inside the inode would
     * exercise none of that path while passing every check. */
    uint64_t attrs_to_leaf;
    /* Data blocks added to a directory that had filled the ones it had. */
    uint64_t dir_blocks_added;
    /* Allocations answered by a group other than the one asked for. Zero
     * means the search never had to leave its first choice, so a test of
     * that search proved nothing. */
    uint64_t alloc_other_group;
    /* Keeps the allocator inside the group it was asked for — the old
     * behaviour, kept so that a test can prove the search matters instead
     * of asserting it. `MADV_DONTNEED` cost this project three attempts at
     * one claim for want of exactly this. */
    bool single_group;
    /* Where a directory block that spans several filesystem blocks is
     * gathered. Per filesystem rather than a module static, so that nothing
     * about reading a directory depends on there being one mount or one
     * thread — and unused when a directory block is a single filesystem
     * block, which is the default geometry. */
    /* **Scratch, shared between mounts, not owned by one.** A directory
     * block being parsed or built, and the hash pairs of a block directory
     * mid-conversion.
     *
     * Pointers rather than arrays because `struct xfs` is now one entry in
     * a mount table, and eighty-five kilobytes of scratch per mount would
     * make the table the largest thing in the kernel. They are only live
     * *inside* one directory operation, and this kernel takes one big lock
     * around everything — so one buffer serves every mount. If that lock
     * is ever split, this is one of the things that has to be split with
     * it, which is why it is said here rather than left to be discovered. */
    uint8_t *dirblock;
    /* The hash pairs of a block directory, while it is being converted to
     * leaf format: they have to be held somewhere while the block they came
     * from is rewritten as data, and `dirblock` is that block.
     *
     * **Bounded by what a block directory can hold**, which is one pair per
     * entry and an entry is at least sixteen bytes — so a quarter of the
     * block, in eight-byte pairs, is the ceiling. Here rather than on a
     * stack because the frame guard refused it twice, which is the rule
     * about kernel stacks doing its job. */
    uint8_t *dirpairs;
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
    /* **The attribute fork**, which shares the literal area with the data
     * fork and begins where `di_forkoff` says.
     *
     * `forkoff` is in eight-byte units and **zero means there is no
     * attribute fork at all** — not "it starts at the beginning", which is
     * the reading that would make every inode appear to have one. Setting
     * the first attribute on an inode therefore has to choose a `forkoff`,
     * and choose it so that the data fork still fits below it.
     *
     * Only the two bytes that *locate* the fork are kept here; its contents
     * are read on demand. Copying them into every inode doubled this struct
     * and pushed three functions holding two inodes over the kernel's 1 KiB
     * frame limit — and most inodes have no attributes at all, so it was
     * paying for the rare case in every one of them. */
    uint8_t aformat;
    uint8_t forkoff;
    /* This file's data is on the realtime device (`XFS_DIFLAG_REALTIME`).
     * Its extent records are indistinguishable from any other file's, so
     * this flag is the only thing that says where they point. */
    bool realtime;
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
/* What a listing reports for one attribute: its stored name (without the
 * namespace prefix), **the namespace alone**, and how long its value is.
 *
 * The namespace, not the raw flag byte. `XFS_ATTR_LOCAL` says where the
 * value is kept and is set in a leaf block but not in a shortform fork — so
 * a caller comparing the whole byte sees a `user.` attribute appear to
 * change namespace the moment its fork outgrows the inode. The first
 * consumer of this callback did exactly that and the board caught it, which
 * is reason enough to hand out the thing callers actually mean. */
typedef int (*xfs_attr_fn)(void *self, const uint8_t *name, uint32_t namelen,
                           uint8_t flags, uint32_t valuelen);

/* Extended attributes. Names are Linux's — `user.foo`, `trusted.foo`,
 * `security.foo` — and the namespace is translated to the flag XFS stores,
 * because XFS does not keep the prefix in the name.
 *
 * Shortform is read **and written**: it lives inside the inode, so a set
 * needs no block. The leaf format is read only, and `xfs_attr_set` answers
 * `KERR_NOSPC` when the inode is full rather than building a half-finished
 * leaf. A value kept in blocks of its own is reported as present and
 * refused on read. */
/* One quota record, as the three quota files hold them.
 *
 * Block counts are in filesystem blocks and inode counts are inodes, which
 * is how the format stores them: converting to bytes here would be a choice
 * a caller might disagree with, and the block size is right there. */
struct xfs_dquot {
    uint32_t id;
    uint8_t type;
    uint64_t blocks_hard;
    uint64_t blocks_soft;
    uint64_t inodes_hard;
    uint64_t inodes_soft;
    uint64_t blocks_used;
    uint64_t inodes_used;
    uint64_t rt_blocks_hard;
    uint64_t rt_blocks_soft;
    uint64_t rt_blocks_used;
};

/* Whether a kind of quota is counted, and whether it is actually refused.
 *
 * Two questions, not one: a filesystem can account without enforcing, which
 * is what `-m uquota` gives until somebody sets a limit. A caller that
 * treated "accounted" as "enforced" would report limits that are not
 * applied. */
bool xfs_quota_accounted(const struct xfs *fs, uint8_t type);
bool xfs_quota_enforced(const struct xfs *fs, uint8_t type);

/* One id's record. `KERR_NOTSUP` when this filesystem does not count that
 * kind, `KERR_NODEV` when it does but the file has not been made yet (which
 * is every freshly formatted quota filesystem), and `KERR_NOENT` for an id
 * with no record — a quota file is sparse and most ids are holes. */
int xfs_quota_get(struct xfs *fs, uint8_t type, uint32_t id, struct xfs_dquot *out);

/* Charges an owner for blocks and inodes, or refuses with `KERR_DQUOT`.
 *
 * **Enforcement and accounting are separate.** A filesystem that only
 * accounts still has its counts updated; only one that enforces refuses.
 * A negative amount gives space back and can never be refused.
 *
 * The counts are written straight to the quota file, the same way this
 * writer already updates `agf_freeblks` and the inode b-trees — so a crash
 * between the allocation and the charge leaves the count stale, exactly as
 * it leaves the group's free-block count stale, and `xfs_repair`
 * recalculates both. That is the existing bargain of this write path, not a
 * new one quotas introduce.
 *
 * Only the **hard** limit refuses. A soft limit begins a grace period,
 * which needs a clock this kernel does not keep, so it is read and reported
 * but never acted on. A hard limit of zero means *no* limit, which is the
 * format's convention and the reverse of how the number reads. */
int xfs_quota_charge(struct xfs *fs, uint8_t type, uint32_t id, int64_t blocks, int64_t inodes);

/* The same, for every kind the filesystem counts. All three or none: a
 * refusal from the third gives back what the first two were charged.
 *
 * `_ids` is the general one and `_all` the facade that reads the ids off an
 * inode — creation has no inode yet, which is why both exist. */
int xfs_quota_charge_ids(struct xfs *fs, uint32_t uid, uint32_t gid, int64_t blocks,
                         int64_t inodes);
int xfs_quota_charge_all(struct xfs *fs, const struct xfs_inode *ip, int64_t blocks,
                         int64_t inodes);

/* Says what the filesystem asks for, at mount. */
void xfs_quota_report(const struct xfs *fs);

/* The realtime bitmap: a plain file on the **data** device holding one bit
 * per realtime extent, in which **a set bit means free**.
 *
 * That polarity is the opposite of most bitmaps and is why these are named
 * rather than exposed: a caller that assumed 1 = used would see a fresh
 * subvolume as entirely full. */
bool xfs_rt_extent_is_free(struct xfs *fs, uint64_t n);
int xfs_rt_free_extents(struct xfs *fs, uint64_t *out);

/* Takes a run of `count` free realtime extents, first fit, and returns the
 * first one's number. All of it or none — a half-marked run is a bitmap
 * that disagrees with the inodes. */
int xfs_rt_alloc(struct xfs *fs, uint64_t count, uint64_t *start);
int xfs_rt_release(struct xfs *fs, uint64_t start, uint64_t count);

/* Gives the filesystem its **realtime** device, so that files marked
 * realtime can be read.
 *
 * Nothing in the superblock names it, exactly as with an external log —
 * Linux is told with `-o rtdev=`. Until it is attached, a realtime file's
 * data reads as `KERR_NODEV` and everything else works: the realtime
 * device holds file contents and no metadata, so its absence costs those
 * files and nothing more. */
int xfs_attach_rt(struct xfs *fs, struct bcache *rt);

/* Gives the filesystem the device its **external** log lives on.
 *
 * A filesystem whose `sb_logstart` is zero keeps its log on a separate
 * device, which nothing in the superblock names — Linux is told with
 * `-o logdev=`, and this is the equivalent. Until it is attached the log
 * cannot be read, so the filesystem mounts **read-only**: refusing it
 * outright would make a configuration somebody chose deliberately look like
 * a corrupt disk, and writing to it without being able to read its log
 * would be worse than either.
 *
 * Attaching checks the log and, if it is clean or can be replayed, lifts
 * the restriction — so the ordinary sequence is mount, attach, write. */
int xfs_attach_log(struct xfs *fs, struct bcache *log);

int xfs_attr_get(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                 uint32_t namelen, uint8_t *out, uint32_t room, uint32_t *valuelen);
int xfs_attr_list(struct xfs *fs, const struct xfs_inode *ip, xfs_attr_fn fn, void *self);
int xfs_attr_set(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                 uint32_t namelen, const uint8_t *value, uint32_t valuelen, uint64_t now_s);
int xfs_attr_remove(struct xfs *fs, const struct xfs_inode *ip, const char *name,
                    uint32_t namelen, uint64_t now_s);

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
