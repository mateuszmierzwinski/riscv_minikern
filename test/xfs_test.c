#include <string.h>

#include "core/errno.h"
#include "core/xfs/xfs_format.h"
#include "core/xfs/xfs_internal.h"
#include "ktest.h"
#include "lib/crc32c.h"

/* The XFS reader under attack.
 *
 * xfs_live_test.sh proves the reader against filesystems Linux made, which
 * is what says it reads XFS correctly. This proves the other half: that a
 * filesystem which is *wrong* — truncated, mislabelled, lying about its own
 * lengths, pointing outside itself — is refused rather than believed. Those
 * images cannot be made with mkfs.xfs, so they are built here, with real
 * checksums (our own crc32c, which the live test has already shown agrees
 * with Linux's), and then damaged one field at a time.
 *
 * A filesystem is somebody's data, so the standard is: never crash, never
 * read outside the device, and never return something made up. */

#define BS      4096u
#define BLOCKS  64u
#define ISIZE   512u
/* Block 4 holds the inodes (eight of them fit), so the inode numbers all
 * name a slot in it: a number whose block collided with the directory's
 * data block was the first thing this scaffolding got wrong. */
#define ROOT_FSB 4u
#define ROOT_INO 32u    /* group 0, block 4, slot 0 */
#define FILE_INO 33u    /* slot 1 */
#define SUB_INO  34u    /* slot 2 */
#define DIR_FSB  5u     /* the root directory's one data block */
#define FILE_FSB 6u     /* the file's one data block */
#define LOG_FSB  8u
#define LOG_BLOCKS 4u

static uint8_t disk[BLOCKS * BS];
static uint8_t cache_mem[8u * BS];
static struct bcache cache;
static struct blockdev dev;
static struct xfs fs;
static uint32_t reads_outside;

static const uint8_t TEST_UUID[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* The device, which also watches for a read outside itself: that is the
 * failure a parser bug would produce, and it must never happen. */
static int disk_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!blockdev_in_range(&dev, lba, count)) {
        reads_outside++;
        return KERR_INVAL;
    }
    memcpy(buf, disk + lba * BS, count * BS);
    return KERR_OK;
}

static int disk_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!blockdev_in_range(&dev, lba, count)) {
        reads_outside++;
        return KERR_INVAL;
    }
    memcpy(disk + lba * BS, buf, count * BS);
    return KERR_OK;
}

static int disk_flush(void *self) {
    (void)self;
    return KERR_OK;
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v) {
    put16(p, (uint16_t)(v >> 16));
    put16(p + 2, (uint16_t)v);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)(v >> 32));
    put32(p + 4, (uint32_t)v);
}

/* The little-endian checksum field, computed with the field zeroed — the
 * same rule the reader verifies by. */
static void seal(uint8_t *buf, uint32_t len, uint32_t crc_off) {
    uint32_t crc;
    memset(buf + crc_off, 0, 4);
    crc = ~crc32c_update(CRC32C_SEED, buf, len);
    buf[crc_off] = (uint8_t)crc;
    buf[crc_off + 1] = (uint8_t)(crc >> 8);
    buf[crc_off + 2] = (uint8_t)(crc >> 16);
    buf[crc_off + 3] = (uint8_t)(crc >> 24);
}

static uint8_t *block(uint32_t fsb) {
    return disk + (uint64_t)fsb * BS;
}

/* Where an inode number says its inode is. */
static uint8_t *inode_at(uint64_t ino) {
    return block((uint32_t)((ino >> 3) & 63u)) + (uint32_t)(ino & 7u) * ISIZE;
}

/* ── building a filesystem that is correct, so the tests can break it ──── */

static void build_super(void) {
    uint8_t *sb = block(0);
    memset(sb, 0, BS);
    put32(sb + 0, XFS_SB_MAGIC);
    put32(sb + XFS_SB_BLOCKSIZE, BS);
    put64(sb + XFS_SB_DBLOCKS, BLOCKS);
    memcpy(sb + XFS_SB_UUID, TEST_UUID, 16);
    put64(sb + XFS_SB_LOGSTART, LOG_FSB);
    put64(sb + XFS_SB_ROOTINO, ROOT_INO);
    put32(sb + XFS_SB_AGBLOCKS, BLOCKS);
    put32(sb + XFS_SB_AGCOUNT, 1u);
    put32(sb + XFS_SB_LOGBLOCKS, LOG_BLOCKS);
    put16(sb + XFS_SB_VERSIONNUM, 0xb000u | XFS_SB_VERSION_5);
    put16(sb + XFS_SB_SECTSIZE, 512u);
    put16(sb + XFS_SB_INODESIZE, ISIZE);
    put16(sb + XFS_SB_INOPBLOCK, BS / ISIZE);
    sb[XFS_SB_BLOCKLOG] = 12u;
    sb[XFS_SB_SECTLOG] = 9u;
    sb[XFS_SB_INODELOG] = 9u;
    sb[XFS_SB_INOPBLOG] = 3u;
    sb[XFS_SB_AGBLKLOG] = 6u;
    put32(sb + XFS_SB_FEAT_RO_COMPAT, XFS_FEAT_RO_FINOBT);
    put32(sb + XFS_SB_FEAT_INCOMPAT, XFS_FEAT_IN_FTYPE);
    seal(sb, 512u, XFS_SB_CRC);
}

/* A clean log: one record, whose one operation is the unmount. */
static void build_log(void) {
    uint8_t *log = block(LOG_FSB);
    memset(log, 0, LOG_BLOCKS * BS);
    put32(log + XLOG_REC_MAGIC, XLOG_MAGIC);
    put32(log + XLOG_REC_CYCLE, 1u);
    put32(log + XLOG_REC_VERSION, 2u);
    put32(log + XLOG_REC_LEN, 512u);
    put32(log + XLOG_REC_NUM_OPS, 1u);
    put32(log + XLOG_SECTOR + XLOG_OP_LEN, 8u);
    log[XLOG_SECTOR + XLOG_OP_CLIENTID] = XLOG_CLIENT_LOG;
    log[XLOG_SECTOR + XLOG_OP_FLAGS] = XLOG_UNMOUNT_TRANS;
    put16(log + XLOG_SECTOR + XLOG_OP_HDR_SIZE, XLOG_UNMOUNT_MAGIC);
}

/* One inode, at the slot its number names. */
static uint8_t *build_inode(uint64_t ino, uint32_t mode, uint8_t format, uint64_t size) {
    uint8_t *di = inode_at(ino);
    memset(di, 0, ISIZE);
    put16(di + XFS_DI_MAGIC, XFS_DINODE_MAGIC);
    put16(di + XFS_DI_MODE, (uint16_t)mode);
    di[XFS_DI_VERSION] = XFS_DI_VERSION_3;
    di[XFS_DI_FORMAT] = format;
    put32(di + XFS_DI_NLINK, 1u);
    put64(di + XFS_DI_SIZE, size);
    put64(di + XFS_DI_INO, ino);
    memcpy(di + XFS_DI_UUID, TEST_UUID, 16);
    return di;
}

/* An extent record in an inode's inline fork. */
static void set_extent(uint8_t *di, uint32_t index, uint64_t startoff, uint64_t startblock,
                       uint32_t count, bool unwritten) {
    uint8_t *p = di + XFS_DI_V3_SIZE + index * XFS_EXTENT_BYTES;
    uint64_t hi = ((uint64_t)(unwritten ? 1u : 0u) << 63) | (startoff << 9) | (startblock >> 43);
    uint64_t lo = ((startblock & (((uint64_t)1 << 43) - 1u)) << 21) | count;
    put64(p, hi);
    put64(p + 8, lo);
    put32(di + XFS_DI_NEXTENTS, index + 1u);
}

/* One directory entry written into a block at *at. */
static void put_dirent(uint8_t *blk, uint32_t *at, uint64_t ino, const char *name, uint8_t type) {
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t len = (8u + 1u + name_len + 1u + 2u + 7u) & ~7u;
    put64(blk + *at, ino);
    blk[*at + 8u] = (uint8_t)name_len;
    memcpy(blk + *at + 9u, name, name_len);
    blk[*at + 9u + name_len] = type;
    put16(blk + *at + len - 2u, (uint16_t)*at);
    *at += len;
}

