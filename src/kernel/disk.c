#include "kernel/disk.h"

#include <stddef.h>

#include "core/blockscale.h"
#include "core/partition.h"
#include "core/xfs/xfs.h"
#include "core/console.h"
#include "arch/riscv/mmu.h"
#include "core/errno.h"
#include "drivers/nvme.h"
#include "core/volinfo.h"
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
static uint32_t parts_found;
static struct blockdev whole;
static struct blockscale scale;
static struct blockdev device;
static struct bcache cache;
static bool present;

/* **Every candidate the disk offers**, not just the one the kernel boots
 * from. `adopt_the_root` picks the one labelled `ROOT`; automount has to try
 * the rest, because another partition with a label is a volume the machine
 * should mount somewhere.
 *
 * The wrapping structures live here rather than in the caller because each
 * presented device *points at* its partition wrapper and its scaler — a
 * device built on a caller's stack is a device whose innards are freed
 * while the mount is still reading through them. */
/* The root's disk comes first (its volumes keep the indices the root
 * decision made), then every other disk's: up to three disks' tables. */
#define VOLUME_SLOTS (PARTITION_MAX * 3u)

static struct partdev vol_part[VOLUME_SLOTS];
static struct blockscale vol_scale[VOLUME_SLOTS];
static struct blockdev vol_over[VOLUME_SLOTS];
static struct blockdev vol_dev[VOLUME_SLOTS];
static struct volinfo vol_info[VOLUME_SLOTS];
static uint32_t vol_count;
/* How many of them are on the root's disk: those are mounted at boot. */
static uint32_t root_disk_count;

/* Which adapter is in use, so the boot log names the device rather than
 * merely its size. */
static const char *attached_kind = "";

const char *disk_kind(void) {
    return attached_kind;
}

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

/* One kind of device the kernel can attach, wrapped so every kind has the
 * same shape. The order of `ways` is the order they are tried: memory, then
 * **NVMe**, because on a board with an M.2 drive that is the storage worth
 * having, then SD, then virtio, which is the emulator's. */
struct attach_way {
    const char *kind;
    int (*init)(const struct fdt *fdt, struct blockdev *out);
};

/* **Memory first**, because asking for it is unambiguous: a filesystem image
 * in DRAM is there because a test put it there. */
static int attach_ram(const struct fdt *fdt, struct blockdev *out) {
    (void)fdt;
    return ram_bytes != 0 ? ramdisk_init(ram_at, ram_bytes, out) : KERR_NOENT;
}

/* NVMe sits behind PCIe, so the bus comes up first. */
static int attach_nvme(const struct fdt *fdt, struct blockdev *out) {
    return pci_init(fdt) == KERR_OK ? nvme_init(out) : KERR_NOENT;
}

/* SDHCI, which is what an emulator models; on the board it finds nothing. */
static int attach_sdhci(const struct fdt *fdt, struct blockdev *out) {
    return sdhci_init(fdt, out);
}

/* The **DesignWare** controller, which is what a real VisionFive 2 has
 * where the documentation promised a Cadence SDHCI. */
static int attach_dwmmc(const struct fdt *fdt, struct blockdev *out) {
    return dwmmc_init(fdt, out);
}

static int attach_virtio(const struct fdt *fdt, struct blockdev *out) {
    return virtio_blk_init(fdt, out);
}

static const struct attach_way ways[] = {
    {"ram", attach_ram},     {"nvme", attach_nvme},     {"sd", attach_sdhci},
    {"sd (designware)", attach_dwmmc}, {"virtio", attach_virtio},
};
#define WAYS (sizeof ways / sizeof ways[0])

/* The devices that attached, in the order they did, and what each is. */
static struct blockdev candidates[WAYS];
static const char *candidate_kind[WAYS];
static uint32_t candidate_count;

/* Attaches the next device `ways` can offer after position `*from`, into
 * the next candidate slot. False when none is left. */
/* Which `ways` have a device attached: a controller is brought up once. */
static bool way_attached[WAYS];

static bool attach_next_of(const struct fdt *fdt, uint32_t *from, bool any_kind) {
    for (; *from < WAYS; (*from)++) {
        const struct attach_way *w = &ways[*from];
        if (way_attached[*from] || (!any_kind && !allowed(w->kind)) ||
            w->init(fdt, &candidates[candidate_count]) != KERR_OK) {
            continue;
        }
        way_attached[*from] = true;
        candidate_kind[candidate_count] = w->init == attach_sdhci ? sdhci_kind() : w->kind;
        candidate_count++;
        (*from)++;
        return true;
    }
    return false;
}

