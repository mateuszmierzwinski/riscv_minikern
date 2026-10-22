#include "kernel/disk.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "drivers/virtio_blk.h"
#include "kernel/mem.h"

/* How much of the disk the cache holds. 64 slots of 4 KiB is 256 KiB, which
 * is enough for a filesystem's metadata to stay resident and small enough
 * that a board with 512 MiB does not notice. */
#define CACHE_SLOTS 64u
#define CACHE_BLOCK 4096u

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
static int attach_any(const struct fdt *fdt) {
    if (pci_init(fdt) == KERR_OK && nvme_init(&device) == KERR_OK) {
        attached_kind = "nvme";
        return KERR_OK;
    }
    if (virtio_blk_init(fdt, &device) == KERR_OK) {
        attached_kind = "virtio";
        return KERR_OK;
    }
    return KERR_NOENT;
}

int disk_attach(const struct fdt *fdt) {
    void *mem;
    int rc = attach_any(fdt);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The cache's storage comes from the frame allocator rather than .bss:
     * 256 KiB of a kernel image is 256 KiB that every board pays for whether
     * or not it has a disk. */
    mem = kzalloc(CACHE_SLOTS * (CACHE_BLOCK + 64u));
    if (mem == NULL) {
        return KERR_NOMEM;
    }
    rc = bcache_init(&cache, &device, mem, CACHE_SLOTS);
    if (rc != KERR_OK) {
        kfree(mem);
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
