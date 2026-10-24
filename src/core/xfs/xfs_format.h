/* xfs_format.h — XFS's on-disk layout, as field offsets.
 *
 * This is an ABI, so it is written down as numbers rather than as C structs:
 * the kernel reads these fields out of a block with explicit big-endian
 * accessors, which means no packing attribute, no alignment assumption and
 * no struct that silently differs between compilers. Every offset here was
 * **measured** against images made by Linux's own mkfs.xfs 6.15 (XFS.md §2)
 * rather than copied from a header, and the dumps that confirmed them are
 * in that document.
 *
 * Nothing in this file is Linux source. The numbers are the format, the
 * same way a syscall number is the interface.
 */
#ifndef MINIKERN_CORE_XFS_FORMAT_H
#define MINIKERN_CORE_XFS_FORMAT_H

/* ── the superblock, at byte 0 of the device and of every AG ──────────── */
#define XFS_SB_MAGIC 0x58465342u /* "XFSB" */

#define XFS_SB_BLOCKSIZE   4u   /* u32: bytes per filesystem block */
#define XFS_SB_DBLOCKS     8u   /* u64: blocks in the data section */
#define XFS_SB_UUID        32u  /* 16 bytes */
#define XFS_SB_LOGSTART    48u  /* u64: the internal log's first block */
#define XFS_SB_ROOTINO     56u  /* u64: the root directory's inode */
#define XFS_SB_AGBLOCKS    84u  /* u32: blocks per allocation group */
#define XFS_SB_AGCOUNT     88u  /* u32 */
#define XFS_SB_LOGBLOCKS   96u  /* u32 */
#define XFS_SB_VERSIONNUM  100u /* u16: the low nibble is the version */
#define XFS_SB_SECTSIZE    102u /* u16 */
#define XFS_SB_INODESIZE   104u /* u16 */
#define XFS_SB_INOPBLOCK   106u /* u16: inodes per block */
#define XFS_SB_FNAME       108u /* 12 bytes: the filesystem label */
#define XFS_SB_BLOCKLOG    120u /* u8 */
#define XFS_SB_SECTLOG     121u /* u8 */
#define XFS_SB_INODELOG    122u /* u8 */
#define XFS_SB_INOPBLOG    123u /* u8 */
#define XFS_SB_AGBLKLOG    124u /* u8 */
#define XFS_SB_INPROGRESS  126u /* u8: mkfs had not finished */
#define XFS_SB_DIRBLKLOG   192u /* u8: directory blocks are 1 << this, in fs blocks */
#define XFS_SB_LABEL_MAX 12u /* `sb_fname` is not NUL-terminated when full */
#define XFS_SB_FEAT_COMPAT     208u /* u32 */
/* **v4's second feature word**, and the two rules that come with it.
 *
 * It is only meaningful when `MOREBITS` is set in `versionnum`: on an older
 * filesystem these four bytes are not a feature word at all, so reading
 * them unconditionally invents features.
 *
 * `bad_features2` is a duplicate of it, and not redundancy — a long-standing
 * alignment bug in Linux wrote the field at the wrong offset on some
 * platforms, so both copies exist and `xfs_repair` reconciles them. A
 * reader takes `features2`; the duplicate is here to explain why the next
 * four bytes repeat it rather than looking like corruption. */
#define XFS_SB_FEATURES2       200u /* u32, valid only with MOREBITS */
#define XFS_SB_BAD_FEATURES2   204u /* u32, the same value at the buggy offset */
#define XFS_SB_V4_MOREBITS     0x8000u

#define XFS_SB_V2_LAZYSBCOUNT  0x00000004u
#define XFS_SB_V2_ATTR2        0x00000008u
#define XFS_SB_V2_PROJID32     0x00000080u
/* **The file-type byte, on v4.** `mkfs.xfs` has enabled it by default for
 * years, including for v4, so a v4 filesystem made today has it — and a
 * reader that looked only at v5's incompat word would decide it did not.
 * The cost is not cosmetic: without the byte a shortform entry is one byte
 * shorter, so the *inode number* is read from the wrong place while the
 * name still reads correctly. A directory listing then looks perfect and
 * every lookup in it returns a wrong inode. */
#define XFS_SB_V2_FTYPE        0x00000200u

#define XFS_SB_FEAT_RO_COMPAT  212u /* u32 */
#define XFS_SB_FEAT_INCOMPAT   216u /* u32 */
#define XFS_SB_FEAT_LOG_INCOMPAT 220u /* u32 */
#define XFS_SB_CRC         224u /* u32, little-endian */
#define XFS_SB_META_UUID   248u /* 16 bytes */
#define XFS_SB_SIZE        264u /* what we read and checksum */

#define XFS_SB_VERSION_MASK 0x000fu
#define XFS_SB_VERSION_5    5u
#define XFS_SB_VERSION_4    4u

