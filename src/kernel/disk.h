/* disk — the one block device the kernel has, and the cache over it.
 *
 * `core/bcache.h` is pure and host-tested; this is the live instance, over
 * whatever storage adapter the machine turned out to have — virtio today,
 * the SD host controller and NVMe next. Nothing above here knows which.
 */
#ifndef MINIKERN_KERNEL_DISK_H
#define MINIKERN_KERNEL_DISK_H

#include <stdbool.h>
#include <stdint.h>

#include "core/types.h"

#include "core/bcache.h"
#include "core/blockdev.h"
#include "core/fdt.h"

/* Attaches the first storage device the machine offers and builds a cache
 * over it. KERR_NOENT when there is none, which is not an error: a kernel
 * that netboots has no disk and should say so once rather than fail. */
/* Which kind of device to attach, when a board has more than one.
 *
 * `NULL` or an empty string keeps the default order — NVMe, then SD, then
 * virtio — which is what a machine with one disk wants. A name restricts it
 * to that kind, which a machine with two needs: this board has a drive in
 * its M.2 slot *and* a card in its slot, and "the storage worth having" is
 * not always the one the operator means to boot from. */
void disk_prefer(const char *kind, uint32_t len);

/* Offers a filesystem image already in memory as the device, for
 * `minikern.disk=ram`. Called from the composition root before `disk_attach`,
 * with a range the frame allocator has been kept off. */
void disk_use_ram(phys_t at, uint64_t bytes);

int disk_attach(const struct fdt *fdt);

/* The cache, for a filesystem to read through. NULL when there is no disk. */
/* Every filesystem candidate the disk offers — each partition, or the whole
 * disk when it has no table — as a device of filesystem-sized blocks.
 *
 * Separate from `disk_cache`, which is the one the kernel boots from.
 * Automount walks these to find out what is actually on the machine. */
uint32_t disk_volume_count(void);
const struct blockdev *disk_volume(uint32_t i);

struct bcache *disk_cache(void);

/* The device itself, for anything that wants whole sectors. */
const struct blockdev *disk_device(void);

bool disk_present(void);

/* Which adapter the device came from — "nvme", "virtio", or "" when there is
 * none. The boot log names it, because "a 64 MiB disk" is much less useful
 * than knowing which controller produced it. */
const char *disk_kind(void);

/* A self-test: writes a pattern to a sector near the end of the device,
 * reads it back through the cache, and restores what was there.
 *
 * It uses the **last** sector on purpose — a filesystem lives at the
 * beginning, and a test that scribbles on sector 0 destroys exactly the
 * thing the next test wants to read. It restores the original contents, and
 * reports whether the restore itself succeeded, because a test that leaves a
 * disk changed has broken the next one. */
int disk_selftest(void);

#endif
