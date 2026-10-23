#include "kernel/disk.h"

#include <stddef.h>

#include "core/blockscale.h"
#include "core/partition.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/nvme.h"
#include "drivers/ramdisk.h"
#include "drivers/pci.h"
#include "drivers/sdhci.h"
#include "drivers/dwmmc.h"
#include "drivers/virtio_blk.h"
#include "kernel/mem.h"

/* How much of the disk the cache holds. 64 slots of 4 KiB is 256 KiB, which
 * is enough for a filesystem's metadata to stay resident and small enough
 * that a board with 512 MiB does not notice. */
#define CACHE_SLOTS 64u

/* The device as the controller presents it (512-byte sectors, usually), and
 * the view the filesystem wants (4 KiB blocks). Something must bridge them:
 * XFS asks its cache in **filesystem blocks**, and without the bridge every
 * metadata read returns an eighth of one — the filesystem mounts, because a
 * superblock fits in the first sector, and then the first directory read
 * fails with an I/O error and nothing else visibly wrong. */
#define FILESYSTEM_BLOCK 4096u

static struct blockdev raw;
static struct partition parts[PARTITION_MAX];
static struct partdev part;
static struct blockdev whole;
static struct blockscale scale;
static struct blockdev device;
static struct bcache cache;
static bool present;

/* Which adapter is in use, so the boot log names the device rather than
 * merely its size. */
static const char *attached_kind = "";

const char *disk_kind(void) {
    return attached_kind;
}

/* Tries the devices in the order a user would want them: **NVMe first**,
 * because on a board with an M.2 drive that is the storage worth having, and
 * virtio second, which is the emulator's. A board with neither is the
 * ordinary case for a kernel that netboots. */
/* The kind the operator asked for, empty for "whatever is there". */
static char wanted[8];

void disk_prefer(const char *kind, uint32_t len) {
    uint32_t i = 0;
    for (; kind != NULL && i < len && i + 1u < sizeof wanted; i++) {
        wanted[i] = kind[i];
    }
    wanted[i] = '\0';
}

/* True when this kind may be attached: either nothing was asked for, or this
 * is it. Compared on the prefix so "sd" matches "sd (designware)". */
static bool allowed(const char *kind) {
    if (wanted[0] == '\0') {
        return true;
    }
    for (uint32_t i = 0; wanted[i] != '\0'; i++) {
        if (kind[i] != wanted[i]) {
            return false;
        }
    }
    return true;
}

/* A filesystem image the boot loader left in memory, if the operator named
 * one. Zero means there is none. */
static phys_t ram_at;
static uint64_t ram_bytes;

void disk_use_ram(phys_t at, uint64_t bytes) {
    ram_at = at;
    ram_bytes = bytes;
}

static int attach_any(const struct fdt *fdt) {
    bool pci;
    /* **Memory first**, because asking for it is unambiguous. A filesystem
     * image in DRAM is there because a test put it there, and a test that
     * silently read the card instead would be worse than one that failed:
     * it would pass against the wrong filesystem. */
    if (allowed("ram") && ram_bytes != 0 && ramdisk_init(ram_at, ram_bytes, &raw) == KERR_OK) {
        attached_kind = "ram";
        return KERR_OK;
    }
    pci = allowed("nvme") && pci_init(fdt) == KERR_OK;
    if (pci && nvme_init(&raw) == KERR_OK) {
        attached_kind = "nvme";
        return KERR_OK;
    }
    if (allowed("sd") && sdhci_init(fdt, &raw) == KERR_OK) {
        attached_kind = sdhci_kind();
        return KERR_OK;
    }
    /* The **DesignWare** mobile-storage controller, which is what a real
     * VisionFive 2 has where the documentation promised a Cadence SDHCI.
     * After SDHCI rather than before it only because an emulator models the
     * other one; on the board the SDHCI probe finds nothing and falls
     * through to here. */
    if (allowed("sd") && dwmmc_init(fdt, &raw) == KERR_OK) {
        attached_kind = "sd (designware)";
        return KERR_OK;
    }
    if (allowed("virtio") && virtio_blk_init(fdt, &raw) == KERR_OK) {
        attached_kind = "virtio";
        return KERR_OK;
    }
    return KERR_NOENT;
}

/* Picks what the filesystem is read from: the first partition if the disk has
 * a table, the whole disk if it does not.
 *
 * The first partition rather than a search for a filesystem, because
 * choosing by content would mean mounting each candidate to find out — and
 * the operator who partitioned the disk put the filesystem in partition 1.
 * A disk whose table is corrupt reports no partitions and is used whole,
 * which fails at the superblock rather than at a plausible offset. */
static int adopt_partition_or_whole(void) {
    uint32_t found = partition_scan(&raw, parts, PARTITION_MAX);
    if (found == 0) {
        whole = raw;
        return KERR_OK;
    }
    kprintf("disk: %u partition%s; using the first, %llu sectors at %llu\n", found,
            found == 1u ? "" : "s", (unsigned long long)parts[0].sectors,
            (unsigned long long)parts[0].first);
    return partition_wrap(&raw, &parts[0], &part, &whole);
}