/* The next device of the kind the operator asked for (`minikern.disk=`). */
static bool attach_next(const struct fdt *fdt, uint32_t *from) {
    return attach_next_of(fdt, from, false);
}

/* Presents each partition (or the whole disk) as a block device of
 * filesystem-sized blocks, ready to mount.
 *
 * A partition whose sector size cannot present a filesystem block is
 * skipped rather than refused: one unusable partition on a disk must not
 * stop the others being mounted, which is the difference between automount
 * and the single-device path above. */
/* Appends one disk's volumes after those already presented, each named
 * as Linux would name it (`base` plus its partition number). */
static void present_disk(struct blockdev *dev, const struct partition *table, uint32_t found,
                         const char *base) {
    for (uint32_t i = 0; i < found && i < PARTITION_MAX && vol_count < VOLUME_SLOTS; i++) {
        if (partition_wrap(dev, &table[i], &vol_part[vol_count], &vol_over[vol_count]) !=
            KERR_OK) {
            continue;
        }
        if (blockscale_wrap(&vol_over[vol_count], FILESYSTEM_BLOCK, &vol_scale[vol_count],
                            &vol_dev[vol_count]) != KERR_OK) {
            continue;
        }
        volinfo_name(base, i + 1u, vol_info[vol_count].name);
        vol_count++;
    }
    if (found == 0 && vol_count < VOLUME_SLOTS &&
        blockscale_wrap(dev, FILESYSTEM_BLOCK, &vol_scale[vol_count], &vol_dev[vol_count]) ==
            KERR_OK) {
        volinfo_name(base, 0, vol_info[vol_count].name);
        vol_count++; /* a whole-disk filesystem, which is what a card is */
    }
}

static const char *scanning_kind = "";

static void present_volumes(uint32_t found) {
    vol_count = 0;
    present_disk(&raw, parts, found, volinfo_base(scanning_kind));
}

uint32_t disk_volume_count(void) {
    return vol_count;
}

const struct blockdev *disk_volume(uint32_t i) {
    return i < vol_count ? &vol_dev[i] : NULL;
}

/* Which presented volume is the root filesystem. */
static uint32_t root_volume;

uint32_t disk_root_volume(void) {
    return root_volume;
}

/* Whether presented volume `i` is an XFS labelled `ROOT`. Only block 0 is
 * read — into a page, because a controller's DMA rules apply to it — and
 * nothing is mounted. */
/* How often a read the root decision rests on is tried. The SD controller's
 * first reads after bring-up fail now and then (ROADMAP §4); one failure
 * must not decide which filesystem the machine runs on. */
#define DECIDING_READS 3u

/* Whether volume `i` holds an XFS superblock, and its label into `label`. */
static bool volume_xfs_label(uint32_t i, uint8_t *page, char *label) {
    const struct blockdev *v = &vol_dev[i];
    int rc = KERR_IO;
    for (uint32_t a = 0; rc != KERR_OK && a < DECIDING_READS && v->block_size <= 4096u; a++) {
        rc = v->read(v->self, 0, 1, page);
    }
    return rc == KERR_OK && xfs_superblock_label(page, v->block_size, label);
}

static bool volume_is_root(uint32_t i, uint8_t *page) {
    char label[XFS_LABEL_BYTES];
    return volume_xfs_label(i, page, label) && label[0] == 'R' && label[1] == 'O' &&
           label[2] == 'O' && label[3] == 'T' && label[4] == '\0';
}

bool disk_has_root(void) {
    return root_volume < vol_count;
}

/* Makes candidate `c` the disk, presents its volumes, and says which of
 * them is labelled `ROOT`, or `vol_count` for none. */
/* Reads candidate `c`'s partition table into `parts`, again when a read
 * failed: an unreadable table is not an absent one, and taking it for one
 * presented a partitioned card as a single volume (its protective MBR). */
static int scan_candidate(uint32_t c) {
    int rc = KERR_IO;
    raw = candidates[c];
    scanning_kind = candidate_kind[c];
    for (uint32_t a = 0; rc == KERR_IO && a < DECIDING_READS; a++) {
        rc = partition_scan_checked(&raw, parts, PARTITION_MAX, &parts_found);
    }
    if (rc != KERR_OK) {
        kprintf("disk: %s's partition table could not be read\n", candidate_kind[c]);
    }
    return rc;
}