/* The root directory as one data block: ".", "..", a file and a directory. */
static void build_dir(uint64_t owner, uint32_t fsb, uint32_t magic) {
    uint8_t *blk = block(fsb);
    uint32_t at = XFS_DIR3_DATA_HDR_SIZE;
    memset(blk, 0, BS);
    put32(blk + XFS_DIR3_HDR_MAGIC, magic);
    put64(blk + XFS_DIR3_HDR_BLKNO, (uint64_t)fsb * (BS / 512u));
    memcpy(blk + XFS_DIR3_HDR_UUID, TEST_UUID, 16);
    put64(blk + XFS_DIR3_HDR_OWNER, owner);
    put_dirent(blk, &at, owner, ".", XFS_DIR3_FT_DIR);
    put_dirent(blk, &at, owner, "..", XFS_DIR3_FT_DIR);
    put_dirent(blk, &at, FILE_INO, "file.txt", XFS_DIR3_FT_REG);
    put_dirent(blk, &at, SUB_INO, "sub", XFS_DIR3_FT_DIR);
    if (magic == XFS_DIR3_BLOCK_MAGIC) {
        /* A single-block directory keeps four hash entries and a tail at
         * the end of the same block, and the entries stop before them —
         * which is the accounting the reader has to get right. */
        uint32_t count = 4u;
        uint32_t leaf = BS - XFS_DIR2_BLOCK_TAIL_SIZE - count * XFS_DIR2_LEAF_ENTRY_SIZE;
        put16(blk + at, XFS_DIR2_DATA_FREE_TAG);
        put16(blk + at + 2u, (uint16_t)(leaf - at));
        put16(blk + leaf - 2u, (uint16_t)at);
        put32(blk + BS - XFS_DIR2_BLOCK_TAIL_SIZE, count);
    } else {
        /* A data block of a larger directory: entries to the very end. */
        put16(blk + at, XFS_DIR2_DATA_FREE_TAG);
        put16(blk + at + 2u, (uint16_t)(BS - at));
        put16(blk + BS - 2u, (uint16_t)at);
    }
    seal(blk, BS, XFS_DIR3_HDR_CRC);
}

/* The whole filesystem: a superblock, a clean log, a root directory in one
 * block, a file of known bytes, and a shortform subdirectory. */
static void build_fs(void) {
    uint8_t *di;
    memset(disk, 0, sizeof disk);
    reads_outside = 0;
    build_super();
    build_log();
    di = build_inode(ROOT_INO, 0040755u, XFS_DINODE_FMT_EXTENTS, BS);
    set_extent(di, 0, 0, DIR_FSB, 1u, false);
    seal(di, ISIZE, XFS_DI_CRC);
    build_dir(ROOT_INO, DIR_FSB, XFS_DIR3_BLOCK_MAGIC);

    di = build_inode(FILE_INO, 0100644u, XFS_DINODE_FMT_EXTENTS, 10u);
    set_extent(di, 0, 0, FILE_FSB, 1u, false);
    seal(di, ISIZE, XFS_DI_CRC);
    memcpy(block(FILE_FSB), "0123456789", 10u);

    /* A shortform directory: count, i8count, parent, then one entry. */
    di = build_inode(SUB_INO, 0040755u, XFS_DINODE_FMT_LOCAL, 0);
    {
        uint8_t *sf = di + XFS_DI_V3_SIZE;
        uint32_t at = XFS_DIR_SF_PARENT + 4u;
        sf[XFS_DIR_SF_COUNT] = 1u;
        sf[XFS_DIR_SF_I8COUNT] = 0u;
        put32(sf + XFS_DIR_SF_PARENT, (uint32_t)ROOT_INO);
        sf[at] = 5u; /* namelen */
        put16(sf + at + 1u, 0x30u);
        memcpy(sf + at + 3u, "inner", 5u);
        sf[at + 8u] = XFS_DIR3_FT_REG;
        put32(sf + at + 9u, (uint32_t)FILE_INO);
        put64(di + XFS_DI_SIZE, at + 13u);
    }
    seal(di, ISIZE, XFS_DI_CRC);
}

static void mount_fresh(void) {
    build_fs();
    dev = (struct blockdev){.read = disk_read,
                            .write = disk_write,
                            .flush = disk_flush,
                            .block_size = BS,
                            .blocks = BLOCKS};
    KTEST_CHECK(bcache_init(&cache, &dev, cache_mem, 8u) == KERR_OK);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
}

/* Rebuilds, applies one change, and mounts: the shape of every refusal
 * test below. */
static int mount_with(void (*damage)(void)) {
    build_fs();
    damage();
    dev = (struct blockdev){.read = disk_read,
                            .write = disk_write,
                            .flush = disk_flush,
                            .block_size = BS,
                            .blocks = BLOCKS};
    bcache_init(&cache, &dev, cache_mem, 8u);
    return xfs_mount(&fs, &cache);
}

/* ── counting what a listing saw ───────────────────────────────────────── */

struct seen {
    uint32_t count;
    char last[64];
    uint64_t last_ino;
};

static int count_entry(void *self, const struct xfs_dirent *e) {
    struct seen *s = self;
    uint32_t n = e->name_len < sizeof s->last - 1u ? e->name_len : (uint32_t)sizeof s->last - 1u;
    s->count++;
    memcpy(s->last, e->name, n);
    s->last[n] = '\0';
    s->last_ino = e->ino;
    return KERR_OK;
}

/* ── the filesystem we built must read correctly ───────────────────────── */

static void test_mount_and_geometry(void) {
    mount_fresh();
    KTEST_CHECK(fs.blocksize == BS && fs.inodesize == ISIZE && fs.inopblock == 8u);
    KTEST_CHECK(fs.rootino == ROOT_INO && fs.agcount == 1u && fs.dblocks == BLOCKS);
    KTEST_CHECK(fs.ftype && fs.mounted);
}

static void test_list_the_root(void) {
    struct xfs_inode root;
    struct seen seen = {0};
    mount_fresh();
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &root) == KERR_OK);
    KTEST_CHECK(xfs_is_dir(&root));
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_OK);
    KTEST_CHECK(seen.count == 4u);
    KTEST_CHECK_STR(seen.last, "sub");
    KTEST_CHECK(seen.last_ino == SUB_INO);
}

static void test_lookup_and_read(void) {
    struct xfs_inode ip;
    uint8_t buf[16];
    uint32_t got = 0;
    mount_fresh();
    KTEST_CHECK(xfs_path_lookup(&fs, "/file.txt", &ip) == KERR_OK);
    KTEST_CHECK(ip.ino == FILE_INO && ip.size == 10u && xfs_is_reg(&ip));
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, sizeof buf, &got) == KERR_OK);
    KTEST_CHECK(got == 10u && memcmp(buf, "0123456789", 10u) == 0);
    /* Part way in, and past the end. */
    KTEST_CHECK(xfs_file_read(&fs, &ip, 4u, buf, 3u, &got) == KERR_OK);
    KTEST_CHECK(got == 3u && memcmp(buf, "456", 3u) == 0);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 10u, buf, 4u, &got) == KERR_OK && got == 0);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 1000u, buf, 4u, &got) == KERR_OK && got == 0);
}

static void test_a_shortform_directory(void) {
    struct xfs_inode ip;
    struct seen seen = {0};
    uint64_t ino = 0;
    mount_fresh();
    KTEST_CHECK(xfs_path_lookup(&fs, "/sub", &ip) == KERR_OK);
    KTEST_CHECK(ip.format == XFS_DINODE_FMT_LOCAL);
    KTEST_CHECK(xfs_dir_list(&fs, &ip, count_entry, &seen) == KERR_OK);
    KTEST_CHECK(seen.count == 3u);  /* ".", "..", "inner" */
    KTEST_CHECK_STR(seen.last, "inner");
    KTEST_CHECK(xfs_dir_lookup(&fs, &ip, "inner", 5u, &ino) == KERR_OK && ino == FILE_INO);
    KTEST_CHECK(xfs_dir_lookup(&fs, &ip, "inne", 4u, &ino) == KERR_NOENT);
    KTEST_CHECK(xfs_dir_lookup(&fs, &ip, "innerx", 6u, &ino) == KERR_NOENT);
    KTEST_CHECK(xfs_path_lookup(&fs, "/sub/inner", &ip) == KERR_OK && ip.ino == FILE_INO);
}