/* **On v4 the feature flags live in `versionnum` itself**, not in the three
 * separate feature words v5 added. Two of them decide whether this reader
 * can be trusted with the filesystem at all:
 *
 * `DIRV2` says the directories are the format this reader knows. A v4
 * filesystem *without* it has **v1 directories**, which are a different
 * structure entirely — not a shorter header but a different design — so
 * reading one as v2 would misparse every directory in it. Every v4
 * filesystem made since 1997 sets it, which is exactly why a reader that
 * never checked would work on every image anyone tried and fail on the one
 * that mattered.
 *
 * `SHARED` marks a filesystem mounted read-only by several hosts at once.
 * Writing to one is not a format question but a correctness one: another
 * host holds cached metadata that this one's writes would invalidate. */
#define XFS_SB_V4_DIRV2  0x2000u
#define XFS_SB_V4_SHARED 0x0200u

/* **The v4 format is the same filesystem with shorter headers and no
 * checksums.** Every structure that gained a `xfs_da3_blkinfo` or a
 * `xfs_dir3_blk_hdr` in v5 had a shorter header before it, and a different
 * magic to tell them apart — so supporting v4 is not a second reader, it is
 * the same reader with the header size and the accepted magic taken from
 * the filesystem rather than from a constant.
 *
 * Two differences are **not** just a size**. A v4 inode core is 100 bytes,
 * so `XFS_DI_CRC` at 100 lands *inside the literal area*: checksumming
 * there reads fork data, and resealing there would overwrite it. And a v4
 * remote attribute value has no header at all — the value's bytes start at
 * the block's first byte, where v5 puts 56 bytes of `XARM`. */
#define XFS_DI_VERSION_2 2u /* the v4 core: 100 bytes, no checksum, no di_ino */
#define XFS_DI_V2_SIZE 100u /* the v2 core; the literal area follows */

/* The v4 counterparts of the v5 magics, paired with them in
 * `xfs_magic_ok`. The directory and attribute ones differ in more than a
 * digit because v5 renumbered rather than extended them. */
#define XFS_DIR2_BLOCK_MAGIC 0x58443242u /* "XD2B" */
#define XFS_DIR2_DATA_MAGIC  0x58443244u /* "XD2D" */
#define XFS_DIR2_FREE_MAGIC  0x58443246u /* "XD2F" */
#define XFS_DIR2_LEAF1_MAGIC 0xd2f1u
#define XFS_DIR2_LEAFN_MAGIC 0xd2ffu
#define XFS_DA_NODE_MAGIC    0xfebeu
#define XFS_ATTR_LEAF_V4_MAGIC 0xfbeeu
#define XFS_BNOBT_V4_MAGIC   0x41425442u /* "ABTB" */
#define XFS_CNTBT_V4_MAGIC   0x41425443u /* "ABTC" */
#define XFS_INOBT_V4_MAGIC   0x49414254u /* "IABT" */
#define XFS_FINOBT_V4_MAGIC  0x46494254u /* "FIBT" */
#define XFS_BMBT_V4_MAGIC    0x424d4150u /* "BMAP" */

/* The v4 header sizes, each the v5 one minus the block header it lacks. */
#define XFS_DIR2_DATA_HDR_SIZE 16u /* magic and three best-free pairs */
#define XFS_DA_HDR_SIZE        16u /* the 12-byte blkinfo, count and level */
#define XFS_DIR2_FREE_HDR_SIZE 16u /* magic, firstdb, nvalid, nused */

/* Features. A reader must refuse anything it does not understand here,
 * which is what keeps it from misreading a filesystem into rubbish. */
#define XFS_FEAT_RO_FINOBT    0x00000001u /* the free-inode btree: we never read it */
#define XFS_FEAT_RO_RMAPBT    0x00000002u /* reverse mapping: refused */
#define XFS_FEAT_RO_REFLINK   0x00000004u /* shared extents: refused */
#define XFS_FEAT_RO_INOBTCNT  0x00000008u /* a counter in the inode btree: harmless */
#define XFS_FEAT_RO_SUPPORTED (XFS_FEAT_RO_FINOBT | XFS_FEAT_RO_INOBTCNT)

#define XFS_FEAT_IN_FTYPE     0x00000001u /* directory entries carry a type byte */
#define XFS_FEAT_IN_SPINODES  0x00000002u /* sparse inode chunks */
#define XFS_FEAT_IN_META_UUID 0x00000004u /* metadata carries sb_meta_uuid */
#define XFS_FEAT_IN_BIGTIME   0x00000008u /* wider timestamps, which we never read */
#define XFS_FEAT_IN_NEEDSREPAIR 0x00000010u /* known broken: refused */
#define XFS_FEAT_IN_NREXT64   0x00000020u /* 64-bit extent counts: the fields move */
#define XFS_FEAT_IN_EXCHRANGE 0x00000040u /* exchange-range: log intents only */
#define XFS_FEAT_IN_PARENT    0x00000080u /* parent pointers, kept as attributes */