static uint32_t look_for_root(uint32_t c, uint8_t *page) {
    uint32_t i = 0;
    vol_count = 0;
    if (scan_candidate(c) != KERR_OK) {
        return 0; /* no volumes, so not a root either */
    }
    present_volumes(parts_found);
    while (i < vol_count && !volume_is_root(i, page)) {
        i++;
    }
    return i;
}

/* With no `ROOT` label anywhere: the first volume, **if it is XFS** — an
 * unlabelled card or a test image. A card as bought is one FAT partition
 * holding the kernel; taking that as the root left it out of the automount
 * (no `/boot`) and pointed the XFS reader at it. Then there is no root on
 * the disk, and `/` is the command line's (`minikern.netroot`). */
static int take_first_if_xfs(uint8_t *page) {
    char label[XFS_LABEL_BYTES];
    if (volume_xfs_label(0, page, label)) {
        root_volume = 0;
        kprintf("disk: no volume is labelled ROOT; using the first on %s\n", attached_kind);
        return KERR_OK;
    }
    root_volume = vol_count;
    kprintf("disk: no XFS root on %s (nothing labelled ROOT, and the first volume is not XFS); "
            "/ is the command line's\n", attached_kind);
    return KERR_OK;
}

/* **The root is the XFS labelled `ROOT`, wherever it is.** Devices are
 * attached in `ways` order and each one's volumes are looked at in turn, so
 * the root can be partition 2 behind a FAT boot partition, or on the NVMe
 * drive while the kernel came off the card. With no `ROOT` anywhere, the
 * first device's first volume is used — the old rule, and what an unlabelled
 * card or a test image still relies on. */
/* The next `ways` entry to try, kept so the disks after the root's can
 * still be attached once the root is found; and which candidate it is on. */
static uint32_t next_way;
static uint32_t root_candidate;

static int choose_root(const struct fdt *fdt, uint8_t *page) {
    uint32_t from = 0;
    while (attach_next(fdt, &from)) {
        uint32_t c = candidate_count - 1u;
        root_volume = look_for_root(c, page);
        if (root_volume < vol_count) {
            next_way = from;
            root_candidate = c;
            attached_kind = candidate_kind[c];
            kprintf("disk: the root is volume %u on %s, labelled ROOT\n", root_volume,
                    attached_kind);
            return KERR_OK;
        }
    }
    if (candidate_count == 0) {
        return KERR_NOENT;
    }
    /* **The second look decides too.** It used to be made only to present
     * the first device's volumes again, and what it found was thrown away —
     * so a first look spoiled by a failed read sent the machine to the FAT
     * partition while this one was reading `ROOT` right beside it. */
    next_way = from;
    root_candidate = 0;
    root_volume = look_for_root(0, page);
    attached_kind = candidate_kind[0];
    if (vol_count == 0) {
        return KERR_IO;
    }
    if (root_volume < vol_count) {
        kprintf("disk: the root is volume %u on %s, labelled ROOT\n", root_volume, attached_kind);
        return KERR_OK;
    }
    return take_first_if_xfs(page);
}

/* Attaches the disk the root lives on, and points `whole` at the root's
 * partition — or at the whole device when it has no partition table. */
