#include "drivers/virtio.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/mem.h"

/* The MMIO register map, version 2. */
#define VIRTIO_MAGIC 0x000u        /* "virt" */
#define VIRTIO_VERSION 0x004u
#define VIRTIO_DEVICE_ID 0x008u
#define VIRTIO_DEVICE_FEATURES 0x010u
#define VIRTIO_DEVICE_FEATURES_SEL 0x014u
#define VIRTIO_DRIVER_FEATURES 0x020u
#define VIRTIO_DRIVER_FEATURES_SEL 0x024u
#define VIRTIO_QUEUE_SEL 0x030u
#define VIRTIO_QUEUE_NUM_MAX 0x034u
#define VIRTIO_QUEUE_NUM 0x038u
#define VIRTIO_QUEUE_READY 0x044u
#define VIRTIO_QUEUE_NOTIFY 0x050u
#define VIRTIO_INTERRUPT_STATUS 0x060u
#define VIRTIO_INTERRUPT_ACK 0x064u
#define VIRTIO_STATUS 0x070u
#define VIRTIO_QUEUE_DESC_LOW 0x080u
#define VIRTIO_QUEUE_DESC_HIGH 0x084u
#define VIRTIO_QUEUE_AVAIL_LOW 0x090u
#define VIRTIO_QUEUE_AVAIL_HIGH 0x094u
#define VIRTIO_QUEUE_USED_LOW 0x0a0u
#define VIRTIO_QUEUE_USED_HIGH 0x0a4u
#define VIRTIO_CONFIG 0x100u

#define VIRTIO_MAGIC_VALUE 0x74726976u /* "virt", little-endian */
#define VIRTIO_VERSION_MODERN 2u

/* Status bits, written in this order: the specification's handshake. */
#define STATUS_ACKNOWLEDGE 1u
#define STATUS_DRIVER 2u
#define STATUS_DRIVER_OK 4u
#define STATUS_FEATURES_OK 8u
#define STATUS_FAILED 128u

/* The one feature this driver insists on **not** having: a packed virtqueue
 * would be a different layout entirely. Offering nothing back is the honest
 * way to say "split queues, and no extensions". */
#define VIRTIO_F_VERSION_1 (1ull << 32)

#define WAIT_SPINS 200000000u

static void write_reg(const struct virtio_dev *d, uint32_t off, uint32_t v) {
    d->regs[off / 4u] = v;
}

static uint32_t read_reg(const struct virtio_dev *d, uint32_t off) {
    return d->regs[off / 4u];
}

uint32_t virtio_config_u32(const struct virtio_dev *d, uint32_t offset) {
    return read_reg(d, VIRTIO_CONFIG + offset);
}

uint64_t virtio_config_u64(const struct virtio_dev *d, uint32_t offset) {
    return (uint64_t)read_reg(d, VIRTIO_CONFIG + offset) |
           ((uint64_t)read_reg(d, VIRTIO_CONFIG + offset + 4u) << 32);
}

/* Takes two pages for the rings: the descriptor table and the available ring
 * on the first, the used ring on the second. Whole pages because the
 * specification wants each aligned, and an allocation that is aligned by
 * construction cannot be misaligned by an arithmetic slip. */