static void test_paths(void) {
    struct xfs_inode ip;
    mount_fresh();
    KTEST_CHECK(xfs_path_lookup(&fs, "/", &ip) == KERR_OK && ip.ino == ROOT_INO);
    KTEST_CHECK(xfs_path_lookup(&fs, "///", &ip) == KERR_OK && ip.ino == ROOT_INO);
    KTEST_CHECK(xfs_path_lookup(&fs, "//file.txt//", &ip) == KERR_OK && ip.ino == FILE_INO);
    KTEST_CHECK(xfs_path_lookup(&fs, "file.txt", &ip) == KERR_INVAL);  /* not absolute */
    KTEST_CHECK(xfs_path_lookup(&fs, NULL, &ip) == KERR_INVAL);
    KTEST_CHECK(xfs_path_lookup(&fs, "/nothing", &ip) == KERR_NOENT);
    KTEST_CHECK(xfs_path_lookup(&fs, "/file.txt/inside", &ip) == KERR_NOTDIR);
    KTEST_CHECK(xfs_path_lookup(&fs, "/a/b/c/d/e/f/g/h/i/j/k/l/m/n/o/p/q", &ip) == KERR_NOENT);
}

/* A directory's bytes are structure, not content. */
static void test_a_directory_is_not_a_file(void) {
    struct xfs_inode root;
    uint8_t buf[16];
    uint32_t got = 0;
    mount_fresh();
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &root) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &root, 0, buf, sizeof buf, &got) == KERR_ISDIR);
    KTEST_CHECK(got == 0);
}

/* Holes and unwritten extents are zeroes, not whatever is on the disk. */
static void test_holes_and_unwritten_read_as_zeroes(void) {
    struct xfs_inode ip;
    uint8_t buf[8];
    uint32_t got = 0;
    uint8_t *di;
    build_fs();
    memset(block(FILE_FSB), 0xcc, BS);
    di = build_inode(FILE_INO, 0100644u, XFS_DINODE_FMT_EXTENTS, BS * 3u);
    set_extent(di, 0, 2u, FILE_FSB, 1u, true);  /* unwritten, and nothing at 0..1 */
    seal(di, ISIZE, XFS_DI_CRC);
    dev = (struct blockdev){
        .read = disk_read, .write = disk_write, .flush = disk_flush, .block_size = BS, .blocks = BLOCKS};
    bcache_init(&cache, &dev, cache_mem, 8u);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    memset(buf, 0xff, sizeof buf);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, sizeof buf, &got) == KERR_OK);
    KTEST_CHECK(got == sizeof buf && buf[0] == 0 && buf[7] == 0);  /* the hole */
    memset(buf, 0xff, sizeof buf);
    KTEST_CHECK(xfs_file_read(&fs, &ip, BS * 2u, buf, sizeof buf, &got) == KERR_OK);
    KTEST_CHECK(got == sizeof buf && buf[0] == 0 && buf[7] == 0);  /* unwritten */
}

/* ── the refusals: a superblock that cannot be trusted ─────────────────── */