/* What this reader can read.
 *
 * Four of these were refused until a drive formatted by an ordinary
 * `mkfs.xfs` arrived carrying all of them — `incompat 0xe2` — and each was
 * then looked at on its merits rather than as a flag to clear:
 *
 *   - `SPINODES` changes how the inode **b-tree** records which inodes in a
 *     chunk exist. A reader that finds an inode by arithmetic from its
 *     number, as this one does, never consults those records.
 *   - `NREXT64` moves two fields in the inode, which is handled where they
 *     are read.
 *   - `EXCHRANGE` adds a kind of log intent. A clean log contains none, and
 *     this reader refuses a dirty log anyway.
 *   - `PARENT` stores parent pointers as extended attributes, in a fork this
 *     reader does not walk.
 *
 * `NEEDSREPAIR` stays refused: it is the filesystem saying so itself. */
#define XFS_FEAT_IN_SUPPORTED                                                                \
    (XFS_FEAT_IN_FTYPE | XFS_FEAT_IN_META_UUID | XFS_FEAT_IN_BIGTIME | XFS_FEAT_IN_SPINODES | \
     XFS_FEAT_IN_NREXT64 | XFS_FEAT_IN_EXCHRANGE | XFS_FEAT_IN_PARENT)

/* ── the inode ────────────────────────────────────────────────────────── */
#define XFS_DINODE_MAGIC 0x494eu /* "IN" */

#define XFS_DI_MAGIC      0u   /* u16 */
#define XFS_DI_MODE       2u   /* u16 */
#define XFS_DI_VERSION    4u   /* u8: 3 for v5 */
#define XFS_DI_FORMAT     5u   /* u8: XFS_DINODE_FMT_* */
#define XFS_DI_UID        8u   /* u32: the owner */
#define XFS_DI_GID        12u  /* u32: the owning group */
#define XFS_DI_NLINK      16u  /* u32 */
#define XFS_DI_ATIME      32u  /* two u32: seconds, then nanoseconds */
#define XFS_DI_MTIME      40u
#define XFS_DI_CTIME      48u
#define XFS_DI_SIZE       56u  /* u64 */
#define XFS_DI_NBLOCKS    64u  /* u64 */
#define XFS_DI_EXTSIZE    72u  /* u32 */
#define XFS_DI_NEXTENTS   76u  /* u32, without NREXT64 */
#define XFS_DI_ANEXTENTS  80u  /* u16, without NREXT64 */

/* **With `NREXT64`, the data fork's extent count moves.**
 *
 * It does not widen in place: the 64-bit count takes over the eight bytes at
 * offset 24 that a v3 inode otherwise leaves as padding, and the *attribute*
 * fork's count takes over the four bytes at 76 where the data fork's used to
 * be. Reading the old offset on a filesystem that has the feature therefore
 * yields the attribute fork's extent count — a small number, usually zero —
 * for the data fork, and a file reads as empty rather than failing.
 *
 * Taken from the on-disk format's own definition (`struct xfs_dinode`, where
 * the union at offset 24 is `di_big_nextents` or `di_v3_pad`) rather than
 * from memory, because "the field got wider" is exactly the plausible wrong
 * answer. */
#define XFS_DI_BIG_NEXTENTS  24u /* u64, with NREXT64 */
#define XFS_DI_BIG_ANEXTENTS 76u /* u32, with NREXT64 */
#define XFS_DI_FORKOFF    82u  /* u8: the attribute fork, in eight-byte units */
#define XFS_DI_AFORMAT    83u  /* u8 */
#define XFS_DI_FLAGS      90u  /* u16 */
#define XFS_DI_GEN        92u  /* u32 */
#define XFS_DI_NEXT_UNLINKED 96u /* u32: the unlinked list's link, null when none */
#define XFS_NULL_AGINO       0xffffffffu
#define XFS_DI_CRC        100u /* u32, little-endian */
#define XFS_DI_FLAGS2     120u /* u64 */
#define XFS_DI_CRTIME     144u /* two u32 */
#define XFS_DI_INO        152u /* u64: the inode's own number, a self-check */
#define XFS_DI_UUID       160u /* 16 bytes */
#define XFS_DI_V3_SIZE    176u /* the core; the literal area follows */

#define XFS_DI_VERSION_3  3u

/* di_flags2 */
/* `di_flags`. **The realtime flag changes which device a file's extents
 * are on**, which is the only thing about it a reader has to honour: the
 * extent records look identical, and reading them against the data device
 * returns whatever that device happens to hold at those block numbers. */
#define XFS_DIFLAG_REALTIME 0x0001u

#define XFS_DIFLAG2_NREXT64 0x10u /* this inode uses the 64-bit extent layout */

/* di_format */
/* An attribute's namespace, which XFS keeps in a flag rather than in the
 * name: Linux's `user.foo` is stored as `foo` with no flag, `trusted.foo`
 * as `foo` with ROOT, `security.foo` as `foo` with SECURE. A reader that
 * took the prefix to be part of the name would find nothing Linux wrote and
 * write nothing Linux could find. */
#define XFS_ATTR_LOCAL      0x01u /* the value is in the fork, not a block */
#define XFS_ATTR_ROOT       0x02u /* the `trusted.` namespace */
#define XFS_ATTR_SECURE     0x04u /* the `security.` namespace */
#define XFS_ATTR_INCOMPLETE 0x80u /* a set that was interrupted: skip it */
#define XFS_ATTR_NSP (XFS_ATTR_ROOT | XFS_ATTR_SECURE)

