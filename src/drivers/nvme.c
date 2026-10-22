#include "drivers/nvme.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "drivers/pci.h"
#include "kernel/mem.h"

/* Controller registers, in BAR0. */
#define NVME_CAP 0x00u      /* capabilities, 64-bit */
#define NVME_CC 0x14u       /* configuration */
#define NVME_CSTS 0x1cu     /* status */
#define NVME_AQA 0x24u      /* admin queue attributes */
#define NVME_ASQ 0x28u      /* admin submission queue address */
#define NVME_ACQ 0x30u      /* admin completion queue address */
#define NVME_DOORBELL 0x1000u

#define CC_ENABLE 1u
#define CC_IOSQES_SHIFT 16u  /* log2 of a submission entry: 64 bytes */
#define CC_IOCQES_SHIFT 20u  /* log2 of a completion entry: 16 bytes */
#define CSTS_READY 1u
#define CSTS_FATAL 2u

/* Admin commands. */
#define ADMIN_CREATE_SQ 0x01u
#define ADMIN_CREATE_CQ 0x05u
#define ADMIN_IDENTIFY 0x06u

/* I/O commands. */
#define IO_WRITE 0x01u
#define IO_READ 0x02u

#define QUEUE_ENTRIES 32u
#define ADMIN_QUEUE 0u
#define IO_QUEUE 1u

#define READY_SPINS 10000000u

/* A 64-byte submission entry, as the specification lays it out. Only the
 * fields this driver sets are named; the rest must be zero, which is why
 * every command starts from a cleared structure. */