int disk_attach(const struct fdt *fdt) {
    void *mem;
    int rc = attach_any(fdt);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **The filesystem may not start at sector 0.** A card formatted whole
     * puts its superblock there and every disk this project had read was
     * made that way; a drive partitioned the ordinary way puts the first
     * partition at sector 2048, and a reader pointed at sector 0 finds a
     * partition table where it expects a superblock.
     *
     * No table is not an error — it is a whole-disk filesystem, which is
     * what the SD cards here are — so the raw device is used directly in
     * that case and the layers above never learn the difference. */
    rc = adopt_partition_or_whole();
    if (rc != KERR_OK) {
        return rc;
    }
    /* The filesystem's block over the controller's sector. A device whose
     * sector size does not divide it is refused rather than rounded: a
     * ratio that is not whole cannot be a block device. */
    rc = blockscale_wrap(&whole, FILESYSTEM_BLOCK, &scale, &device);
    if (rc != KERR_OK) {
        kprintf("disk: %u-byte sectors cannot present %u-byte blocks\n", raw.block_size,
                FILESYSTEM_BLOCK);
        return rc;
    }
    /* **Whole pages, not heap bytes.** The cache's slots are handed
     * straight to a DMA engine, and a controller's data pointer has
     * alignment rules the heap knows nothing about: NVMe describes a
     * transfer as a page-offset plus a length, so a buffer that straddles a
     * page boundary needs a second pointer the driver does not supply. The
     * heap returns 8-byte-aligned memory, so slots landed across page
     * boundaries and the controller refused the read — the filesystem
     * mounted and then its root directory would not load, with an I/O error
     * and nothing else wrong.
     *
     * Page-aligned pages make every 512-byte slot page-contained by
     * construction, which is a property rather than a hope. */
    phys_t pages = 0;
    uint32_t bytes = CACHE_SLOTS * device.block_size;
    uint32_t npages = (bytes + 4095u) / 4096u;
    if (mem_alloc_pages(npages, 1, &pages) != KERR_OK) {
        return KERR_NOMEM;
    }
    mem = (void *)(uintptr_t)pages;
    for (uint32_t i = 0; i < npages * 4096u; i++) {
        ((volatile uint8_t *)mem)[i] = 0;
    }
    rc = bcache_init(&cache, &device, mem, CACHE_SLOTS);
    if (rc != KERR_OK) {
        (void)mem_free_pages(pages, npages);
        return rc;
    }
    present = true;
    return KERR_OK;
}

struct bcache *disk_cache(void) {
    return present ? &cache : NULL;
}

const struct blockdev *disk_device(void) {
    return present ? &device : NULL;
}

bool disk_present(void) {
    return present;
}

/* Reads the last sector, writes a pattern over it, reads it back through the
 * cache, and puts the original contents back. */
static int round_trip(uint64_t lba, uint8_t *original, uint8_t *scratch) {
    int rc = device.read(device.self, lba, 1, original);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < device.block_size; i++) {
        scratch[i] = (uint8_t)(i * 7u + 13u);
    }
    rc = device.write(device.self, lba, 1, scratch);
    rc = rc != KERR_OK ? rc : device.flush(device.self);
    if (rc != KERR_OK) {
        return rc;
    }
    /* Through a path that cannot have cached it: the cache was never asked
     * for this sector, and the write went straight to the device. */
    for (uint32_t i = 0; i < device.block_size; i++) {
        scratch[i] = 0;
    }
    rc = device.read(device.self, lba, 1, scratch);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint32_t i = 0; i < device.block_size; i++) {
        if (scratch[i] != (uint8_t)(i * 7u + 13u)) {
            kprintf("disk: byte %u read back as %02x, not %02x\n", i, scratch[i],
                    (uint8_t)(i * 7u + 13u));
            return KERR_IO;
        }
    }
    return KERR_OK;
}

int disk_selftest(void) {
    uint8_t *original, *scratch;
    uint64_t lba;
    int rc, restore;
    if (!present) {
        return KERR_NODEV;
    }
    original = kmalloc(device.block_size);
    scratch = kmalloc(device.block_size);
    if (original == NULL || scratch == NULL) {
        kfree(original);
        kfree(scratch);
        return KERR_NOMEM;
    }
    lba = device.blocks - 1u; /* the end, so a filesystem at the start survives */
    rc = round_trip(lba, original, scratch);
    /* Put it back whatever happened, and say if that failed: a test that
     * leaves the disk changed has broken whatever runs next. */
    restore = device.write(device.self, lba, 1, original);
    restore = restore != KERR_OK ? restore : device.flush(device.self);
    if (restore != KERR_OK) {
        kprintf("disk: could not restore sector %llu (rc %d)\n", (unsigned long long)lba, restore);
    }
    kfree(original);
    kfree(scratch);
    return rc != KERR_OK ? rc : restore;
}