/* A leaf block of attributes, for reading what Linux wrote when the set
 * outgrew the inode. The header is a `da3` blkinfo and then its own fields. */
#define XFS_ATTR_LEAF_MAGIC 0x3beeu
#define ATTR_LEAF_COUNT     56u /* be16 */
#define ATTR_LEAF_HDR       80u /* blkinfo, count, usedbytes, firstused, holes, freemap */
#define ATTR_LEAF_V4_BASE   12u /* v4: the blkinfo is 12 bytes, not 56 */
#define ATTR_LEAF_V4_HDR    32u /* and the header is not padded */
#define ATTR_LEAF_ENTRY     8u  /* hashval be32, nameidx be16, flags u8, pad u8 */
#define ATTR_LEAF_NAMEIDX   4u  /* be16, within an entry */
#define ATTR_LEAF_FLAGS     6u  /* u8, within an entry */
#define ATTR_LEAF_USEDBYTES 58u /* be16 */
#define ATTR_LEAF_FIRSTUSED 60u /* be16: the lowest name/value byte in use */
#define ATTR_LEAF_HOLES     62u /* u8: a removal left a gap */
#define ATTR_LEAF_FREEMAP   64u /* three {base be16, size be16} */

/* A **local** name/value pair, and the field order that matters: the value
 * length comes first as a be16, *then* the one-byte name length, then the
 * name and the value. Reading them the other way round — namelen first —
 * costs nothing on a short name and silently mangles everything else, and
 * it is what this reader did for as long as there was no leaf block to read
 * it against. */
#define ATTR_LOCAL_VALUELEN 0u /* be16 */
#define ATTR_LOCAL_NAMELEN  2u /* u8 */
#define ATTR_LOCAL_NAMEVAL  3u /* the name, then the value */

/* A **remote** pair, whose value lives in blocks of its own. */
#define ATTR_REMOTE_VALUEBLK 0u /* be32 */
#define ATTR_REMOTE_VALUELEN 4u /* be32 */
#define ATTR_REMOTE_NAMELEN  8u /* u8 */
#define ATTR_REMOTE_NAME     9u

/* A local pair's bytes in the block, and a remote one's — both rounded to
 * eight. A remote entry carries only the name and where the value lives, so
 * its size does not depend on the value's length at all, which is the whole
 * point of it. */
#define ATTR_LOCAL_SIZE(nlen, vlen) (((nlen) + (vlen) + 3u + 7u) & ~7u)
#define ATTR_REMOTE_SIZE(nlen) (((nlen) + 9u + 7u) & ~7u)

/* A **remote value block**: the value itself, with a header of its own on a
 * v5 filesystem so that a torn or misplaced block is detectable rather than
 * being read as value bytes. */
#define XFS_ATTR3_RMT_MAGIC 0x5841524du /* "XARM" */
#define ATTR_RMT_CRC        8u   /* u32, little-endian */
#define ATTR_RMT_UUID       12u  /* 16 bytes */
#define ATTR_RMT_OWNER      28u  /* u64: the inode it belongs to */
#define ATTR_RMT_BLKNO      36u  /* u64 */
#define ATTR_RMT_LSN        44u  /* u64 */
#define ATTR_RMT_OFFSET     52u  /* u32: this block's offset into the value */
#define ATTR_RMT_BYTES      56u  /* u32: value bytes in this block */
#define ATTR_RMT_HDR        60u  /* then the value bytes */

/* The largest value this writes, which bounds the blocks a single set may
 * need. XFS itself allows 64 KiB; this takes a quarter of that, which is
 * eight 4 KiB blocks and more than any label or ACL in practice. */
#define XFS_ATTR_VALUE_MAX 16384u

#define XFS_DINODE_FMT_DEV     0u
#define XFS_DINODE_FMT_LOCAL   1u /* the fork holds the data itself */
#define XFS_DINODE_FMT_EXTENTS 2u /* the fork holds extent records */
#define XFS_DINODE_FMT_BTREE   3u /* the fork holds a btree root (xfs_bmbt.c) */

/* The extent b-tree, for a fork that has outgrown the inode.
 *
 * Two shapes for one tree: the root lives in the inode behind a four-byte
 * header, and every other block on the disk behind a 72-byte v5 header. In
 * both, a node's keys and pointers are **separate arrays sized by what fits**
 * — so the pointers start after the maximum number of keys, not after the
 * ones in use. `MAX_LEVELS` is a refusal threshold rather than a format
 * limit: a real tree is a handful of levels deep, and a cap is what turns a
 * tree that points at itself into an error instead of a loop. */
#define XFS_BMDR_HDR_BYTES   4u          /* level and numrecs, in the fork */
#define XFS_BMBT_BLOCK_BYTES 72u         /* a v5 long-form btree block header */
#define XFS_BMBT_CRC_MAGIC   0x424d4133u /* "BMA3" */
#define XFS_BMBT_KEY_BYTES   8u
#define XFS_BMBT_PTR_BYTES   8u
#define XFS_BMBT_MAX_LEVELS  16u
#define XFS_BMBT_CRC_OFF     64u /* u32, little-endian */
#define XFS_BMBT_UUID_OFF    40u /* 16 bytes */