static void damage_magic(void) {
    put32(block(0) + 0, 0x12345678u);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_version(void) {
    put16(block(0) + XFS_SB_VERSIONNUM, 0xb004u);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_crc(void) {
    block(0)[XFS_SB_CRC] ^= 0xffu;
}

static void damage_inprogress(void) {
    block(0)[XFS_SB_INPROGRESS] = 1u;
    seal(block(0), 512u, XFS_SB_CRC);
}

/* No damage at all: the control for a test that checks a refusal, so the
 * refusal is shown to be the feature's doing. */
static void damage_nothing(void) {
}

static void damage_reflink(void) {
    put32(block(0) + XFS_SB_FEAT_RO_COMPAT, XFS_FEAT_RO_FINOBT | XFS_FEAT_RO_REFLINK);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_rmapbt(void) {
    put32(block(0) + XFS_SB_FEAT_RO_COMPAT, XFS_FEAT_RO_RMAPBT);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_nrext64(void) {
    put32(block(0) + XFS_SB_FEAT_INCOMPAT, XFS_FEAT_IN_FTYPE | XFS_FEAT_IN_NREXT64);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_spinodes(void) {
    put32(block(0) + XFS_SB_FEAT_INCOMPAT, XFS_FEAT_IN_FTYPE | XFS_FEAT_IN_SPINODES);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_needsrepair(void) {
    put32(block(0) + XFS_SB_FEAT_INCOMPAT, XFS_FEAT_IN_FTYPE | XFS_FEAT_IN_NEEDSREPAIR);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_log_incompat(void) {
    put32(block(0) + XFS_SB_FEAT_LOG_INCOMPAT, 1u);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_unknown_feature(void) {
    put32(block(0) + XFS_SB_FEAT_INCOMPAT, XFS_FEAT_IN_FTYPE | 0x8000u);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_blocksize(void) {
    put32(block(0) + XFS_SB_BLOCKSIZE, 100u);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_blocklog(void) {
    block(0)[XFS_SB_BLOCKLOG] = 11u;  /* does not match blocksize */
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_inopblock(void) {
    put16(block(0) + XFS_SB_INOPBLOCK, 9u);
    seal(block(0), 512u, XFS_SB_CRC);
}

/* A legal inode size that is not this fixture's: 2048, which XFS makes with
 * `-i size=2048` and which this reader refused outright until it learned
 * that the refusal was about its own fork cache rather than about the
 * format. The geometry is kept self-consistent so that the *size* is what
 * is being tested and not the arithmetic around it. */
static void damage_inodesize(void) {
    put16(block(0) + XFS_SB_INODESIZE, 2048u);
    put16(block(0) + XFS_SB_INOPBLOCK, 2u);
    block(0)[XFS_SB_INOPBLOG] = 1u;
    seal(block(0), 512u, XFS_SB_CRC);
}

/* And one XFS does not make: 4096, above the format's own ceiling of 2048.
 *
 * **Its arithmetic is kept consistent deliberately.** The first attempt
 * used 384, which is not a power of two — and `check_geometry` rejected it
 * one line earlier, for `inopblock != blocksize / inodesize`, with
 * `KERR_INVAL`. The test passed nothing: it asserted a refusal that came
 * from a different check than the one being tested. One inode per 4096-byte
 * block adds up perfectly, so the only thing left to refuse it is the size
 * bound itself. */
static void damage_inodesize_impossible(void) {
    put16(block(0) + XFS_SB_INODESIZE, 4096u);
    put16(block(0) + XFS_SB_INOPBLOCK, 1u);
    block(0)[XFS_SB_INOPBLOG] = 0u;
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_sectsize(void) {
    put16(block(0) + XFS_SB_SECTSIZE, 8192u);  /* larger than a block */
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_dblocks(void) {
    put64(block(0) + XFS_SB_DBLOCKS, 1u << 30);  /* far beyond the groups */
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_agcount(void) {
    put32(block(0) + XFS_SB_AGCOUNT, 0);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_agblklog(void) {
    block(0)[XFS_SB_AGBLKLOG] = 2u;  /* 4 blocks per group cannot hold 64 */
    seal(block(0), 512u, XFS_SB_CRC);
}

/* A directory block of two filesystem blocks: a geometry, not damage.
 *
 * `mkfs.xfs -n size=8192` makes one, and it used to be refused at mount.
 * Only the mount is checked here — the synthetic filesystem's root
 * directory is a single block, so *listing* it under this geometry would be
 * asking the reader to gather a directory block the fixture does not have.
 * The board suite carries the real one. */
static void two_block_directory_blocks(void) {
    block(0)[XFS_SB_DIRBLKLOG] = 1u;
    seal(block(0), 512u, XFS_SB_CRC);
}

/* One larger than the buffer that gathers it, which is the largest the
 * format allows — so this is a corrupt superblock rather than a geometry. */
static void damage_dirblklog(void) {
    block(0)[XFS_SB_DIRBLKLOG] = 6u; /* 64 blocks: 256 KiB */
    seal(block(0), 512u, XFS_SB_CRC);
}

/* The promise made when a read-only-compatible feature is allowed through:
 * the filesystem is readable and **not writable**. Reflink means extents
 * can be shared, and a writer that does not implement copy-on-write would
 * corrupt the file that shares them. */
static void test_read_only_features_mount_but_forbid_writing(void) {
    KTEST_CHECK(mount_with(damage_reflink) == KERR_OK);
    KTEST_CHECK(!xfs_writable(&fs));
    KTEST_CHECK(mount_with(damage_rmapbt) == KERR_OK);
    KTEST_CHECK(!xfs_writable(&fs));
    /* A filesystem with none of them stays writable, so the refusal is the
     * feature's doing and not a mount that is read-only anyway. */
    KTEST_CHECK(mount_with(damage_nothing) == KERR_OK);
    KTEST_CHECK(xfs_writable(&fs));
}

static void test_a_superblock_that_cannot_be_trusted(void) {
    KTEST_CHECK(mount_with(damage_magic) == KERR_INVAL);
    KTEST_CHECK(mount_with(damage_version) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(damage_crc) == KERR_IO);
    KTEST_CHECK(mount_with(damage_inprogress) == KERR_INVAL);
    /* Reflink and reverse mapping are **read-only-compatible** features:
     * the format says a reader may ignore them so long as it does not
     * write, and this reader does not. So they mount — and the obligation
     * that buys the mount is checked next door, in
     * `test_read_only_features_mount_but_forbid_writing`.
     *
     * These two lines used to assert a refusal, which made the reader
     * usable only on a filesystem formatted specially for it: `mkfs.xfs`
     * turns both on by default, so an ordinary drive was rejected. A test
     * that encodes an assumption protects the assumption, not the user. */
    KTEST_CHECK(mount_with(damage_reflink) == KERR_OK);
    KTEST_CHECK(mount_with(damage_rmapbt) == KERR_OK);
    /* Both are readable now, each for its own reason: `NREXT64` moves two
     * fields, which is handled where they are read, and `SPINODES` changes
     * inode-b-tree records this reader never consults. See
     * `XFS_FEAT_IN_SUPPORTED`. */
    KTEST_CHECK(mount_with(damage_nrext64) == KERR_OK);
    KTEST_CHECK(mount_with(damage_spinodes) == KERR_OK);
    KTEST_CHECK(mount_with(damage_needsrepair) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(damage_log_incompat) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(damage_unknown_feature) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(damage_blocksize) == KERR_INVAL);
    KTEST_CHECK(mount_with(damage_blocklog) == KERR_INVAL);
    KTEST_CHECK(mount_with(damage_inopblock) == KERR_INVAL);
    /* **A legal inode size mounts**, whatever this reader caches.
     *
     * This line used to expect `KERR_NOTSUP`, protecting a refusal of every
     * 1024- and 2048-byte-inode filesystem — and the refusal was never
     * about the format: it was about `struct xfs_inode` carrying a copy of
     * the literal area, which cannot grow past the kernel's frame limit.
     * That is a limit on this reader, so it belongs on the inodes that
     * exceed it rather than on the whole filesystem. A size XFS cannot have
     * is still refused, which is the line below. */
    KTEST_CHECK(mount_with(damage_inodesize) == KERR_OK);
    KTEST_CHECK(mount_with(damage_inodesize_impossible) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(damage_sectsize) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(damage_dblocks) == KERR_INVAL);
    KTEST_CHECK(mount_with(damage_agcount) == KERR_INVAL);
    KTEST_CHECK(mount_with(damage_agblklog) == KERR_INVAL);
    KTEST_CHECK(mount_with(damage_dirblklog) == KERR_NOTSUP);
    KTEST_CHECK(mount_with(two_block_directory_blocks) == KERR_OK);
    KTEST_CHECK(reads_outside == 0);
}

/* ── the log ───────────────────────────────────────────────────────────── */

static void damage_log_not_unmount(void) {
    block(LOG_FSB)[XLOG_SECTOR + XLOG_OP_FLAGS] = 0;  /* an ordinary transaction */
}

static void damage_log_wrong_client(void) {
    block(LOG_FSB)[XLOG_SECTOR + XLOG_OP_CLIENTID] = 0x69u;
}

static void damage_log_many_ops(void) {
    put32(block(LOG_FSB) + XLOG_REC_NUM_OPS, 7u);
}

static void damage_log_newer_record(void) {
    /* A record after the unmount: a crash leaves exactly this. */
    uint8_t *bb = block(LOG_FSB) + 2u * XLOG_SECTOR;
    put32(bb + XLOG_REC_MAGIC, XLOG_MAGIC);
    put32(bb + XLOG_REC_CYCLE, 1u);
    put32(bb + XLOG_REC_LEN, 512u);
    put32(bb + XLOG_REC_NUM_OPS, 3u);
}

static void damage_log_empty(void) {
    memset(block(LOG_FSB), 0, LOG_BLOCKS * BS);
}

static void damage_log_external(void) {
    put64(block(0) + XFS_SB_LOGSTART, 0);
    seal(block(0), 512u, XFS_SB_CRC);
}

static void damage_log_no_record(void) {
    /* Blocks claiming the cycle, but no record header anywhere. */
    memset(block(LOG_FSB), 0, LOG_BLOCKS * BS);
    for (uint32_t bb = 0; bb < 8u; bb++) {
        put32(block(LOG_FSB) + bb * XLOG_SECTOR, 1u);
    }
}

/* A dirty log sends the mount through the check instead of refusing it.
 *
 * This test used to assert `KERR_NOTSUP` for each of these — it protected
 * the old behaviour of refusing any unclean filesystem, which made a power
 * cut as damaging as a broken disk. The behaviour deliberately changed: an
 * unclean filesystem is checked, and mounted **read-only** if it is sound.
 *
 * What this fixture can show is the first half. It is a synthetic
 * filesystem with a superblock, a log and inodes and **no group headers at
 * all**, so the check correctly finds it unsound — which is why `KERR_DIRTY`
 * is the right answer here and not a weaker version of `KERR_OK`. The
 * read-only mount of a *sound* unclean filesystem needs a filesystem
 * `mkfs.xfs` made, so it is proved on the board, where this project's rule
 * says such things are proved anyway.
 *
 * The distinction being asserted is `KERR_DIRTY` against `KERR_NOTSUP`:
 * the mount reached the checker rather than stopping at the log. */
/* A second device, holding nothing but a log: what an **external** log is.
 *
 * Separate storage, a separate `blockdev` and a separate cache, because
 * that is the whole of the configuration — the filesystem's superblock says
 * `logstart == 0` and names nothing, so the log's device is something the
 * kernel is handed rather than something it can find. */
static uint8_t logdisk[LOG_BLOCKS * BS];
static uint8_t logcache_mem[4u * BS];
static struct bcache logcache;
static struct blockdev logdev;

static int logdisk_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!blockdev_in_range(&logdev, lba, count)) {
        reads_outside++;
        return KERR_INVAL;
    }
    memcpy(buf, logdisk + lba * BS, count * BS);
    return KERR_OK;
}

static int logdisk_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!blockdev_in_range(&logdev, lba, count)) {
        reads_outside++;
        return KERR_INVAL;
    }
    memcpy(logdisk + lba * BS, buf, count * BS);
    return KERR_OK;
}

/* The same clean log the internal one gets, but at **its own device's first
 * byte** rather than at `logstart`. That offset is the one thing an
 * external log changes about reading it, which is why the reader takes a
 * base rather than computing one. */
static void build_external_log(void) {
    memset(logdisk, 0, sizeof logdisk);
    put32(logdisk + XLOG_REC_MAGIC, XLOG_MAGIC);
    put32(logdisk + XLOG_REC_CYCLE, 1u);
    put32(logdisk + XLOG_REC_VERSION, 2u);
    put32(logdisk + XLOG_REC_LEN, 512u);
    put32(logdisk + XLOG_REC_NUM_OPS, 1u);
    put32(logdisk + XLOG_SECTOR + XLOG_OP_LEN, 8u);
    logdisk[XLOG_SECTOR + XLOG_OP_CLIENTID] = XLOG_CLIENT_LOG;
    logdisk[XLOG_SECTOR + XLOG_OP_FLAGS] = XLOG_UNMOUNT_TRANS;
    put16(logdisk + XLOG_SECTOR + XLOG_OP_HDR_SIZE, XLOG_UNMOUNT_MAGIC);
    logdev = (struct blockdev){.read = logdisk_read,
                               .write = logdisk_write,
                               .flush = disk_flush,
                               .block_size = BS,
                               .blocks = LOG_BLOCKS};
    bcache_init(&logcache, &logdev, logcache_mem, 4u);
}

/* An external log: read-only until its device arrives, writable after.
 *
 * **Both halves matter.** A filesystem whose log is elsewhere used to be
 * refused outright, which made a configuration somebody chose look like a
 * corrupt disk — so the first half is that it mounts at all, and read-only,
 * because a log that cannot be read is exactly the position a dirty log
 * leaves things in. The second half is that attaching the device lifts
 * that: otherwise "supported" would mean "mounts and can never be written",
 * which is not support.
 *
 * The device is deliberately a *different* one, with its own storage and
 * its own range check, so a reader that quietly used the filesystem's own
 * device would read the wrong bytes and be caught rather than pass. */
/* A quota file with one record in it, for user id 7.
 *
 * The record's **place is its id**, so id 7 lives at `7 * 136` and the file
 * has to be at least that long — which is why a real quota file is sparse
 * and why this one is a LOCAL fork holding only what it must. The inode is
 * given the data fork's extents pointing at a block we fill, because a
 * record at offset 952 does not fit in an inode.
 *
 * `QUOTA_INO` and `QUOTA_FSB` are a spare inode and a spare block of the
 * synthetic filesystem. */
#define QUOTA_INO 104u
#define QUOTA_FSB 14u

static void build_a_quota_file(uint32_t id, uint64_t blk_hard, uint64_t blocks_used) {
    uint8_t *di;
    uint8_t *blk = block(QUOTA_FSB);
    uint32_t at = id * XFS_DQUOT_BYTES;
    struct xfs_extent e = {.startoff = 0, .startblock = QUOTA_FSB, .blockcount = 1};
    uint8_t *r;
    memset(blk, 0, BS);
    r = blk + at;
    put16(r + XFS_DQ_MAGIC, XFS_DQUOT_MAGIC);
    r[XFS_DQ_VERSION] = XFS_DQUOT_VERSION;
    r[XFS_DQ_TYPE] = XFS_DQTYPE_USER;
    put32(r + XFS_DQ_ID, id);
    put64(r + XFS_DQ_BLK_HARD, blk_hard);
    put64(r + XFS_DQ_BLK_SOFT, blk_hard / 2u);
    put64(r + XFS_DQ_INO_HARD, 1000u);
    put64(r + XFS_DQ_BCOUNT, blocks_used);
    put64(r + XFS_DQ_ICOUNT, 3u);
    memcpy(r + XFS_DQ_UUID_OFF, block(0) + XFS_SB_UUID, 16);
    seal(r, XFS_DQUOT_BYTES, XFS_DQ_CRC_OFF);

    di = build_inode(QUOTA_INO, 0100600u, XFS_DINODE_FMT_EXTENTS,
                     (uint64_t)at + XFS_DQUOT_BYTES);
    put32(di + XFS_DI_NEXTENTS, 1u);
    put64(di + XFS_DI_NBLOCKS, 1u);
    xfs_extent_pack(di + XFS_DI_V3_SIZE, &e);
    seal(di, ISIZE, XFS_DI_CRC);

    put64(block(0) + XFS_SB_UQUOTINO, QUOTA_INO);
    put16(block(0) + XFS_SB_QFLAGS, XFS_UQUOTA_ACCT | XFS_UQUOTA_ENFD);
    seal(block(0), 512u, XFS_SB_CRC);
}

/* Quotas: what the filesystem asks for, and one record read out of it.
 *
 * **A fresh quota filesystem is the normal case and reads as "not yet".**
 * `mkfs.xfs -m uquota` sets the flags and leaves the inode numbers zero —
 * a kernel makes the files on first mount — so "counted, but no file" is a
 * true answer and `KERR_NODEV` is how it is said. Returning `KERR_NOENT`
 * there would conflate it with an id that simply has no record, which is
 * the ordinary case on a populated file. */
static void test_quotas_are_read(void) {
    struct xfs_dquot dq;
    mount_fresh();
    put16(block(0) + XFS_SB_QFLAGS, XFS_UQUOTA_ACCT | XFS_UQUOTA_ENFD);
    seal(block(0), 512u, XFS_SB_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_quota_accounted(&fs, XFS_DQTYPE_USER));
    KTEST_CHECK(xfs_quota_enforced(&fs, XFS_DQTYPE_USER));
    KTEST_CHECK(!xfs_quota_accounted(&fs, XFS_DQTYPE_GROUP));
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_USER, 7u, &dq) == KERR_NODEV);
    /* A kind the filesystem does not count is refused differently again. */
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_GROUP, 7u, &dq) == KERR_NOTSUP);

    /* And with a file: the record for id 7 reads back with its limits. */
    mount_fresh();
    build_a_quota_file(7u, 4096u, 19u);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_USER, 7u, &dq) == KERR_OK);
    KTEST_CHECK(dq.id == 7u);
    KTEST_CHECK(dq.blocks_hard == 4096u);
    KTEST_CHECK(dq.blocks_soft == 2048u);
    KTEST_CHECK(dq.blocks_used == 19u);
    KTEST_CHECK(dq.inodes_used == 3u);

    /* An id whose record was never written is a hole, and a hole is not an
     * error: a quota file is sparse by construction and most ids are holes.
     * Id 6 shares the block with 7 and is zeroes. */
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_USER, 6u, &dq) == KERR_NOENT);
    /* An id past the file's end is the same answer by a different route. */
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_USER, 9000u, &dq) == KERR_NOENT);

    /* A record whose checksum does not match must not be believed: a torn
     * one would otherwise read as a limit nobody set. */
    mount_fresh();
    build_a_quota_file(7u, 4096u, 19u);
    block(QUOTA_FSB)[7u * XFS_DQUOT_BYTES + XFS_DQ_CRC_OFF] ^= 0xffu;
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_USER, 7u, &dq) == KERR_IO);

    /* And a record of the wrong *kind* sitting in the user file is wrong
     * however good its checksum: the id cannot tell them apart, so the
     * type is what catches a group record in the user's file. */
    mount_fresh();
    build_a_quota_file(7u, 4096u, 19u);
    block(QUOTA_FSB)[7u * XFS_DQUOT_BYTES + XFS_DQ_TYPE] = XFS_DQTYPE_GROUP;
    seal(block(QUOTA_FSB) + 7u * XFS_DQUOT_BYTES, XFS_DQUOT_BYTES, XFS_DQ_CRC_OFF);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_quota_get(&fs, XFS_DQTYPE_USER, 7u, &dq) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