static int build_queue(struct virtio_dev *d, uint32_t queue) {
    phys_t first = 0, second = 0;
    int rc = mem_alloc_pages(1, 1, &first);
    rc = rc != KERR_OK ? rc : mem_alloc_pages(1, 1, &second);
    if (rc != KERR_OK) {
        return rc;
    }
    struct virtq *q = &d->q[queue];
    /* **The device is given `first`/`second` and the kernel these.** The
     * two were the same number while the kernel was identity-mapped, which
     * is why the distinction never had to be made before. */
    q->desc_phys = first;
    q->avail_phys = first + 2048u;
    q->used_phys = second;
    q->desc = (struct virtq_desc *)mmu_phys_to_virt(first);
    q->avail = (struct virtq_avail *)mmu_phys_to_virt(first + 2048u);
    q->used = (struct virtq_used *)mmu_phys_to_virt(second);
    for (uint32_t i = 0; i < sizeof(struct virtq_desc) * VIRTIO_QUEUE_SIZE; i++) {
        ((volatile uint8_t *)q->desc)[i] = 0;
    }
    q->avail->flags = 0;
    q->avail->idx = 0;
    q->used->flags = 0;
    q->used->idx = 0;
    q->last_used = 0;
    q->free_head = 0;
    write_reg(d, VIRTIO_QUEUE_SEL, queue);
    write_reg(d, VIRTIO_QUEUE_NUM, VIRTIO_QUEUE_SIZE);
    write_reg(d, VIRTIO_QUEUE_DESC_LOW, (uint32_t)first);
    write_reg(d, VIRTIO_QUEUE_DESC_HIGH, (uint32_t)(first >> 32));
    write_reg(d, VIRTIO_QUEUE_AVAIL_LOW, (uint32_t)(first + 2048u));
    write_reg(d, VIRTIO_QUEUE_AVAIL_HIGH, (uint32_t)((first + 2048u) >> 32));
    write_reg(d, VIRTIO_QUEUE_USED_LOW, (uint32_t)second);
    write_reg(d, VIRTIO_QUEUE_USED_HIGH, (uint32_t)(second >> 32));
    write_reg(d, VIRTIO_QUEUE_READY, 1);
    return KERR_OK;
}

/* The specification's handshake, in its order. Each step is a write the
 * device may refuse, and the refusal is checked: a driver that writes
 * DRIVER_OK without confirming FEATURES_OK is talking to a device that has
 * already given up on it. */