struct nvme_command {
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t namespace_id;
    uint64_t reserved;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct nvme_completion {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t command_id;
    uint16_t status;  /* bit 0 is the phase; the rest is the status code */
};

/* One queue pair. */
struct nvme_queue {
    struct nvme_command *sq;
    struct nvme_completion *cq;
    phys_t sq_phys;
    phys_t cq_phys;
    uint16_t sq_tail;
    uint16_t cq_head;
    bool phase;      /* what a *new* completion's phase bit will be */
    uint32_t id;
};

static struct {
    volatile uint32_t *regs;
    struct pci_device pci;
    struct nvme_queue admin;
    struct nvme_queue io;
    uint32_t doorbell_stride;
    uint64_t blocks;
    uint32_t block_size;
    char model[41];
    uint16_t next_command_id;
    bool up;
} nvme;

static void write_reg(uint32_t off, uint32_t v) {
    nvme.regs[off / 4u] = v;
}

static uint32_t read_reg(uint32_t off) {
    return nvme.regs[off / 4u];
}

static void write_reg64(uint32_t off, uint64_t v) {
    write_reg(off, (uint32_t)v);
    write_reg(off + 4u, (uint32_t)(v >> 32));
}

/* The doorbell for a queue's submission tail or completion head. The stride
 * comes from the controller's capabilities, because an implementation may
 * space them further apart than the minimum. */
static void ring(uint32_t queue, bool completion, uint16_t value) {
    uint32_t off = NVME_DOORBELL + (2u * queue + (completion ? 1u : 0u)) * nvme.doorbell_stride;
    write_reg(off, value);
}

/* Two pages for a queue pair, which is more than 32 entries of either kind
 * needs and keeps each ring page-aligned — a requirement for the addresses
 * handed to the controller. */
static int build_queue(struct nvme_queue *q, uint32_t id) {
    phys_t sq = 0, cq = 0;
    int rc = mem_alloc_pages(1, 1, &sq);
    rc = rc != KERR_OK ? rc : mem_alloc_pages(1, 1, &cq);
    if (rc != KERR_OK) {
        return rc;
    }
    q->sq = (struct nvme_command *)(uintptr_t)sq;
    q->cq = (struct nvme_completion *)(uintptr_t)cq;
    q->sq_phys = sq;
    q->cq_phys = cq;
    q->sq_tail = 0;
    q->cq_head = 0;
    q->id = id;
    /* A completion's phase starts at 1 because the rings start zeroed: the
     * controller writes 1 into the first pass's entries. */
    q->phase = true;
    for (uint32_t i = 0; i < 4096u; i++) {
        ((volatile uint8_t *)q->sq)[i] = 0;
        ((volatile uint8_t *)q->cq)[i] = 0;
    }
    return KERR_OK;
}

/* Submits one command and waits for its completion.
 *
 * Synchronous, like the block driver beside it: the caller cannot proceed
 * without the answer, and a queue depth of one needs no tracking of which
 * completion belongs to whom. */
static int submit(struct nvme_queue *q, const struct nvme_command *cmd, uint32_t *result) {
    volatile struct nvme_completion *c = &q->cq[q->cq_head];
    q->sq[q->sq_tail] = *cmd;
    q->sq[q->sq_tail].command_id = nvme.next_command_id++;
    q->sq_tail = (uint16_t)((q->sq_tail + 1u) % QUEUE_ENTRIES);
    __asm__ volatile("fence w, w" : : : "memory");
    ring(q->id, false, q->sq_tail);
    for (uint32_t spin = 0; spin < READY_SPINS; spin++) {
        __asm__ volatile("fence r, r" : : : "memory");
        /* A completion is new when its phase matches what this pass
         * expects. The entries are never cleared, so this bit is the only
         * thing distinguishing a fresh completion from last pass's. */
        if (((c->status & 1u) != 0) == q->phase) {
            uint16_t status = (uint16_t)(c->status >> 1);
            if (result != NULL) {
                *result = c->result;
            }
            q->cq_head = (uint16_t)((q->cq_head + 1u) % QUEUE_ENTRIES);
            /* The expected phase inverts when the ring wraps — the detail a
             * driver that works for 32 commands and hangs on the 33rd has
             * got wrong (nvme.h). */
            q->phase = q->cq_head == 0 ? !q->phase : q->phase;
            ring(q->id, true, q->cq_head);
            return status == 0 ? KERR_OK : KERR_IO;
        }
    }
    return KERR_TIMEDOUT;
}

/* Waits for the controller's ready bit to reach `want`. */
static int wait_ready(bool want) {
    for (uint32_t spin = 0; spin < READY_SPINS; spin++) {
        uint32_t csts = read_reg(NVME_CSTS);
        if ((csts & CSTS_FATAL) != 0) {
            return KERR_IO;
        }
        if (((csts & CSTS_READY) != 0) == want) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Identify: 4 KiB of description, either of the controller or of a
 * namespace. */
static int identify(uint32_t which, uint32_t nsid, void *buf) {
    struct nvme_command cmd = {0};
    cmd.opcode = ADMIN_IDENTIFY;
    cmd.namespace_id = nsid;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = which;
    return submit(&nvme.admin, &cmd, NULL);
}

/* The model string, which the controller reports space-padded rather than
 * terminated. */
static void take_model(const uint8_t *identify_page) {
    uint32_t end = 40u;
    for (uint32_t i = 0; i < 40u; i++) {
        nvme.model[i] = (char)identify_page[24u + i];
    }
    while (end > 0 && (nvme.model[end - 1u] == ' ' || nvme.model[end - 1u] == '\0')) {
        end--;
    }
    nvme.model[end] = '\0';
}

/* Namespace 1's geometry: how many blocks, and how big each is. The size is
 * reported as a log2 in a table of formats, indexed by the format the
 * namespace is currently using. */
static void take_geometry(const uint8_t *ns_page) {
    uint64_t blocks = 0;
    uint32_t format_index, lbaf;
    for (uint32_t i = 0; i < 8u; i++) {
        blocks |= (uint64_t)ns_page[i] << (8u * i);
    }
    format_index = ns_page[26] & 0xfu;
    lbaf = (uint32_t)ns_page[128u + format_index * 4u + 2u];
    nvme.blocks = blocks;
    nvme.block_size = 1u << lbaf;
}

static int create_io_queues(void) {
    struct nvme_command cmd = {0};
    int rc = build_queue(&nvme.io, IO_QUEUE);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The completion queue first: a submission queue names the completion
     * queue it reports into, so the order is not a preference. */
    cmd.opcode = ADMIN_CREATE_CQ;
    cmd.prp1 = nvme.io.cq_phys;
    cmd.cdw10 = (uint32_t)IO_QUEUE | ((QUEUE_ENTRIES - 1u) << 16);
    cmd.cdw11 = 1u; /* physically contiguous; no interrupts, since this polls */
    rc = submit(&nvme.admin, &cmd, NULL);
    if (rc != KERR_OK) {
        return rc;
    }
    cmd = (struct nvme_command){0};
    cmd.opcode = ADMIN_CREATE_SQ;
    cmd.prp1 = nvme.io.sq_phys;
    cmd.cdw10 = (uint32_t)IO_QUEUE | ((QUEUE_ENTRIES - 1u) << 16);
    cmd.cdw11 = 1u | ((uint32_t)IO_QUEUE << 16); /* contiguous, reports into our CQ */
    return submit(&nvme.admin, &cmd, NULL);
}

/* One transfer. A single page per command, which bounds a request to a page
 * and needs no scatter list: the buffer cache reads a block at a time, and a
 * driver that supports more without a caller asking is untested code. */
static int transfer(uint8_t opcode, uint64_t lba, uint32_t blocks, void *buf) {
    struct nvme_command cmd = {0};
    cmd.opcode = opcode;
    cmd.namespace_id = 1;
    cmd.prp1 = (uint64_t)(uintptr_t)buf;
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = blocks - 1u; /* zero-based, as the specification has it */
    return submit(&nvme.io, &cmd, NULL);
}

static int nvme_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!nvme.up || count == 0 || lba + count > nvme.blocks) {
        return KERR_INVAL;
    }
    return transfer(IO_READ, lba, count, buf);
}

static int nvme_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!nvme.up || count == 0 || lba + count > nvme.blocks) {
        return KERR_INVAL;
    }
    return transfer(IO_WRITE, lba, count, (void *)(uintptr_t)buf);
}