static void test_an_external_log_is_read_from_its_own_device(void) {
    mount_fresh();
    put64(block(0) + XFS_SB_LOGSTART, 0);
    seal(block(0), 512u, XFS_SB_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(fs.log_detached);
    KTEST_CHECK(!xfs_writable(&fs));

    build_external_log();
    KTEST_CHECK(xfs_attach_log(&fs, &logcache) == KERR_OK);
    KTEST_CHECK(!fs.log_detached);
    KTEST_CHECK(xfs_writable(&fs));
    KTEST_CHECK(reads_outside == 0);

    /* And a filesystem whose log is *internal* refuses the attachment,
     * rather than quietly pointing its log reader at a device that holds
     * somebody else's. */
    mount_fresh();
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_attach_log(&fs, &logcache) == KERR_NOTSUP);
    KTEST_CHECK(reads_outside == 0);
}

static void test_a_dirty_log_is_checked(void) {
    void (*dirty[])(void) = {damage_log_not_unmount, damage_log_wrong_client,
                             damage_log_many_ops, damage_log_newer_record};
    for (uint32_t i = 0; i < sizeof dirty / sizeof dirty[0]; i++) {
        KTEST_CHECK(mount_with(dirty[i]) == KERR_DIRTY);
    }
    /* **An external log mounts, read-only.** It is a configuration
     * somebody chose, not a defect: the log is on a device the superblock
     * does not name, so until one is attached the log cannot be read — and
     * a log that cannot be read leaves the filesystem exactly where a dirty
     * one does, readable and not to be written. This line used to expect
     * `KERR_NOTSUP`, which made a deliberate choice indistinguishable from
     * a corrupt disk. */
    KTEST_CHECK(mount_with(damage_log_external) == KERR_OK);
    KTEST_CHECK(fs.log_detached);
    KTEST_CHECK(!xfs_writable(&fs));
    /* A log this reader cannot find a record in is still a refusal, and a
     * different kind: it is not a statement about cleanliness at all. */
    KTEST_CHECK(mount_with(damage_log_no_record) == KERR_NOTSUP);
    /* A log that was never written has nothing to replay, so it is clean
     * and the check never runs — which is what lets this fixture, with no
     * group headers in it, mount at all. */
    KTEST_CHECK(mount_with(damage_log_empty) == KERR_OK);
    KTEST_CHECK(!fs.log_was_dirty);
    KTEST_CHECK(reads_outside == 0);
}

