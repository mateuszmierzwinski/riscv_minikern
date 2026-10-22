/* nvme — the M.2 slot: a solid-state drive, as a `struct blockdev`.
 *
 * This is the storage the VisionFive 2 is worth using, and the driver is
 * short because NVMe is a sane interface: a controller with a pair of
 * **queues in host memory**, a doorbell register per queue, and commands
 * that are 64-byte structures. There is no bus protocol to implement, no
 * command set to negotiate, and no vendor variation to accommodate — the
 * specification is the whole contract.
 *
 * The shape:
 *
 *   * an **admin queue** pair, created before the controller is enabled,
 *     which is how the driver asks what the drive is and creates the queues
 *     it will actually use;
 *   * one **I/O queue** pair for reads and writes;
 *   * a doorbell per queue, written to say "there is a new entry", and a
 *     phase bit in each completion that flips every time the ring wraps —
 *     which is how a driver knows an entry is new without the controller
 *     having to clear anything.
 *
 * The phase bit is the part worth reading carefully: completions are not
 * zeroed between passes, so "is this entry new?" is answered by comparing
 * its phase with the one the driver expects, and that expectation inverts
 * each time the ring wraps. A driver that forgets to invert it hangs on the
 * 33rd command — long after any quick test would have passed.
 */
#ifndef MINIKERN_DRIVERS_NVME_H
#define MINIKERN_DRIVERS_NVME_H

#include "core/blockdev.h"
#include "core/fdt.h"

/* The PCI class of an NVMe controller: mass storage, non-volatile memory,
 * NVM Express. */
#define NVME_PCI_CLASS 0x010802u

/* Finds an NVMe controller behind the PCIe bus, brings it up, identifies
 * namespace 1 and presents it as a block device.
 *
 * KERR_NOENT when there is no such controller — the ordinary case on a board
 * with an empty M.2 slot. KERR_TIMEDOUT when the controller never becomes
 * ready, which is a real outcome and is reported rather than waited on for
 * ever. */
int nvme_init(struct blockdev *out);

/* What it found, for the boot log: the model as the drive reports it, and
 * its geometry. */
const char *nvme_model(void);
uint64_t nvme_blocks(void);
uint32_t nvme_block_size(void);

#endif