/* One extent record is sixteen bytes of packed bit fields:
 *   bit    127  unwritten
 *   bits 73-126 logical block offset  (54 bits)
 *   bits 21-72  physical block        (52 bits)
 *   bits  0-20  block count           (21 bits)
 * Confirmed against a real file: a 200000-byte file came out as offset 0,
 * startblock 20, count 49 (XFS.md §2). */
#define XFS_EXTENT_BYTES 16u

/* Extents this writer will put in a directory's fork, which bounds an array
 * on a stack rather than the format: a 512-byte inode's literal area holds
 * twenty-one and a 256-byte one holds five, so a fork that needs more than
 * this has needed a b-tree for a long time already — and a b-tree
 * *directory* fork is not written. */
#define XFS_DIR_FORK_EXTENTS 24u

/* ── directories ──────────────────────────────────────────────────────── */
/* Shortform, inside the inode: count, i8count, then the parent inode in
 * four or eight bytes, then entries of
 *   namelen(u8) offset(u16) name[namelen] ftype(u8) inumber(u32 or u64). */
#define XFS_DIR_SF_COUNT   0u
#define XFS_DIR_SF_I8COUNT 1u
#define XFS_DIR_SF_PARENT  2u

/* A directory block: a 64-byte header, then entries. "XDB3" is the single
 * block of a small directory, which also carries hash entries and a tail at
 * its end; "XDD3" is a data block of a larger one, which does not. */
#define XFS_DIR3_BLOCK_MAGIC 0x58444233u /* "XDB3" */
#define XFS_DIR3_DATA_MAGIC  0x58444433u /* "XDD3" */

#define XFS_DIR3_HDR_MAGIC 0u  /* u32 */
#define XFS_DIR3_HDR_CRC   4u  /* u32, little-endian */
#define XFS_DIR3_HDR_BLKNO 8u  /* u64, in sectors */
#define XFS_DIR3_HDR_UUID  24u /* 16 bytes */
#define XFS_DIR3_HDR_OWNER 40u /* u64: the directory's own inode */
#define XFS_DIR3_DATA_HDR_SIZE 64u /* magic..owner, three best-free pairs, pad */

/* An entry: inumber(u64) namelen(u8) name[] ftype(u8) tag(u16), the whole
 * thing rounded up to eight bytes. An unused run starts with 0xffff. */
#define XFS_DIR2_DATA_FREE_TAG 0xffffu
#define XFS_DIR2_DATA_ALIGN    8u

/* The tail of an "XDB3" block: count and stale, in its last eight bytes,
 * preceded by count eight-byte hash entries. */
#define XFS_DIR2_BLOCK_TAIL_SIZE 8u
#define XFS_DIR2_LEAF_ENTRY_SIZE 8u

/* The hash index: leaf blocks of sorted hashes, and node blocks above them.
 *
 * A directory that outgrows a single block gains a **leaf** block holding
 * `{hash, address}` pairs sorted by hash; one that outgrows a single leaf
 * gains **node** blocks above them, holding `{hash, block}` pairs. Both sit
 * above 32 GiB of logical offset, which is how they stay out of the data.
 *
 * An `address` is a byte offset into the directory's data space divided by
 * eight — the format's `xfs_dir2_dataptr_t` — so resolving one is a
 * multiply, then a division into a block and an offset within it.
 *
 * The v5 header these blocks share (`xfs_da3_blkinfo`) is 56 bytes, and
 * both kinds put a count where it ends. */
#define XFS_DIR3_LEAF1_MAGIC 0x3df1u /* the one leaf of a leaf-format directory */
#define XFS_DIR3_LEAFN_MAGIC 0x3dffu /* a leaf under node blocks */
#define XFS_DA3_NODE_MAGIC   0x3ebeu /* an interior block of the hash tree */

#define XFS_DA3_MAGIC_OFF  8u  /* u16, inside the 56-byte blkinfo */
#define XFS_DA3_FORW_OFF   0u  /* u32: the next block at this level */
#define XFS_DA3_CRC_OFF    12u /* u32, little-endian */
#define XFS_DA3_UUID_OFF   32u /* 16 bytes */
#define XFS_DA3_OWNER_OFF  48u /* u64 */
#define XFS_DA3_HDR_SIZE   64u /* blkinfo, then count and one more u16, padded */
#define XFS_DA3_COUNT_OFF  56u /* u16: entries in a leaf, or children in a node */
#define XFS_DA3_LEVEL_OFF  58u /* u16, node blocks only: 0 is the leaf level */
#define XFS_DA3_BACK_OFF   4u  /* u32: the previous block at this level */
#define XFS_DA3_BLKNO_OFF  16u /* u64: this block's own da address, in fs blocks */
#define XFS_DA_COUNT_OFF   12u /* v4: straight after the 12-byte blkinfo */
#define XFS_DA_LEVEL_OFF   14u
#define XFS_DA3_ENTRY_SIZE 8u  /* {hash, address} or {hash, block} */