/* Nothing to flush: this driver writes through, with no volatile cache of
 * its own. The drive may have one, and flushing it is an NVMe command this
 * kernel will need when it starts caring about power loss. */
static int nvme_flush(void *self) {
    (void)self;
    return nvme.up ? KERR_OK : KERR_NODEV;
}

/* Brings the controller up: reset, admin queues, enable, identify. */
static int start_controller(void) {
    uint64_t cap = (uint64_t)read_reg(NVME_CAP) | ((uint64_t)read_reg(NVME_CAP + 4u) << 32);
    int rc;
    nvme.doorbell_stride = 4u << ((cap >> 32) & 0xfu);
    write_reg(NVME_CC, 0); /* disable before touching the admin queue addresses */
    rc = wait_ready(false);
    rc = rc != KERR_OK ? rc : build_queue(&nvme.admin, ADMIN_QUEUE);
    if (rc != KERR_OK) {
        return rc;
    }
    write_reg(NVME_AQA, (QUEUE_ENTRIES - 1u) | ((QUEUE_ENTRIES - 1u) << 16));
    write_reg64(NVME_ASQ, nvme.admin.sq_phys);
    write_reg64(NVME_ACQ, nvme.admin.cq_phys);
    write_reg(NVME_CC, CC_ENABLE | (6u << CC_IOSQES_SHIFT) | (4u << CC_IOCQES_SHIFT));
    return wait_ready(true);
}

int nvme_init(struct blockdev *out) {
    uint8_t *page;
    int rc = pci_find_class(NVME_PCI_CLASS, &nvme.pci);
    if (rc != KERR_OK) {
        return rc;
    }
    nvme.regs = (volatile uint32_t *)(uintptr_t)nvme.pci.bar0;
    rc = start_controller();
    if (rc != KERR_OK) {
        return rc;
    }
    page = kzalloc(4096u);
    if (page == NULL) {
        return KERR_NOMEM;
    }
    rc = identify(1u, 0, page); /* the controller */
    if (rc == KERR_OK) {
        take_model(page);
        rc = identify(0u, 1u, page); /* namespace 1 */
    }
    if (rc == KERR_OK) {
        take_geometry(page);
        rc = create_io_queues();
    }
    kfree(page);
    if (rc != KERR_OK) {
        return rc;
    }
    nvme.up = true;
    *out = (struct blockdev){.read = nvme_read, .write = nvme_write, .flush = nvme_flush,
                             .self = NULL, .block_size = nvme.block_size, .blocks = nvme.blocks,
                             .read_only = false};
    return KERR_OK;
}

const char *nvme_model(void) {
    return nvme.up ? nvme.model : "";
}

uint64_t nvme_blocks(void) {
    return nvme.blocks;
}

uint32_t nvme_block_size(void) {
    return nvme.block_size;
}
