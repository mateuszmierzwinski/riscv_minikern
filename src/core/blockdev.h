/* blockdev — fixed-size blocks addressed by index, the one thing a
 * filesystem needs from storage.
 *
 * It is a port (ports.h's shape: function pointers plus self), so what is
 * behind it is the composition root's business: a network block device over
 * our own TCP (kernel/nbd.h), a file on the build machine (the tests), or
 * one day an SD controller. The filesystem and the buffer cache above it
 * never learn which.
 *
 * read and write move count whole blocks starting at lba and return
 * KERR_OK or a negative KERR_*; a short transfer is a failure, not a
 * partial success, because a filesystem has no use for half a block.
 * flush makes everything written durable. Operations are synchronous: the
 * caller is a kernel thread that may sleep, or a host test. */
#ifndef MINIKERN_CORE_BLOCKDEV_H
#define MINIKERN_CORE_BLOCKDEV_H

#include <stdbool.h>
#include <stdint.h>

struct blockdev {
    int (*read)(void *self, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *self, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(void *self);
    void *self;
    uint32_t block_size;  /* a power of two, at least 512 */
    uint64_t blocks;      /* how many of them; the device ends there */
    bool read_only;       /* write and flush must refuse */
};

/* True when [lba, lba+count) lies inside the device. Every implementation
 * checks this, so a filesystem bug cannot become a write past the end. */
static inline bool blockdev_in_range(const struct blockdev *d, uint64_t lba, uint32_t count) {
    return count > 0 && lba <= d->blocks && count <= d->blocks - lba;
}

#endif