/* How many levels of node blocks this will descend before refusing. The
 * format's limit is five; the cap exists so a tree that points at itself is
 * an error rather than a loop. */
#define XFS_DA3_MAX_LEVELS 8u

/* A leaf block's own tail: how many data blocks its `bests` array covers.
 *
 * The array sits at the end of the leaf, one 16-bit value per data block
 * holding that block's largest free span, and it grows **downwards** from
 * the tail while the hash entries grow upwards from the header. This reader
 * does not consult it — it walks the data blocks and uses the hashes — but
 * a writer that leaves it wrong hands `xfs_repair` and Linux a filesystem
 * they will call corrupt, so it is maintained. */
/* A node-format directory's third address space: the **free** blocks, at
 * 64 GiB of logical offset.
 *
 * Leaf format keeps its `bests` array inside its one leaf block. Node
 * format cannot — a `LEAFN` block is all hashes — so the array moves into
 * blocks of its own up here, above both the data and the leaf/node blocks.
 * This reader never consults them, walking the data blocks instead, but
 * `xfs_repair` and Linux do. */
#define XFS_DIR2_FREE_SPACE_BYTES 0x1000000000ull /* 64 GiB: the third space */
#define XFS_DIR3_FREE_MAGIC 0x58444633u          /* "XDF3" */
#define XFS_DIR3_FREE_HDR_SIZE 64u               /* blk_hdr, firstdb, nvalid, nused, pad */
#define XFS_DIR3_FREE_FIRSTDB 48u                /* be32 */
#define XFS_DIR3_FREE_NVALID 52u                 /* be32 */
#define XFS_DIR3_FREE_NUSED 56u                  /* be32 */

#define XFS_DIR2_LEAF_TAIL_SIZE 4u /* u32 bestcount, at the very end */
#define XFS_DIR2_LEAF_BEST_SIZE 2u /* one u16 per data block */

/* Directory data lives below 32 GiB of logical offset; the hash structures
 * live above it. A listing only has to read what is below. */
#define XFS_DIR2_LEAF_SPACE_BYTES 0x800000000ull /* 32 GiB */

/* The entry types a filesystem with the ftype feature stores are part of
 * what a caller reads, so they live in xfs.h with the field that carries
 * them (struct xfs_dirent). */

/* ── the group headers ───────────────────────────────────────────────────
 * Each allocation group begins with four sectors: a copy of the
 * superblock, the free-space header (AGF), the inode header (AGI) and the
 * free list (AGFL). Measured on a filesystem mkfs.xfs made (XFS.md §10). */
#define XFS_AGI_MAGIC   0x58414749u /* "XAGI" */
#define XFS_AGI_SECTOR  2u          /* which sector of the group's first block */

#define XFS_AGI_SEQNO      8u   /* u32 */
#define XFS_AGI_LENGTH     12u  /* u32 */
#define XFS_AGI_COUNT      16u  /* u32: inodes this group has */
#define XFS_AGI_ROOT       20u  /* u32: the inode btree's root block */
#define XFS_AGI_LEVEL      24u  /* u32: how many levels it has, 1 for a lone leaf */
#define XFS_AGI_FREECOUNT  28u  /* u32: free inodes in this group */
#define XFS_AGI_NEWINO     32u  /* u32 */
#define XFS_AGI_CRC        312u /* u32, little-endian */
#define XFS_AGI_UUID       296u /* 16 bytes */
#define XFS_AGI_FREE_ROOT  328u /* u32: the free-inode btree's root block */
#define XFS_AGI_FREE_LEVEL 332u /* u32 */

/* The superblock's counters, which a create has to keep in step. */
#define XFS_SB_ICOUNT  128u /* u64 */
#define XFS_SB_IFREE   136u /* u64 */

/* The group's free-space header, in sector 1 of its first block. */
#define XFS_AGF_MAGIC  0x58414746u /* "XAGF" */
#define XFS_AGF_SECTOR 1u

#define XFS_AGF_ROOTS     16u  /* three u32: by block, by count, by owner */
#define XFS_AGF_LEVELS    28u  /* three u32 */
#define XFS_AGF_FREEBLKS  52u  /* u32: free blocks in this group */
#define XFS_AGF_LONGEST   56u  /* u32: the largest free extent in it */
#define XFS_AGF_UUID      64u  /* 16 bytes */
#define XFS_AGF_CRC       216u /* u32, little-endian */

/* The free-space btrees: the same shape as the inode ones, with 8-byte
 * records. One is ordered by block, the other by count — and that second
 * ordering is what makes a changed record have to move. */
#define XFS_BNOBT_MAGIC 0x41423342u /* "AB3B" */
#define XFS_CNTBT_MAGIC 0x41423343u /* "AB3C" */

#define XFS_ALLOC_REC_SIZE       8u
#define XFS_ALLOC_REC_STARTBLOCK 0u /* u32 */
#define XFS_ALLOC_REC_BLOCKCOUNT 4u /* u32 */

