/* XFS, fuzzed from the card's side: the input is the start of a disk.
 *
 * A filesystem on removable media is the largest input the kernel trusts
 * no further than its checksums. The input is the first bytes of a 512 MiB
 * device whose remainder reads as zeroes, so a seed made from a real
 * `mkfs.xfs` image need only carry the first allocation group's metadata.
 * Writes — a dirty log is replayed at mount — land in a private copy of
 * the input and are dropped past it. The harness mounts, then walks what a
 * user would: the root, each entry, its attributes and its first bytes.
 *
 * **Checksums are resealed before the mount.** On a v5 filesystem nearly
 * every mutation lands in a block whose CRC then fails, and the fuzzer
 * spends its run proving the CRC check works — 482 edges after eighteen
 * million inputs. Resealing the superblock, the AG headers, every inode and
 * every AG b-tree and directory block it can recognise moves the mutations
 * past the checksum into the code that trusts what it covers, which is
 * what an attacker who can compute a CRC32C reaches anyway. */
#include <stdlib.h>
#include <string.h>

#include "core/bcache.h"
#include "core/errno.h"
#include "core/xfs/xfs.h"
#include "core/xfs/xfs_format.h"
#include "lib/crc32c.h"
#include "fuzz.h"

#define BS 4096u
#define DEVICE_BLOCKS (512u * 256u) /* 512 MiB in 4 KiB blocks */
#define WALK_MAX 64u                 /* entries looked at per directory */
#define SECTOR 512u
#define SBLOCK_CRC 52u /* a v5 short-form b-tree block's CRC */
#define DIR3_CRC 4u    /* a v5 directory block's CRC */

struct disk {
    uint8_t *bytes;
    size_t len;
};

/* A directory walk: the filesystem and how many entries it has seen. */
struct walk {
    struct xfs *fs;
    uint32_t seen;
};

static struct disk disk;
static uint8_t cache_mem[BCACHE_MAX_SLOTS * BS];

/* Copies what the input holds of [lba, lba+count) and zero-fills the rest. */
static int disk_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    struct disk *d = self;
    uint64_t at = lba * BS, want = (uint64_t)count * BS;
    memset(buf, 0, want);
    if (at < d->len) {
        memcpy(buf, d->bytes + at, d->len - at < want ? d->len - at : want);
    }
    return KERR_OK;
}

/* Keeps what lands inside the input's copy; the rest is dropped. */
static int disk_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    struct disk *d = self;
    uint64_t at = lba * BS, want = (uint64_t)count * BS;
    if (at < d->len) {
        memcpy(d->bytes + at, buf, d->len - at < want ? d->len - at : want);
    }
    return KERR_OK;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

/* Writes the CRC32C of [at, at+len) into it at `off`, as XFS does: the
 * field zeroed first, the sum stored little-endian. */
static void seal(uint8_t *at, uint32_t len, uint32_t off) {
    memset(at + off, 0, 4);
    uint32_t c = crc32c(at, len);
    memcpy(at + off, &c, 4);
}

/* Where a block's CRC lives, by its magic; 0 when it is not one this seals. */
static uint32_t block_crc_offset(uint32_t magic) {
    switch (magic) {
    case 0x41423342u: /* AB3B */
    case 0x41423343u: /* AB3C */
    case 0x49414233u: /* IAB3 */
    case 0x46494233u: /* FIB3 */
    case 0x524d4233u: /* RMB3 */
    case 0x52334643u: /* R3FC */
        return SBLOCK_CRC;
    case 0x58444233u: /* XDB3 */
    case 0x58444433u: /* XDD3 */
        return DIR3_CRC;
    default:
        return 0;
    }
}

/* Reseals every structure recognisable in `d` (see the file comment). The
 * sizes are the superblock's own, bounded so that a hostile one cannot send
 * the scan outside the buffer; the kernel is still the judge of them. */
static void reseal_all(struct disk *d) {
    if (d->len < 4u * SECTOR || (be16(d->bytes + XFS_SB_VERSIONNUM) & 0xfu) != 5u) {
        return;
    }
    uint32_t bsize = be32(d->bytes + XFS_SB_BLOCKSIZE), isize = be16(d->bytes + XFS_SB_INODESIZE);
    bsize = bsize >= SECTOR && bsize <= BS ? bsize : BS;
    isize = isize >= 256u && isize <= 2048u ? isize : 512u;
    for (size_t at = 0; at + bsize <= d->len; at += bsize) {
        uint32_t off = block_crc_offset(be32(d->bytes + at));
        if (off != 0) {
            seal(d->bytes + at, bsize, off);
        }
    }
    for (size_t at = 0; at + isize <= d->len; at += 256u) {
        if (be16(d->bytes + at) == XFS_DINODE_MAGIC && d->bytes[at + 4] == 3u) {
            seal(d->bytes + at, isize, XFS_DI_CRC);
        }
    }
    seal(d->bytes + SECTOR, SECTOR, XFS_AGF_CRC);
    seal(d->bytes + 2u * SECTOR, SECTOR, XFS_AGI_CRC);
    seal(d->bytes + 3u * SECTOR, SECTOR, XFS_AGFL_CRC);
    seal(d->bytes, SECTOR, XFS_SB_CRC);
}

static int disk_flush(void *self) {
    (void)self;
    return KERR_OK;
}

static int count_attr(void *self, const uint8_t *name, uint32_t namelen, uint8_t flags, uint32_t valuelen) {
    (void)name, (void)namelen, (void)flags, (void)valuelen;
    (*(uint32_t *)self)++;
    return KERR_OK;
}

/* Reads one entry the way `ls -l` and `cat` would. */
static int visit(void *self, const struct xfs_dirent *e) {
    struct walk *w = self;
    struct xfs *fs = w->fs;
    struct xfs_inode ip;
    uint8_t head[512];
    uint32_t attrs = 0, got = 0;
    if (xfs_inode_read(fs, e->ino, &ip) == KERR_OK) {
        (void)xfs_attr_list(fs, &ip, count_attr, &attrs);
        (void)xfs_file_read(fs, &ip, 0, head, sizeof head, &got);
    }
    return ++w->seen >= WALK_MAX ? KERR_NOSPC : KERR_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static struct blockdev dev;
    static struct bcache cache;
    static struct xfs fs;
    struct xfs_inode root;
    struct walk w = {&fs, 0};
    disk = (struct disk){.bytes = malloc(size ? size : 1u), .len = size};
    if (disk.bytes == NULL) {
        return 0;
    }
    memcpy(disk.bytes, data, size);
    reseal_all(&disk);
    dev = (struct blockdev){.read = disk_read, .write = disk_write, .flush = disk_flush,
                            .self = &disk, .block_size = BS, .blocks = DEVICE_BLOCKS};
    memset(&fs, 0, sizeof fs);
    if (bcache_init(&cache, &dev, cache_mem, BCACHE_MAX_SLOTS) == KERR_OK && xfs_mount(&fs, &cache) == KERR_OK &&
        xfs_path_lookup(&fs, "/", &root) == KERR_OK) {
        (void)xfs_dir_list(&fs, &root, visit, &w);
    }
    free(disk.bytes);
    return 0;
}
