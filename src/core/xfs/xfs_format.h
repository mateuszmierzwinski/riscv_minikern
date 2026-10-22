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
#define XFS_SB_BLOCKLOG    120u /* u8 */
#define XFS_SB_SECTLOG     121u /* u8 */
#define XFS_SB_INODELOG    122u /* u8 */
#define XFS_SB_INOPBLOG    123u /* u8 */
#define XFS_SB_AGBLKLOG    124u /* u8 */
#define XFS_SB_INPROGRESS  126u /* u8: mkfs had not finished */
#define XFS_SB_DIRBLKLOG   192u /* u8: directory blocks are 1 << this, in fs blocks */
#define XFS_SB_FEAT_COMPAT     208u /* u32 */
#define XFS_SB_FEAT_RO_COMPAT  212u /* u32 */
#define XFS_SB_FEAT_INCOMPAT   216u /* u32 */
#define XFS_SB_FEAT_LOG_INCOMPAT 220u /* u32 */
#define XFS_SB_CRC         224u /* u32, little-endian */
#define XFS_SB_META_UUID   248u /* 16 bytes */
#define XFS_SB_SIZE        264u /* what we read and checksum */

#define XFS_SB_VERSION_MASK 0x000fu
#define XFS_SB_VERSION_5    5u

/* Features. A reader must refuse anything it does not understand here,
 * which is what keeps it from misreading a filesystem into rubbish. */
#define XFS_FEAT_RO_FINOBT    0x00000001u /* the free-inode btree: we never read it */
#define XFS_FEAT_RO_RMAPBT    0x00000002u /* reverse mapping: refused */
#define XFS_FEAT_RO_REFLINK   0x00000004u /* shared extents: refused */
#define XFS_FEAT_RO_INOBTCNT  0x00000008u /* a counter in the inode btree: harmless */
#define XFS_FEAT_RO_SUPPORTED (XFS_FEAT_RO_FINOBT | XFS_FEAT_RO_INOBTCNT)

#define XFS_FEAT_IN_FTYPE     0x00000001u /* directory entries carry a type byte */
#define XFS_FEAT_IN_SPINODES  0x00000002u /* sparse inode chunks: refused */
#define XFS_FEAT_IN_META_UUID 0x00000004u /* metadata carries sb_meta_uuid */
#define XFS_FEAT_IN_BIGTIME   0x00000008u /* wider timestamps, which we never read */
#define XFS_FEAT_IN_NEEDSREPAIR 0x00000010u /* known broken: refused */
#define XFS_FEAT_IN_NREXT64   0x00000020u /* 64-bit extent counts: a different inode layout */
#define XFS_FEAT_IN_SUPPORTED \
    (XFS_FEAT_IN_FTYPE | XFS_FEAT_IN_META_UUID | XFS_FEAT_IN_BIGTIME)

/* ── the inode ────────────────────────────────────────────────────────── */
#define XFS_DINODE_MAGIC 0x494eu /* "IN" */

#define XFS_DI_MAGIC      0u   /* u16 */
#define XFS_DI_MODE       2u   /* u16 */
#define XFS_DI_VERSION    4u   /* u8: 3 for v5 */
#define XFS_DI_FORMAT     5u   /* u8: XFS_DINODE_FMT_* */
#define XFS_DI_NLINK      16u  /* u32 */
#define XFS_DI_ATIME      32u  /* two u32: seconds, then nanoseconds */
#define XFS_DI_MTIME      40u
#define XFS_DI_CTIME      48u
#define XFS_DI_SIZE       56u  /* u64 */
#define XFS_DI_NBLOCKS    64u  /* u64 */
#define XFS_DI_EXTSIZE    72u  /* u32 */
#define XFS_DI_NEXTENTS   76u  /* u32 */
#define XFS_DI_ANEXTENTS  80u  /* u16 */
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
#define XFS_DIFLAG2_NREXT64 0x10u /* this inode uses the 64-bit extent layout */

/* di_format */
#define XFS_DINODE_FMT_DEV     0u
#define XFS_DINODE_FMT_LOCAL   1u /* the fork holds the data itself */
#define XFS_DINODE_FMT_EXTENTS 2u /* the fork holds extent records */
#define XFS_DINODE_FMT_BTREE   3u /* the fork holds a btree root: refused */

/* One extent record is sixteen bytes of packed bit fields:
 *   bit    127  unwritten
 *   bits 73-126 logical block offset  (54 bits)
 *   bits 21-72  physical block        (52 bits)
 *   bits  0-20  block count           (21 bits)
 * Confirmed against a real file: a 200000-byte file came out as offset 0,
 * startblock 20, count 49 (XFS.md §2). */
#define XFS_EXTENT_BYTES 16u

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
#define XFS_BTREE_RECORDS 56u /* where the records start */

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

#endif