/* The superblock's free-block counter, which a change has to keep in step. */
#define XFS_SB_FDBLOCKS 144u /* u64 */
#define XFS_SB_RBLOCKS  16u  /* u64: blocks on the realtime device */
#define XFS_SB_REXTENTS 24u  /* u64: realtime extents, which is rblocks/rextsize */
#define XFS_SB_RBMINO   64u  /* u64: the realtime bitmap's inode */
#define XFS_SB_RSUMINO  72u  /* u64: the realtime summary's inode */
#define XFS_SB_REXTSIZE 80u  /* u32: blocks per realtime extent */
#define XFS_SB_UQUOTINO 160u /* u64: the user quota file's inode, 0 if none */
#define XFS_SB_GQUOTINO 168u /* u64: the group quota file's */
#define XFS_SB_QFLAGS   176u /* u16: which quotas are accounted and enforced */
#define XFS_SB_PQUOTINO 232u /* u64: the project quota file's, added in v5 */

/* `sb_qflags`. **Accounting and enforcement are separate**, which is the
 * distinction a reader has to keep: a filesystem can count usage without
 * refusing anything, and `mkfs.xfs -m uquota` sets both while creating
 * neither inode — those appear when a kernel first mounts it with quotas
 * on, so a fresh quota filesystem has the flags and no files. */
#define XFS_UQUOTA_ACCT 0x0001u
#define XFS_UQUOTA_ENFD 0x0002u
#define XFS_UQUOTA_CHKD 0x0004u
#define XFS_PQUOTA_ACCT 0x0008u
#define XFS_OQUOTA_ENFD 0x0010u
#define XFS_OQUOTA_CHKD 0x0020u
#define XFS_GQUOTA_ACCT 0x0040u
#define XFS_GQUOTA_ENFD 0x0080u
#define XFS_GQUOTA_CHKD 0x0100u
#define XFS_PQUOTA_ENFD 0x0200u
#define XFS_PQUOTA_CHKD 0x0400u

/* A disk quota record. The quota files are plain files whose contents are
 * an array of these, and a record's place in the array **is** its id:
 * record `n` is at `n * XFS_DQUOT_BYTES`. There is no index and no search —
 * which is why a quota file is sparse, and why reading one is an ordinary
 * file read at a computed offset. */
#define XFS_DQUOT_MAGIC   0x4451u /* "DQ" */
#define XFS_DQUOT_VERSION 3u      /* on a v5 filesystem */
#define XFS_DQUOT_BYTES   136u

#define XFS_DQ_MAGIC         0u   /* be16 */
#define XFS_DQ_VERSION       2u   /* u8 */
#define XFS_DQ_TYPE          3u   /* u8 */
#define XFS_DQ_ID            4u   /* be32 */
#define XFS_DQ_BLK_HARD      8u   /* be64, in filesystem blocks */
#define XFS_DQ_BLK_SOFT      16u  /* be64 */
#define XFS_DQ_INO_HARD      24u  /* be64, a count of inodes */
#define XFS_DQ_INO_SOFT      32u  /* be64 */
#define XFS_DQ_BCOUNT        40u  /* be64: blocks in use */
#define XFS_DQ_ICOUNT        48u  /* be64: inodes in use */
#define XFS_DQ_RTB_HARD      72u  /* be64, the realtime subvolume's */
#define XFS_DQ_RTB_SOFT      80u  /* be64 */
#define XFS_DQ_RTBCOUNT      88u  /* be64 */
#define XFS_DQ_CRC_OFF       108u /* u32, little-endian */
#define XFS_DQ_UUID_OFF      120u /* 16 bytes */

/* Which quota a record belongs to. A record carries its own type, so a user
 * record found in the group file is a record in the wrong file — worth
 * checking, because the id alone cannot tell them apart. */
#define XFS_DQTYPE_USER    0x01u
#define XFS_DQTYPE_PROJ    0x02u
#define XFS_DQTYPE_GROUP   0x04u

/* ── the inode btrees ────────────────────────────────────────────────────
 * Two trees of the same shape: every inode chunk (`IAB3`), and the chunks
 * that still have a free inode (`FIB3`). A short-form btree block is a
 * 56-byte header and then its records. */
#define XFS_INOBT_MAGIC  0x49414233u /* "IAB3" */
#define XFS_FINOBT_MAGIC 0x46494233u /* "FIB3" */

#define XFS_BTREE_LEVEL   4u  /* u16 */
#define XFS_BTREE_NUMRECS 6u  /* u16 */
#define XFS_BTREE_UUID    32u /* 16 bytes */
#define XFS_BTREE_CRC     52u /* u32, little-endian */
#define XFS_BTREE_RECORDS 56u    /* where the records start on v5 */
#define XFS_BTREE_V4_RECORDS 16u /* magic, level, numrecs and two siblings */

/* One record of either tree, with sparse inodes off: the first inode of a
 * chunk, how many of its 64 are free, and which. A set bit is free. */
#define XFS_INOBT_REC_SIZE      16u
#define XFS_INOBT_REC_STARTINO  0u /* u32 */
#define XFS_INOBT_REC_FREECOUNT 4u /* u32 */
#define XFS_INOBT_REC_FREE      8u /* u64 */
#define XFS_INODES_PER_CHUNK    64u

