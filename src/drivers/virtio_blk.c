#include "drivers/virtio_blk.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"
#include "drivers/virtio.h"

/* The request header and the status byte that bracket every transfer. */
#define VIRTIO_BLK_T_IN 0u   /* read: the device writes */
#define VIRTIO_BLK_T_OUT 1u  /* write: the device reads */
#define VIRTIO_BLK_T_FLUSH 4u

#define VIRTIO_BLK_S_OK 0u
#define VIRTIO_BLK_S_IOERR 1u
#define VIRTIO_BLK_S_UNSUPP 2u

/* The capacity, in 512-byte sectors, at the start of configuration space. */
#define CONFIG_CAPACITY 0u

#define SECTOR_BYTES 512u

struct blk_request {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

static struct {
    struct virtio_dev dev;
    uint64_t sectors;
    bool up;
} blk;

/* One request at a time, so the header and the status byte can live here
 * rather than on a caller's stack. They must be addressable by the device,
 * which an identity-mapped .bss is and a stack also is — but a stack buffer
 * that outlives its frame is the kind of bug that corrupts a disk, and
 * keeping them here makes it impossible. */
static struct blk_request request;
static volatile uint8_t status;

/* Submits one transfer: header, payload, status. The payload's direction is
 * the only thing that changes between a read and a write — the device writes
 * it for a read, reads it for a write. */
static int transfer(uint32_t type, uint64_t sector, void *buf, uint32_t bytes, bool device_writes) {
    /* Header, payload, status — and for a request with no payload (a
     * flush), header and status, with nothing in between. Passing a null
     * payload descriptor instead would hand the device an address it must
     * not read. */
    const void *bufs[3] = {&request, bytes == 0 ? (const void *)&status : buf,
                           (const void *)&status};
    uint32_t lens[3] = {sizeof request, bytes == 0 ? 1u : bytes, 1u};
    bool writable[3] = {false, bytes == 0 ? true : device_writes, true};
    int rc;
    request.type = type;
    request.reserved = 0;
    request.sector = sector;
    status = 0xff; /* so an untouched status cannot read as success */
    rc = virtio_request(&blk.dev, bufs, lens, writable, bytes == 0 ? 2u : 3u);
    if (rc != KERR_OK) {
        return rc;
    }
    switch (status) {
    case VIRTIO_BLK_S_OK: return KERR_OK;
    case VIRTIO_BLK_S_UNSUPP: return KERR_NOTSUP;
    default: return KERR_IO;
    }
}

/* The block size this port advertises is 512: the specification's sector,
 * and what the capacity is counted in. A larger logical block would be a
 * translation this driver has no reason to perform. */
static int blk_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!blk.up || count == 0 || lba + count > blk.sectors) {
        return KERR_INVAL;
    }
    return transfer(VIRTIO_BLK_T_IN, lba, buf, count * SECTOR_BYTES, true);
}

static int blk_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!blk.up || count == 0 || lba + count > blk.sectors) {
        return KERR_INVAL;
    }
    /* The cast drops const because the descriptor takes one address for
     * either direction; `writable` is what tells the device it may not
     * write here. */
    return transfer(VIRTIO_BLK_T_OUT, lba, (void *)(uintptr_t)buf, count * SECTOR_BYTES, false);
}

/* A flush the device is free to refuse, because a device without the flush
 * feature has nothing to flush — its writes are already durable as far as
 * anything above can tell. Refusal is therefore success, not an error. */
static int blk_flush(void *self) {
    int rc;
    (void)self;
    if (!blk.up) {
        return KERR_NODEV;
    }
    rc = transfer(VIRTIO_BLK_T_FLUSH, 0, NULL, 0, false);
    return rc == KERR_NOTSUP ? KERR_OK : rc;
}

int virtio_blk_init(const struct fdt *fdt, struct blockdev *out) {
    int rc = virtio_find(fdt, VIRTIO_ID_BLOCK, &blk.dev);
    if (rc != KERR_OK) {
        return rc;
    }
    blk.sectors = virtio_config_u64(&blk.dev, CONFIG_CAPACITY);
    blk.up = true;
    *out = (struct blockdev){
        .read = blk_read,
        .write = blk_write,
        .flush = blk_flush,
        .self = NULL,
        .block_size = SECTOR_BYTES,
        .blocks = blk.sectors,
        .read_only = false,
    };
    return KERR_OK;
}

const char *virtio_blk_path(void) {
    return blk.up ? blk.dev.path : "";
}

uint64_t virtio_blk_sectors(void) {
    return blk.sectors;
}