static int negotiate(struct virtio_dev *d) {
    write_reg(d, VIRTIO_STATUS, 0); /* reset */
    write_reg(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE);
    write_reg(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    /* Accept VIRTIO_F_VERSION_1 and nothing else: it is the bit that says
     * "modern layout", and every other feature is an extension this driver
     * does not implement and must therefore not claim. */
    write_reg(d, VIRTIO_DRIVER_FEATURES_SEL, 1);
    write_reg(d, VIRTIO_DRIVER_FEATURES, 1u); /* bit 32 */
    write_reg(d, VIRTIO_DRIVER_FEATURES_SEL, 0);
    write_reg(d, VIRTIO_DRIVER_FEATURES, 0);
    write_reg(d, VIRTIO_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK);
    if ((read_reg(d, VIRTIO_STATUS) & STATUS_FEATURES_OK) == 0) {
        write_reg(d, VIRTIO_STATUS, STATUS_FAILED);
        return KERR_NOTSUP;
    }
    return KERR_OK;
}

/* Walks the tree for a virtio transport of the wanted type. Each
 * `virtio,mmio` node is a transport that may hold any device — or none, with
 * a device id of 0 — so the type is read from the register rather than from
 * the tree. */
static int probe_each(const struct fdt *fdt, uint32_t device_id, struct virtio_dev *out,
                      char *path, uint32_t cap, uint32_t *found_id) {
    phys_t base = 0;
    uint64_t size = 0;
    int rc;
    /* **Before any return.** This function promises to report an id, and it
     * has two failure paths that used to return without writing one — so the
     * caller added a garbage stack value into `occupied`, the counter whose
     * entire purpose is to tell "there are no devices" from "I could not see
     * the devices that are there". A diagnostic that lies in the failure case
     * is worse than no diagnostic, and that is the case it exists for.
     * Found by `clang-tidy` on its first run over this tree. */
    *found_id = 0;
    rc = fdt_device_reg(fdt, path, 0, &base, &size);
    if (rc != KERR_OK) {
        kprintf("virtio: %s has no readable reg (rc %d)\n", path, rc);
        return KERR_NOENT;
    }
    {
        void *at = mmu_map_device(base, size);
        if (at == NULL) {
            kprintf("virtio: %s at %016llx could not be mapped\n", path,
                    (unsigned long long)base);
            return KERR_NOMEM;
        }
        out->regs = (volatile uint32_t *)at;
    }
    out->base = base;
    if (read_reg(out, VIRTIO_MAGIC) != VIRTIO_MAGIC_VALUE) {
        kprintf("virtio: %s at %016llx reads magic %08x, not %08x\n", path,
                (unsigned long long)base, read_reg(out, VIRTIO_MAGIC), VIRTIO_MAGIC_VALUE);
        return KERR_NOENT;
    }
    if (read_reg(out, VIRTIO_VERSION) != VIRTIO_VERSION_MODERN) {
        /* Legacy version 1, whose queue layout differs. Refused rather than
         * half-supported — but **said out loud**, because this is exactly
         * how the first attempt here failed: an emulator offering legacy
         * transports by default looked identical to a machine with no
         * devices at all, and the silence was the bug. QEMU wants
         * `-global virtio-mmio.force-legacy=false`. */
        kprintf("virtio: %s is a legacy (version %u) transport; this driver needs version 2\n",
                path, read_reg(out, VIRTIO_VERSION));
        return KERR_NOTSUP;
    }
    *found_id = read_reg(out, VIRTIO_DEVICE_ID);
    if (*found_id != device_id) {
        return KERR_NOENT;
    }
    out->device_id = device_id;
    out->irq = fdt_get_u32(fdt, path, "interrupts", 0);
    for (uint32_t i = 0; i < cap && path[i] != '\0'; i++) {
        out->path[i] = path[i];
        out->path[i + 1u] = '\0';
    }
    return KERR_OK;
}

int virtio_find(const struct fdt *fdt, uint32_t device_id, uint32_t queues,
                struct virtio_dev *out) {
    uint32_t slots = 0, occupied = 0;
    /* The tree lists every transport slot, most of them empty. Each
     * candidate is probed and the first that holds the wanted device wins.
     *
     * What it found is reported when it finds nothing: "no device" and "I
     * could not see the devices that are there" look identical otherwise,
     * and they have completely different causes. */
    for (uint32_t skip = 0; skip < 64u; skip++) {
        char path[64];
        uint32_t id = 0;
        int rc = fdt_find_compatible_nth(fdt, "virtio,mmio", skip, path, sizeof path);
        if (rc != KERR_OK) {
            break;
        }
        slots++;
        rc = probe_each(fdt, device_id, out, path, sizeof path, &id);
        occupied += id != 0 ? 1u : 0u;
        if (rc != KERR_OK) {
            continue;
        }
        rc = negotiate(out);
        out->queues = 0;
        for (uint32_t qi = 0; rc == KERR_OK && qi < queues && qi < VIRTIO_MAX_QUEUES; qi++) {
            rc = build_queue(out, qi);
            out->queues += rc == KERR_OK ? 1u : 0u;
        }
        if (rc != KERR_OK) {
            return rc;
        }
        write_reg(out, VIRTIO_STATUS,
                  STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK);
        return KERR_OK;
    }
    kprintf("virtio: %u transport slots, %u occupied, none of type %u\n", slots, occupied,
            device_id);
    return KERR_NOENT;
}

/* Links the buffers into a descriptor chain, **always from descriptor 0**.
 *
 * This driver is synchronous: one request is outstanding at a time, and
 * `virtio_request` does not return until the device has finished. So the
 * same descriptors can be reused for every request, and there is no free
 * list to maintain and nothing to reclaim from the used ring.
 *
 * The first version advanced a cursor instead and never gave descriptors
 * back, which worked for exactly one request and then timed out — the
 * device was handed a chain built out of descriptors it still owned. A
 * cursor is the right answer for a driver with several requests in flight,
 * and that driver would also need to free them; neither is worth having
 * until something asks for the concurrency. */
static uint16_t chain(struct virtio_dev *d, uint32_t queue, const void *const *bufs,
                      const uint32_t *lens, const bool *writable, uint32_t count) {
    struct virtq *q = &d->q[queue];
    for (uint32_t i = 0; i < count; i++) {
        /* The device fetches from this address, so it is the bus one. */
        q->desc[i].addr = (uint64_t)mmu_virt_to_phys(bufs[i]);
        q->desc[i].len = lens[i];
        q->desc[i].flags = (uint16_t)((writable[i] ? VIRTQ_DESC_F_WRITE : 0u) |
                                        (i + 1u < count ? VIRTQ_DESC_F_NEXT : 0u));
        q->desc[i].next = (uint16_t)(i + 1u);
    }
    return 0;
}

int virtio_request(struct virtio_dev *d, const void *const *bufs, const uint32_t *lens,
                   const bool *writable, uint32_t count) {
    return virtio_request_on(d, 0, bufs, lens, writable, count);
}

int virtio_request_on(struct virtio_dev *d, uint32_t queue, const void *const *bufs,
                      const uint32_t *lens, const bool *writable, uint32_t count) {
    struct virtq *q = &d->q[queue];
    uint16_t head;
    if (count == 0 || count > VIRTIO_QUEUE_SIZE || queue >= d->queues) {
        return KERR_INVAL;
    }
    head = chain(d, queue, bufs, lens, writable, count);
    q->avail->ring[q->avail->idx % VIRTIO_QUEUE_SIZE] = head;
    /* The ring must be visible before the index that publishes it, and the
     * index before the notification. Two fences, because the device may be
     * another core or a model running on one. */
    __asm__ volatile("fence w, w" : : : "memory");
    q->avail->idx++;
    __asm__ volatile("fence w, w" : : : "memory");
    write_reg(d, VIRTIO_QUEUE_NOTIFY, queue);
    for (uint32_t spin = 0; spin < WAIT_SPINS; spin++) {
        __asm__ volatile("fence r, r" : : : "memory");
        if (q->used->idx != q->last_used) {
            q->last_used = q->used->idx;
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

bool virtio_ack_interrupt(struct virtio_dev *d) {
    uint32_t status = read_reg(d, VIRTIO_INTERRUPT_STATUS);
    if (status == 0) {
        return false;
    }
    write_reg(d, VIRTIO_INTERRUPT_ACK, status);
    return true;
}

void virtio_post_buffer(struct virtio_dev *d, uint32_t queue, uint16_t index, phys_t buf,
                        uint32_t len) {
    struct virtq *q = &d->q[queue];
    if (queue >= d->queues || index >= VIRTIO_QUEUE_SIZE) {
        return;
    }
    q->desc[index].addr = buf;
    q->desc[index].len = len;
    q->desc[index].flags = VIRTQ_DESC_F_WRITE; /* the device writes a frame into it */
    q->desc[index].next = 0;
    q->avail->ring[q->avail->idx % VIRTIO_QUEUE_SIZE] = index;
    /* The ring entry before the index that publishes it. */
    __asm__ volatile("fence w, w" : : : "memory");
    q->avail->idx++;
}

void virtio_kick(struct virtio_dev *d, uint32_t queue) {
    if (queue >= d->queues) {
        return;
    }
    __asm__ volatile("fence w, w" : : : "memory");
    write_reg(d, VIRTIO_QUEUE_NOTIFY, queue);
}

bool virtio_take_completion(struct virtio_dev *d, uint32_t queue, uint16_t *index,
                            uint32_t *len) {
    struct virtq *q = &d->q[queue];
    const struct virtq_used_elem *e;
    if (queue >= d->queues) {
        return false;
    }
    __asm__ volatile("fence r, r" : : : "memory");
    if (q->used->idx == q->last_used) {
        return false;
    }
    e = &q->used->ring[q->last_used % VIRTIO_QUEUE_SIZE];
    *index = (uint16_t)e->id;
    *len = e->len;
    q->last_used++;
    return true;
}