/* The check reads a group header that is not there and says so, rather than
 * reading past the end of the filesystem to find one. */
static void test_the_check_stays_inside_the_filesystem(void) {
    KTEST_CHECK(mount_with(damage_log_not_unmount) == KERR_DIRTY);
    KTEST_CHECK(reads_outside == 0);
}

/* ── inodes ────────────────────────────────────────────────────────────── */

static void test_an_inode_that_cannot_be_trusted(void) {
    struct xfs_inode ip;
    uint8_t *di;
    mount_fresh();
    di = inode_at(ROOT_INO + 5u);  /* an empty slot: all zeroes */
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO + 5u, &ip) == KERR_IO);

    /* The right shape, but it is not the inode we asked for. */
    mount_fresh();
    di = inode_at(ROOT_INO);
    put64(di + XFS_DI_INO, 999u);
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_IO);

    /* A version this layout does not describe. */
    mount_fresh();
    di = inode_at(ROOT_INO);
    di[XFS_DI_VERSION] = 2u;
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_IO);

    /* Another filesystem's inode, carried in by a copy. */
    mount_fresh();
    di = inode_at(ROOT_INO);
    di[XFS_DI_UUID] ^= 0xffu;
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_IO);

    /* A damaged checksum. */
    mount_fresh();
    inode_at(ROOT_INO)[XFS_DI_CRC] ^= 0xffu;
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_IO);
}

/* A file whose extent map is a b-tree, built by hand.
 *
 * `BTREE_FSB` holds one leaf block with a single record pointing at the
 * file's data block, and the inode's fork holds the root that points at it.
 * The arrays are the part worth getting right: the root's pointers begin
 * after **as many keys as the fork could hold**, not after the one key in
 * use, and a reader that assumes otherwise follows a key as if it were a
 * block number. */
#define BTREE_FSB 12u

static void build_a_btree_fork(uint64_t ino, uint64_t data_fsb) {
    uint8_t *leaf = block(BTREE_FSB);
    uint8_t *di = inode_at(ino);
    uint8_t *fork = di + XFS_DI_V3_SIZE;
    uint32_t maxrecs = (ISIZE - XFS_DI_V3_SIZE - XFS_BMDR_HDR_BYTES) / 16u;
    struct xfs_extent e = {.startoff = 0, .startblock = data_fsb, .blockcount = 1};
    memset(leaf, 0, BS);
    put32(leaf, XFS_BMBT_CRC_MAGIC);
    put16(leaf + 4, 0);  /* level 0: a leaf */
    put16(leaf + 6, 1);  /* one record */
    put64(leaf + 8, ~0ull);
    put64(leaf + 16, ~0ull);
    put64(leaf + 24, BTREE_FSB);
    memcpy(leaf + XFS_BMBT_UUID_OFF, block(0) + XFS_SB_UUID, 16);
    put64(leaf + 56, ino);
    xfs_extent_pack(leaf + XFS_BMBT_BLOCK_BYTES, &e);
    seal(leaf, BS, XFS_BMBT_CRC_OFF);

    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_BTREE;
    memset(fork, 0, ISIZE - XFS_DI_V3_SIZE);
    put16(fork, 1);      /* level 1: a root over leaves */
    put16(fork + 2, 1);  /* one key and one pointer */
    put64(fork + XFS_BMDR_HDR_BYTES, 0);  /* the key: logical block 0 */
    put64(fork + XFS_BMDR_HDR_BYTES + maxrecs * XFS_BMBT_KEY_BYTES, BTREE_FSB);
    seal(di, ISIZE, XFS_DI_CRC);
}

/* **The format that used to be refused outright.** A 5,000-entry directory
 * is enough to put an extent map in a b-tree, so this was not an exotic
 * case: the root directory of such a filesystem could not be read at all. */
/* The same tree, but as the **attribute** fork.
 *
 * `forkoff` puts it above the data fork, `di_aformat` says it is a b-tree,
 * and the root goes at `forkoff * 8` instead of at the start of the literal
 * area. Everything below the inode — the leaf block, its keys, its records
 * — is identical, which is the point: an inode has two forks and either can
 * be a b-tree, so the descent should not care which it is given. */
static void build_a_btree_attr_fork(uint64_t ino, uint64_t data_fsb) {
    uint8_t *leaf = block(BTREE_FSB);
    uint8_t *di = inode_at(ino);
    uint32_t forkoff = 16u; /* 128 bytes in: clear of a small data fork */
    uint8_t *fork = di + XFS_DI_V3_SIZE + forkoff * 8u;
    uint32_t room = ISIZE - XFS_DI_V3_SIZE - forkoff * 8u;
    uint32_t maxrecs = (room - XFS_BMDR_HDR_BYTES) / 16u;
    struct xfs_extent e = {.startoff = 0, .startblock = data_fsb, .blockcount = 1};
    memset(leaf, 0, BS);
    put32(leaf, XFS_BMBT_CRC_MAGIC);
    put16(leaf + 4, 0); /* level 0: a leaf */
    put16(leaf + 6, 1); /* one record */
    put64(leaf + 8, ~0ull);
    put64(leaf + 16, ~0ull);
    put64(leaf + 24, BTREE_FSB);
    memcpy(leaf + XFS_BMBT_UUID_OFF, block(0) + XFS_SB_UUID, 16);
    put64(leaf + 56, ino);
    xfs_extent_pack(leaf + XFS_BMBT_BLOCK_BYTES, &e);
    seal(leaf, BS, XFS_BMBT_CRC_OFF);

    di[XFS_DI_FORKOFF] = (uint8_t)forkoff;
    di[XFS_DI_AFORMAT] = XFS_DINODE_FMT_BTREE;
    memset(fork, 0, room);
    put16(fork, 1);     /* level 1: a root over leaves */
    put16(fork + 2, 1); /* one key and one pointer */
    put64(fork + XFS_BMDR_HDR_BYTES, 0);
    put64(fork + XFS_BMDR_HDR_BYTES + maxrecs * XFS_BMBT_KEY_BYTES, BTREE_FSB);
    seal(di, ISIZE, XFS_DI_CRC);
}

/* A b-tree **attribute** fork is walked, not refused.
 *
 * This shape cannot be made on a real filesystem by anything here —
 * producing one needs hundreds of attributes on one inode, and this
 * kernel's writer answers `KERR_NOSPC` before the fork would convert. So it
 * is built by hand, which is the honest way to test a reader for a
 * structure nothing available can produce. The alternative was to leave the
 * format refused and the code unwritten. */
static void test_a_btree_attribute_fork_is_walked(void) {
    struct xfs_inode ip;
    struct xfs_extent e;
    mount_fresh();
    build_a_btree_attr_fork(FILE_INO, FILE_FSB);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(ip.aformat == XFS_DINODE_FMT_BTREE);
    KTEST_CHECK(ip.forkoff == 16u);
    KTEST_CHECK(xfs_attr_bmap(&fs, &ip, 0, &e) == KERR_OK);
    KTEST_CHECK(e.startblock == FILE_FSB);
    KTEST_CHECK(e.blockcount == 1u);

    /* And the same refusals the data fork's tree gets, because it is the
     * same descent: a root claiming to be a leaf is corrupt rather than a
     * one-block tree, since a map that fits in the inode uses the extents
     * format and never reaches here. */
    mount_fresh();
    build_a_btree_attr_fork(FILE_INO, FILE_FSB);
    put16(inode_at(FILE_INO) + XFS_DI_V3_SIZE + 16u * 8u, 0);
    seal(inode_at(FILE_INO), ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_attr_bmap(&fs, &ip, 0, &e) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

static void test_a_btree_extent_fork_reads(void) {
    struct xfs_inode ip;
    char buf[8];
    uint32_t got = 0;
    mount_fresh();
    build_a_btree_fork(FILE_INO, FILE_FSB);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(ip.format == XFS_DINODE_FMT_BTREE);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, 5, &got) == KERR_OK);
    KTEST_CHECK(got == 5);
    KTEST_CHECK(memcmp(buf, "01234", 5) == 0);

    /* A tree whose root claims to be a leaf is corrupt, not a one-block
     * tree: the extents format is what a map that fits in the inode uses,
     * so level 0 here can only mean the bytes are not a b-tree root. */
    mount_fresh();
    build_a_btree_fork(FILE_INO, FILE_FSB);
    put16(inode_at(FILE_INO) + XFS_DI_V3_SIZE, 0);
    seal(inode_at(FILE_INO), ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, 5, &got) == KERR_IO);

    /* A leaf whose checksum does not match must not be read: it is a
     * pointer to follow, which makes believing it worse than believing a
     * bad record. */
    mount_fresh();
    build_a_btree_fork(FILE_INO, FILE_FSB);
    block(BTREE_FSB)[XFS_BMBT_CRC_OFF] ^= 0xffu;
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, 5, &got) == KERR_IO);

    /* A pointer outside the filesystem. */
    mount_fresh();
    build_a_btree_fork(FILE_INO, FILE_FSB);
    {
        uint32_t maxrecs = (ISIZE - XFS_DI_V3_SIZE - XFS_BMDR_HDR_BYTES) / 16u;
        uint8_t *fork = inode_at(FILE_INO) + XFS_DI_V3_SIZE;
        put64(fork + XFS_BMDR_HDR_BYTES + maxrecs * XFS_BMBT_KEY_BYTES, BLOCKS + 99u);
        seal(inode_at(FILE_INO), ISIZE, XFS_DI_CRC);
    }
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, 5, &got) == KERR_IO);
}

