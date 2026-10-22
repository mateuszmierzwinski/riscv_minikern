/* virtio_blk — a disk, as a `struct blockdev`.
 *
 * The point of this driver is what sits **above** it: the buffer cache and
 * the XFS implementation came across from the ARM kernel with their host
 * tests, and have never had a real device underneath them on this
 * architecture. With this, they do — in an emulator that models the
 * transport faithfully. So when the SD and NVMe drivers arrive, a failure is
 * in the new driver's registers rather than somewhere in a filesystem that
 * has never been exercised here.
 *
 * It presents the same `blockdev` port every other storage adapter will, so
 * nothing above it knows which one it is talking to.
 */
#ifndef MINIKERN_DRIVERS_VIRTIO_BLK_H
#define MINIKERN_DRIVERS_VIRTIO_BLK_H

#include "core/blockdev.h"
#include "core/fdt.h"

/* Finds a virtio block device and fills `out`. The device's capacity comes
 * from its configuration space, in 512-byte sectors, which is what the
 * specification defines regardless of what the backing file's block size is.
 *
 * KERR_NOENT when the machine has no such device, which is the ordinary case
 * and not a failure. */
int virtio_blk_init(const struct fdt *fdt, struct blockdev *out);

/* Where it was found, and how big it is, for the boot log. */
const char *virtio_blk_path(void);
uint64_t virtio_blk_sectors(void);

#endif