/* ── shortform directories ──────────────────────────────────────────────
 * An entry is namelen(u8) offset(u16) name[] ftype(u8) inumber(u32), and
 * the offset is where the entry would sit in a block-form directory: the
 * first is 96 — a 64-byte header plus sixteen bytes each for "." and ".." —
 * and each one after advances by (12 + namelen) rounded up to eight.
 * Measured against four files mkfs.xfs put in a directory (XFS.md §10). */
#define XFS_DIR_SF_FIRST_OFFSET 96u
#define XFS_DIR_SF_ENTRY_FIXED  12u /* an entry's bytes in a data block, less the name */

/* ── the log ──────────────────────────────────────────────────────────── */
#define XLOG_MAGIC 0xfeedbabeu

#define XLOG_REC_MAGIC      0u  /* u32 */
#define XLOG_REC_CYCLE      4u  /* u32 */
#define XLOG_REC_VERSION    8u  /* u32 */
#define XLOG_REC_LEN        12u /* u32 */
#define XLOG_REC_LSN        16u /* u64 */
#define XLOG_REC_NUM_OPS    40u /* u32 */
#define XLOG_REC_HDR_SIZE   512u

/* The operation header that follows a record header. */
#define XLOG_OP_TID      0u /* u32 */
#define XLOG_OP_LEN      4u /* u32 */
#define XLOG_OP_CLIENTID 8u /* u8 */
#define XLOG_OP_FLAGS    9u /* u8 */
#define XLOG_OP_HDR_SIZE 12u

#define XLOG_CLIENT_LOG     0xaau /* XFS_LOG: the filesystem itself */
#define XLOG_UNMOUNT_TRANS  0x20u /* this record is the unmount record */
#define XLOG_UNMOUNT_MAGIC  0x556eu /* "Un", written in the host's byte order */

/* A log sector is 512 bytes unless the superblock says otherwise, and a
 * record header always occupies one sector of it. */
#define XLOG_SECTOR 512u

/* **The first four bytes of every data sector are not the data.** The
 * writer stamps the record's cycle number into them and keeps the real
 * bytes in `h_cycle_data` in the header — so that a torn write is
 * detectable: a sector whose first word is not this record's cycle was
 * never written. Replay has to put them back before it can read an
 * operation header, and a replay that did not would find a length and a
 * client id taken from the middle of a cycle number. */
#define XLOG_REC_CYCLE_DATA 44u /* 64 entries, one per 512-byte data sector */
#define XLOG_REC_CRC        32u /* u32, little-endian */

/* The operation flags that frame a transaction, and the item types inside
 * one. Shared with the writer, which puts them there (xfs_logwrite.c). */
#define XLOG_START_TRANS  0x01u
#define XLOG_COMMIT_TRANS 0x02u
#define XLOG_CONTINUE_TRANS 0x04u
#define XLOG_WAS_CONT_TRANS 0x08u
#define XLOG_END_TRANS    0x10u

#define XFS_TRANS_HEADER_MAGIC 0x5452414eu /* "TRAN" */
#define TRANS_HDR_MAGIC     0u  /* u32 */
#define TRANS_HDR_TYPE      4u  /* u32 */
#define TRANS_HDR_TID       8u  /* u32 */
#define TRANS_HDR_NUM_ITEMS 12u /* u32 */
#define TRANS_HDR_SIZE      16u

/* An inode log item: the format structure, then the fields it names. Every
 * field is **little-endian** — a log item is written in the host's byte
 * order, unlike everything else on an XFS filesystem, because the log is
 * replayed by the machine that wrote it. */
#define XFS_LI_INODE     0x123bu
#define XFS_LI_BUF       0x123cu
#define ILF_TYPE         0u  /* u16 */
#define ILF_SIZE         2u  /* u16: how many operations this item occupies */
#define ILF_FIELDS       4u  /* u32 */
#define ILF_ASIZE        8u  /* u16 */
#define ILF_DSIZE        10u /* u16 */
#define ILF_INO          16u /* u64 */
#define ILF_BLKNO        40u /* u64: the inode chunk's device block */
#define ILF_LEN          48u /* u32: its length in 512-byte blocks */
#define ILF_BOFFSET      52u /* u32: the inode's byte offset within it */
#define ILF_STRUCT_SIZE  56u
#define XFS_ILOG_CORE    0x0001u /* the item carries the inode's core */

/* A buffer log item: a block, and a bitmap of which 128-byte chunks of it
 * the record carries. This is what Linux logs most metadata as. */
#define BLF_FLAGS        2u  /* u16 */
#define BLF_LEN          4u  /* u16: the buffer's length in 512-byte blocks */
#define BLF_BLKNO        8u  /* u64: its device block */
#define BLF_MAP_SIZE     16u /* u32: words in the dirty-chunk bitmap */
#define BLF_DATA_MAP     20u /* the bitmap itself */
#define BLF_CHUNK        128u /* a bitmap bit covers this many bytes */

#endif