static void test_inode_formats_we_refuse(void) {
    struct xfs_inode ip;
    uint8_t *di;
    /* **A fork that says b-tree and holds extent records is corrupt.** It
     * used to be refused for being a b-tree at all; now it is refused for
     * not holding one, which is the check that still protects a reader —
     * the bytes are a plausible-looking tree and following them would read
     * the file's own data as block numbers. */
    mount_fresh();
    di = inode_at(ROOT_INO);
    di[XFS_DI_FORMAT] = XFS_DINODE_FMT_BTREE;
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_OK);
    {
        struct seen seen = {0};
        KTEST_CHECK(xfs_dir_list(&fs, &ip, count_entry, &seen) == KERR_IO);
    }

    /* **An inode with 64-bit extent counts reads, and reads them from the
     * right place.** The count does not widen where it was: it moves to the
     * eight bytes at offset 24 that a v3 inode otherwise pads, and the old
     * four-byte slot becomes the *attribute* fork's. Reading the old offset
     * on such an inode yields the attribute count — nearly always zero — so
     * the file looks empty rather than failing, which is why this checks the
     * value and not merely the absence of an error. */
    mount_fresh();
    di = inode_at(ROOT_INO);
    put64(di + XFS_DI_FLAGS2, XFS_DIFLAG2_NREXT64);
    put64(di + XFS_DI_BIG_NEXTENTS, 3u);
    put32(di + XFS_DI_NEXTENTS, 99u); /* the attribute fork's, once NREXT64 is set */
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_OK);
    KTEST_CHECK(ip.nextents == 3u);
    KTEST_CHECK(ip.big_extent_counts);

    /* And without the flag, the same inode's count comes from the old slot,
     * so one layout is never read as the other. */
    mount_fresh();
    di = inode_at(ROOT_INO);
    put32(di + XFS_DI_NEXTENTS, 7u);
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_OK);
    KTEST_CHECK(ip.nextents == 7u);
    KTEST_CHECK(!ip.big_extent_counts);
}

/* An inode that claims more content, or more extents, than it has room
 * for: the classic way a parser is walked off the end of a structure. */
static void test_an_inode_that_claims_too_much(void) {
    struct xfs_inode ip;
    uint8_t *di;
    mount_fresh();
    di = inode_at(SUB_INO);  /* the shortform directory's block */
    put64(di + XFS_DI_SIZE, 4000u);
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, 48u, &ip) == KERR_IO);

    mount_fresh();
    di = inode_at(ROOT_INO);
    put32(di + XFS_DI_NEXTENTS, 1000u);
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &ip) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

/* An inode number that is not in the filesystem at all. */
static void test_an_inode_outside_the_filesystem(void) {
    struct xfs_inode ip;
    mount_fresh();
    KTEST_CHECK(xfs_inode_read(&fs, 0xffffffffu, &ip) == KERR_NOENT);
    KTEST_CHECK(xfs_inode_read(&fs, (uint64_t)1 << 40, &ip) == KERR_NOENT);
    KTEST_CHECK(reads_outside == 0);
}

/* ── extents ───────────────────────────────────────────────────────────── */

static void test_extent_unpacking(void) {
    uint8_t rec[16];
    struct xfs_extent e;
    /* The record a real 200000-byte file had: offset 0, block 20, 49
     * blocks (XFS.md §2). */
    memset(rec, 0, sizeof rec);
    rec[12] = 0x02;
    rec[13] = 0x80;
    rec[14] = 0x00;
    rec[15] = 0x31;
    xfs_extent_unpack(rec, &e);
    KTEST_CHECK(e.startoff == 0 && e.startblock == 20u && e.blockcount == 49u && !e.unwritten);

    /* Every field at once, including the unwritten flag. */
    memset(rec, 0, sizeof rec);
    put64(rec, ((uint64_t)1 << 63) | ((uint64_t)0x123456 << 9) | 1u);
    put64(rec + 8, ((uint64_t)0x89abcdef << 21) | 0x1ffffu);
    xfs_extent_unpack(rec, &e);
    KTEST_CHECK(e.unwritten);
    KTEST_CHECK(e.startoff == 0x123456u);
    KTEST_CHECK(e.startblock == (((uint64_t)1 << 43) | 0x89abcdefu));
    KTEST_CHECK(e.blockcount == 0x1ffffu);
}

static void test_an_extent_outside_the_filesystem(void) {
    struct xfs_inode ip;
    uint8_t buf[16];
    uint32_t got = 0;
    uint8_t *di;
    build_fs();
    di = build_inode(FILE_INO, 0100644u, XFS_DINODE_FMT_EXTENTS, 100u);
    set_extent(di, 0, 0, BLOCKS + 10u, 1u, false);  /* past the end of the device */
    seal(di, ISIZE, XFS_DI_CRC);
    dev = (struct blockdev){
        .read = disk_read, .write = disk_write, .flush = disk_flush, .block_size = BS, .blocks = BLOCKS};
    bcache_init(&cache, &dev, cache_mem, 8u);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, sizeof buf, &got) == KERR_IO);
    KTEST_CHECK(got == 0);
    KTEST_CHECK(reads_outside == 0);  /* refused before the device was asked */
}

/* A count of zero, and one that would wrap past the end of the device. */
static void test_extent_counts(void) {
    struct xfs_inode ip;
    uint8_t buf[16];
    uint32_t got = 0;
    uint8_t *di;
    build_fs();
    di = build_inode(FILE_INO, 0100644u, XFS_DINODE_FMT_EXTENTS, 100u);
    set_extent(di, 0, 0, FILE_FSB, 0, false);
    seal(di, ISIZE, XFS_DI_CRC);
    dev = (struct blockdev){
        .read = disk_read, .write = disk_write, .flush = disk_flush, .block_size = BS, .blocks = BLOCKS};
    bcache_init(&cache, &dev, cache_mem, 8u);
    KTEST_CHECK(xfs_mount(&fs, &cache) == KERR_OK);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, sizeof buf, &got) == KERR_IO);

    build_fs();
    di = build_inode(FILE_INO, 0100644u, XFS_DINODE_FMT_EXTENTS, 100u);
    set_extent(di, 0, 0, BLOCKS - 1u, 0x1fffffu, false);
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, FILE_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_file_read(&fs, &ip, 0, buf, sizeof buf, &got) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

/* ── directories ───────────────────────────────────────────────────────── */

static void test_a_directory_block_that_cannot_be_trusted(void) {
    struct xfs_inode root;
    struct seen seen = {0};
    mount_fresh();
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &root) == KERR_OK);

    /* Another directory's block, reached by a mangled extent. */
    put64(block(DIR_FSB) + XFS_DIR3_HDR_OWNER, 999u);
    seal(block(DIR_FSB), BS, XFS_DIR3_HDR_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_IO);

    /* A block that is not a directory block at all. */
    mount_fresh();
    put32(block(DIR_FSB) + XFS_DIR3_HDR_MAGIC, 0x41424344u);
    seal(block(DIR_FSB), BS, XFS_DIR3_HDR_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_IO);

    /* A damaged checksum. */
    mount_fresh();
    block(DIR_FSB)[XFS_DIR3_HDR_CRC] ^= 0xffu;
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

