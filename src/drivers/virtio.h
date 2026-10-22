/* virtio — the transport every emulated device here speaks.
 *
 * It earns its place for one reason: **it lets the storage and network stacks
 * be proved before a single line of board driver exists.** The buffer cache,
 * XFS, the IPv4 stack and TCP all carried over from the ARM kernel with host
 * tests; what they have never had on this architecture is a real device
 * under them. virtio provides one, in an emulator that models it faithfully,
 * so the SD and NVMe drivers that follow are debugging *their own
 * registers* rather than the filesystem above them.
 *
 * This is the modern MMIO transport (version 2), which the device tree
 * advertises as `virtio,mmio`. Legacy version 1 is deliberately **refused**
 * rather than supported: its queue layout differs, QEMU offers version 2 by
 * default, and a driver that half-supports a layout it cannot test is worse
 * than one that says no.
 *
 * Queues are "split" virtqueues: a descriptor table, an available ring the
 * driver writes, and a used ring the device writes. The subtlety that is
 * worth knowing before reading the code is that **the device is not obliged
 * to use descriptors in order** — the used ring carries the head index of
 * each completed chain, which is why a request's identity has to travel with
 * it rather than being inferred from position.
 */
#ifndef MINIKERN_DRIVERS_VIRTIO_H
#define MINIKERN_DRIVERS_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* Device types, as the transport reports them. */
#define VIRTIO_ID_NET 1u
#define VIRTIO_ID_BLOCK 2u

/* How many descriptors a queue has. A power of two, and small on purpose:
 * one request in flight at a time is all this kernel needs, and a deeper
 * queue is memory spent on parallelism nothing asks for yet. */
#define VIRTIO_QUEUE_SIZE 8u

/* Descriptor flags. */
#define VIRTQ_DESC_F_NEXT 1u
#define VIRTQ_DESC_F_WRITE 2u

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_QUEUE_SIZE];
    uint16_t used_event;
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_QUEUE_SIZE];
    uint16_t avail_event;
};

/* One queue's rings, laid out as the specification requires: the descriptor
 * table and the available ring on one page, the used ring on another, each
 * aligned. Allocated as whole pages so the alignment is a property of the
 * allocation rather than of arithmetic nobody checks. */
struct virtq {
    struct virtq_desc *desc;
    struct virtq_avail *avail;
    struct virtq_used *used;
    uint16_t last_used;   /* how much of the used ring has been consumed */
    uint16_t free_head;   /* the next descriptor to hand out */
};

struct virtio_dev {
    volatile uint32_t *regs;
    phys_t base;
    uint32_t irq;
    uint32_t device_id;
    struct virtq q;
    char path[64];
};

/* Finds the n-th `virtio,mmio` node whose device id matches, and brings the
 * transport up to the point where a queue can be used: features negotiated,
 * the queue's rings allocated and published, status set to DRIVER_OK.
 *
 * KERR_NOENT when the tree has no such device — which is the ordinary case
 * on a machine without one, not an error. KERR_NOTSUP for a legacy version-1
 * device. */
int virtio_find(const struct fdt *fdt, uint32_t device_id, struct virtio_dev *out);

/* The device's own configuration space, which every device type reads
 * differently (a disk's capacity, a NIC's MAC address). */
uint32_t virtio_config_u32(const struct virtio_dev *d, uint32_t offset);
uint64_t virtio_config_u64(const struct virtio_dev *d, uint32_t offset);

/* Submits a chain of up to three buffers and waits for the device to finish
 * it. Synchronous on purpose: the first user is a block device whose caller
 * cannot proceed without the data, and an asynchronous path with nothing to
 * overlap is complexity bought on credit.
 *
 * `writable` says which buffers the **device** writes. Returns KERR_OK, or
 * KERR_TIMEDOUT when the device never completes — which is a real outcome,
 * not an impossible one, and a driver that waits for ever instead cannot say
 * what went wrong. */
int virtio_request(struct virtio_dev *d, const void *const *bufs, const uint32_t *lens,
                   const bool *writable, uint32_t count);

/* Acknowledges a device interrupt. Returns true when this device raised it. */
bool virtio_ack_interrupt(struct virtio_dev *d);

#endif
