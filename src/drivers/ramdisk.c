#include "drivers/ramdisk.h"

#include "arch/riscv/mmu.h"

#include <stddef.h>

#include "core/errno.h"

#define SECTOR 512u

/* The one device. A second would need a second of these, and nothing wants
 * two: this exists so that a test filesystem can be mounted, and a test
 * mounts one. */
static struct {
    uint8_t *at;
    uint64_t sectors;
} disk;

static uint8_t *sector_at(uint64_t lba) {
    return disk.at + lba * SECTOR;
}

static int ramdisk_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    const struct blockdev *d = self;
    if (!blockdev_in_range(d, lba, count)) {
        return KERR_INVAL;
    }
    for (uint64_t i = 0; i < (uint64_t)count * SECTOR; i++) {
        ((uint8_t *)buf)[i] = sector_at(lba)[i];
    }
    return KERR_OK;
}

static int ramdisk_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    const struct blockdev *d = self;
    if (!blockdev_in_range(d, lba, count)) {
        return KERR_INVAL;
    }
    for (uint64_t i = 0; i < (uint64_t)count * SECTOR; i++) {
        sector_at(lba)[i] = ((const uint8_t *)buf)[i];
    }
    return KERR_OK;
}

/* Nothing to flush: the device *is* memory, so a write is already as durable
 * as this device gets. Saying so by succeeding is right — a filesystem that
 * flushes and is refused concludes the device is broken. */
static int ramdisk_flush(void *self) {
    (void)self;
    return KERR_OK;
}

int ramdisk_init(phys_t at, uint64_t bytes, struct blockdev *out) {
    if (at == 0 || bytes < SECTOR || (bytes % SECTOR) != 0) {
        return KERR_INVAL;
    }
    /* The address comes from the command line as a physical one — the
     * boot loader put the image there — so it is reached through the
     * direct map rather than used as a pointer. */
    disk.at = (uint8_t *)mmu_phys_to_virt(at);
    disk.sectors = bytes / SECTOR;
    *out = (struct blockdev){.read = ramdisk_read,
                             .write = ramdisk_write,
                             .flush = ramdisk_flush,
                             .self = out,
                             .block_size = SECTOR,
                             .blocks = disk.sectors,
                             .read_only = false};
    return KERR_OK;
}