/* Entries whose lengths do not fit the block they are in. */
static void test_directory_entries_that_claim_too_much(void) {
    struct xfs_inode root;
    struct seen seen = {0};
    mount_fresh();
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &root) == KERR_OK);

    /* A name of 255 bytes where a one-byte name was: every entry after it
     * is then misaligned. Either answer is acceptable — what is not is a
     * read outside the block, which the device below would record. */
    block(DIR_FSB)[XFS_DIR3_DATA_HDR_SIZE + 8u] = 255u;
    seal(block(DIR_FSB), BS, XFS_DIR3_HDR_CRC);
    bcache_invalidate(&cache);
    seen.count = 0;
    {
        int rc = xfs_dir_list(&fs, &root, count_entry, &seen);
        KTEST_CHECK(rc == KERR_OK || rc == KERR_IO);
    }

    /* A name length of zero, which would never advance. */
    mount_fresh();
    block(DIR_FSB)[XFS_DIR3_DATA_HDR_SIZE + 8u] = 0;
    seal(block(DIR_FSB), BS, XFS_DIR3_HDR_CRC);
    bcache_invalidate(&cache);
    seen.count = 0;
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_IO);
    KTEST_CHECK(seen.count == 0);

    /* An unused run of length zero: the same trap, by the other path. */
    mount_fresh();
    put16(block(DIR_FSB) + XFS_DIR3_DATA_HDR_SIZE, XFS_DIR2_DATA_FREE_TAG);
    put16(block(DIR_FSB) + XFS_DIR3_DATA_HDR_SIZE + 2u, 0);
    seal(block(DIR_FSB), BS, XFS_DIR3_HDR_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

/* A single-block directory's tail says how much of the block is entries. */
static void test_a_block_tail_that_lies(void) {
    struct xfs_inode root;
    struct seen seen = {0};
    mount_fresh();
    KTEST_CHECK(xfs_inode_read(&fs, ROOT_INO, &root) == KERR_OK);
    put32(block(DIR_FSB) + BS - XFS_DIR2_BLOCK_TAIL_SIZE, 0xffffffffu);
    seal(block(DIR_FSB), BS, XFS_DIR3_HDR_CRC);
    bcache_invalidate(&cache);
    /* Nothing can be read, and nothing is invented. */
    KTEST_CHECK(xfs_dir_list(&fs, &root, count_entry, &seen) == KERR_OK);
    KTEST_CHECK(seen.count == 0);
    KTEST_CHECK(reads_outside == 0);
}

/* A shortform directory whose entries run past the inode. */
static void test_a_shortform_directory_that_claims_too_much(void) {
    struct xfs_inode ip;
    struct seen seen = {0};
    uint8_t *di;
    mount_fresh();
    di = inode_at(SUB_INO);
    di[XFS_DI_V3_SIZE + XFS_DIR_SF_COUNT] = 40u;  /* far more than it holds */
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, SUB_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_dir_list(&fs, &ip, count_entry, &seen) == KERR_IO);

    /* A size smaller than the header itself. */
    mount_fresh();
    di = inode_at(SUB_INO);
    put64(di + XFS_DI_SIZE, 3u);
    seal(di, ISIZE, XFS_DI_CRC);
    bcache_invalidate(&cache);
    KTEST_CHECK(xfs_inode_read(&fs, SUB_INO, &ip) == KERR_OK);
    KTEST_CHECK(xfs_dir_list(&fs, &ip, count_entry, &seen) == KERR_IO);
    KTEST_CHECK(reads_outside == 0);
}

/* Every byte of the filesystem, flipped one at a time in the structures
 * that matter: the reader must either refuse or read something sane, and
 * must never read outside the device or spin. */
static void test_fuzz_does_not_crash(void) {
    uint32_t state = 0x13572468u;
    for (uint32_t round = 0; round < 3000u; round++) {
        struct xfs_inode ip;
        struct seen seen = {0};
        uint8_t buf[64];
        uint32_t got = 0;
        uint32_t at, byte;
        build_fs();
        state = state * 1664525u + 1013904223u;
        /* Somewhere in the first eight blocks: the superblock, the inodes,
         * the directory, the log. */
        at = (state >> 8) % (8u * BS);
        byte = (state >> 3) & 0xffu;
        disk[at] = (uint8_t)byte;
        dev = (struct blockdev){.read = disk_read,
                                .write = disk_write,
                                .flush = disk_flush,
                                .block_size = BS,
                                .blocks = BLOCKS};
        bcache_init(&cache, &dev, cache_mem, 8u);
        if (xfs_mount(&fs, &cache) != KERR_OK) {
            continue;
        }
        if (xfs_inode_read(&fs, fs.rootino, &ip) == KERR_OK) {
            (void)xfs_dir_list(&fs, &ip, count_entry, &seen);
        }
        if (xfs_path_lookup(&fs, "/file.txt", &ip) == KERR_OK) {
            (void)xfs_file_read(&fs, &ip, 0, buf, sizeof buf, &got);
            KTEST_CHECK(got <= sizeof buf);
        }
    }
    KTEST_CHECK(reads_outside == 0);
}

/* Packing an extent and unpacking it must give back exactly what went in,
 * over the whole range of each field — the boundaries are where the fields
 * do not fall on byte lines, and a shift one bit out survives any single
 * example. */
static void an_extent_round_trips(void) {
    static const struct xfs_extent CASES[] = {
        {0, 0, 1, false},
        {0, 0, 1, true},
        {1, 2, 3, false},
        {(1ull << 54) - 1u, (1ull << 52) - 1u, (1u << 21) - 1u, true},
        {(1ull << 53), (1ull << 51), (1u << 20), false},
        {0x2aaaaaaaaaaaaull, 0x555555555555ull, 0x15555u, true},
        {123456789, 987654321, 49, false},
    };
    for (uint32_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint8_t packed[XFS_EXTENT_BYTES];
        struct xfs_extent back;
        xfs_extent_pack(packed, &CASES[i]);
        xfs_extent_unpack(packed, &back);
        KTEST_CHECK(back.startoff == CASES[i].startoff);
        KTEST_CHECK(back.startblock == CASES[i].startblock);
        KTEST_CHECK(back.blockcount == CASES[i].blockcount);
        KTEST_CHECK(back.unwritten == CASES[i].unwritten);
    }
}

int main(void) {
    KTEST_RUN(an_extent_round_trips);
    KTEST_RUN(test_mount_and_geometry);
    KTEST_RUN(test_list_the_root);
    KTEST_RUN(test_lookup_and_read);
    KTEST_RUN(test_a_shortform_directory);
    KTEST_RUN(test_paths);
    KTEST_RUN(test_a_directory_is_not_a_file);
    KTEST_RUN(test_holes_and_unwritten_read_as_zeroes);
    KTEST_RUN(test_read_only_features_mount_but_forbid_writing);
    KTEST_RUN(test_a_superblock_that_cannot_be_trusted);
    KTEST_RUN(test_quotas_are_read);
    KTEST_RUN(test_an_external_log_is_read_from_its_own_device);
    KTEST_RUN(test_a_dirty_log_is_checked);
    KTEST_RUN(test_the_check_stays_inside_the_filesystem);
    KTEST_RUN(test_an_inode_that_cannot_be_trusted);
    KTEST_RUN(test_inode_formats_we_refuse);
    KTEST_RUN(test_an_inode_that_claims_too_much);
    KTEST_RUN(test_an_inode_outside_the_filesystem);
    KTEST_RUN(test_extent_unpacking);
    KTEST_RUN(test_an_extent_outside_the_filesystem);
    KTEST_RUN(test_extent_counts);
    KTEST_RUN(test_a_directory_block_that_cannot_be_trusted);
    KTEST_RUN(test_directory_entries_that_claim_too_much);
    KTEST_RUN(test_a_block_tail_that_lies);
    KTEST_RUN(test_a_shortform_directory_that_claims_too_much);
    KTEST_RUN(test_a_btree_extent_fork_reads);
    KTEST_RUN(test_a_btree_attribute_fork_is_walked);
    KTEST_RUN(test_fuzz_does_not_crash);
    return ktest_failures();
}