static int adopt_the_root(const struct fdt *fdt) {
    phys_t page = 0;
    int rc;
    if (mem_alloc_pages(1, 1, &page) != KERR_OK) {
        return KERR_NOMEM;
    }
    rc = choose_root(fdt, (uint8_t *)mmu_phys_to_virt(page));
    (void)mem_free_pages(page, 1);
    if (dwmmc_read_retries() != 0) {
        kprintf("disk: %llu SD read(s) needed a second attempt\n",
                (unsigned long long)dwmmc_read_retries());
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (parts_found == 0) {
        whole = raw;
        return KERR_OK;
    }
    whole = vol_over[root_volume < vol_count ? root_volume : 0];
    return KERR_OK;
}

/* Each other disk's partition table: kept, because its presented volumes
 * point into it for as long as the kernel runs. */
static struct partition other_parts[WAYS][PARTITION_MAX];

/* **Every other disk the machine has**, after the root's: attached, its
 * table read and its volumes presented — but not mounted. Automount takes
 * the root's disk only; a second drive is listed (`/proc/volumes`) and
 * mounted when somebody asks (`mount(2)`), because a drive put in a slot is
 * not consent to have it written to. */
static void present_other_disks(const struct fdt *fdt) {
    uint32_t from = 0;
    root_disk_count = vol_count;
    /* **Every kind, whatever `minikern.disk=` asked for.** The preference
     * chooses where the root comes from; it used to be the only disk
     * attached, so a card-booted board (`minikern.disk=sd`) could not list
     * its NVMe drive at all. Nothing here is mounted, so attaching it
     * writes nothing; a kind already attached is skipped. */
    while (attach_next_of(fdt, &from, true)) {
    }
    for (uint32_t c = 0; c < candidate_count; c++) {
        uint32_t found = 0;
        int rc = KERR_IO;
        for (uint32_t a = 0; c != root_candidate && rc == KERR_IO && a < DECIDING_READS; a++) {
            rc = partition_scan_checked(&candidates[c], other_parts[c], PARTITION_MAX, &found);
        }
        if (rc == KERR_OK) {
            present_disk(&candidates[c], other_parts[c], found, volinfo_base(candidate_kind[c]));
        }
    }
}

/* What each presented volume is, from its first block, read once now: the
 * listing and a mount by name answer from this rather than reading a disk
 * that may be mounted and busy. */
static void probe_volumes(void) {
    phys_t page = 0;
    uint8_t *p;
    if (mem_alloc_pages(1, 1, &page) != KERR_OK) {
        return;
    }
    p = (uint8_t *)mmu_phys_to_virt(page);
    for (uint32_t i = 0; i < vol_count; i++) {
        const struct blockdev *v = &vol_dev[i];
        int rc = KERR_IO;
        for (uint32_t a = 0; rc != KERR_OK && a < DECIDING_READS && v->block_size <= 4096u; a++) {
            rc = v->read(v->self, 0, 1, p);
        }
        volinfo_probe(rc == KERR_OK ? p : NULL, v->block_size, &vol_info[i]);
        vol_info[i].bytes = v->blocks * v->block_size;
        vol_info[i].root_disk = i < root_disk_count;
    }
    (void)mem_free_pages(page, 1);
}

uint32_t disk_root_disk_count(void) {
    return root_disk_count;
}

const struct volinfo *disk_volume_info(uint32_t i) {
    return i < vol_count ? &vol_info[i] : NULL;
}

uint32_t disk_volume_named(const char *name) {
    uint32_t i = 0;
    while (i < vol_count && !volinfo_same(vol_info[i].name, name)) {
        i++;
    }
    return i;
}

/* Whole pages for the cache to write runs of blocks through (bcache.h): the
 * SD card programs a multi-block write once, so consecutive dirty blocks go
 * together. 16 blocks of 4 KiB; without the memory the cache writes one at
 * a time, as it always did. */
#define GATHER_BLOCKS 16u

void give_the_cache_a_gather_buffer(struct bcache *c, uint32_t block_size) {
    uint32_t npages = (GATHER_BLOCKS * block_size + 4095u) / 4096u;
    phys_t pages = 0;
    if (mem_alloc_pages(npages, 1, &pages) == KERR_OK) {
        bcache_set_gather(c, mmu_phys_to_virt(pages), GATHER_BLOCKS);
    }
}

int disk_attach(const struct fdt *fdt) {
    void *mem;
    int rc;
    /* **The filesystem may not start at sector 0.** A card formatted whole
     * puts its superblock there and every disk this project had read was
     * made that way; a drive partitioned the ordinary way puts the first
     * partition at sector 2048, and a reader pointed at sector 0 finds a
     * partition table where it expects a superblock.
     *
     * No table is not an error — it is a whole-disk filesystem, which is
     * what the SD cards here are — so the raw device is used directly in
     * that case and the layers above never learn the difference. */
    rc = adopt_the_root(fdt);
    if (rc != KERR_OK) {
        return rc;
    }
    present_other_disks(fdt);
    probe_volumes();
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
    mem = mmu_phys_to_virt(pages); /* never the identity alias (step C) */
    for (uint32_t i = 0; i < npages * 4096u; i++) {
        ((volatile uint8_t *)mem)[i] = 0;
    }
    rc = bcache_init(&cache, &device, mem, CACHE_SLOTS);
    if (rc != KERR_OK) {
        (void)mem_free_pages(pages, npages);
        return rc;
    }
    give_the_cache_a_gather_buffer(&cache, device.block_size);
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
