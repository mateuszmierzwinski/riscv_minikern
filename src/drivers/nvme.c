#include "drivers/nvme.h"

#include "arch/riscv/mmu.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "drivers/pci.h"
#include "arch/riscv/plic.h"
#include "kernel/irqwait.h"
#include "kernel/mem.h"

/* Controller registers, in BAR0. */
#define NVME_CAP 0x00u      /* capabilities, 64-bit */
#define NVME_CC 0x14u       /* configuration */
#define NVME_CSTS 0x1cu     /* status */
#define NVME_INTMS 0x0cu    /* interrupt mask set — write 1s to mask */
#define NVME_INTMC 0x10u    /* interrupt mask clear — write 1s to unmask */
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
#define IO_DSM 0x09u          /* Dataset Management: here, deallocate */
#define DSM_DEALLOCATE 4u     /* cdw11 bit 2, "AD" */
#define ONCS_DSM 4u           /* Identify Controller ONCS bit 2: DSM supported */
#define IDENTIFY_ONCS 520u    /* offset of ONCS, 16 bits */
#define IDENTIFY_DLFEAT 33u   /* namespace: what a deallocated block reads as */
#define DLFEAT_READS_ZERO 1u  /* bits 2:0 = 001b */

#define QUEUE_ENTRIES 32u
#define ADMIN_QUEUE 0u
#define IO_QUEUE 1u

#define READY_SPINS 10000000u
/* Generous on purpose: this is the sleep's deadline, not the command's. An
 * NVMe command that has not completed in two seconds has failed, and the
 * spin loop below is what decides that — the deadline only has to be longer
 * than a slow transfer so a working device is never reported as missing its
 * interrupt. */
#define COMMAND_TIMEOUT_NS 2000000000ull

/* **Quiets the pin, which is the whole job of this callback.**
 *
 * An NVMe legacy pin is level-triggered and stays asserted until the
 * completion is *consumed* — and consuming it means advancing the head
 * doorbell, which only the waiter can do, after it has read the entry. So a
 * handler that merely notes the firing leaves the line up, the controller
 * re-raises immediately, and the trap is re-entered before the waiter ever
 * runs again: the machine takes one interrupt for ever and does nothing
 * else. That is exactly how the first attempt at this hung, with the source
 * number on the wire and nothing after it.
 *
 * Masking vector 0 here breaks that loop. `submit` unmasks before each
 * command, so the mask is always lifted by whoever is about to need it, and
 * a mask left behind by an abandoned command heals on the next one rather
 * than disabling the device for ever.
 *
 * **This is not sufficient on this board**, and the reason is worth having
 * before anyone tries again. The drive's pin does not reach the PLIC: the
 * JH7110's PCIe controller aggregates `INTA`–`INTD` and MSI into *one*
 * source — 57, which is the single cell its node declares, and that number
 * is right. But the controller latches the condition in its own status
 * register, and the PLIC line stays asserted until *that* is cleared. So
 * masking the drive's vector 0, as below, quiets the drive and leaves the
 * bridge asserting. Enabling source 57 then takes one interrupt for ever.
 *
 * Finishing it means driving the controller's interrupt demultiplexer — the
 * PLDA XpressRICH `ISTATUS_LOCAL` / `ISTATUS_MSI` family — which is a
 * separate piece of work from converting this driver, and is why the switch
 * below defaults off. `minikern.nvmeirqprobe` reports the number without
 * enabling it, which is how that number was read without a third storm.
 *
 * It writes, which `irqwait.h` describes as a read — the SD driver's handler
 * does the same thing for the same reason (`put(DW_RINTSTS, ...)`): a device
 * whose interrupt is not acknowledged in the handler cannot be waited on at
 * all. The contract that matters is the one the header gives next: do not
 * take the kernel lock, which this does not. */
static uint32_t quiet_the_pin(void);

/* The completion wait's state. Declared here rather than beside the function
 * that arms it because `submit` below is the user, and this file reads top to
 * bottom; the reasoning for the whole mechanism is on `nvme_use_interrupts`. */
static struct irqwait nvme_done;
static bool nvme_irq_armed;
static bool nvme_probe_only;
static bool nvme_said_how_the_first_went;
static bool nvme_handler_spoke;

void nvme_irq_probe_only(bool on) {
    nvme_probe_only = on;
}
static uint64_t nvme_interrupts_missed;


/* The page size the controller is configured for: CC.MPS of 0 means 4 KiB,
 * which is what this kernel sets. A PRP entry addresses within one of these. */
#define PAGE_BYTES 4096u

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
    bool can_deallocate;   /* ONCS says Dataset Management is there */
    bool dealloc_zeroes;   /* DLFEAT says a deallocated block reads as zeroes */
} nvme;

static void write_reg(uint32_t off, uint32_t v) {
    nvme.regs[off / 4u] = v;
}

static uint32_t nvme_plic_source;

static uint32_t quiet_the_pin(void) {
    /* **The device first, the bridge second.** The controller aggregates
     * INTA-INTD into one PLIC source and latches which fired; clearing that
     * latch while this device is still asserting simply sets it again. So
     * the drive's vector is masked here, which drops its pin, and only then
     * is the bridge's latch cleared. Reversing these two is the difference
     * between a working wait and a machine that takes one interrupt for
     * ever — which is how this arrived at two functions. */
    /* **Masked at the interrupt controller, because nothing else can drop
     * the line here.**
     *
     * A pin-based NVMe interrupt is deasserted when the completion is
     * *consumed* — when the head doorbell moves — and only the waiter can
     * do that, after it has read the entry. Masking the drive's vector was
     * tried and is not enough: a pending assertion survives it. Clearing
     * the bridge's latch was tried and is not enough either: the drive
     * re-asserts and it latches again. Both of those were a boot each.
     *
     * So the source is masked at the PLIC, which is the one thing that
     * cannot be re-asserted from outside, and `submit` re-enables it once
     * the completion has been taken off the ring. That is the ordinary
     * treatment of a level-triggered line shared by a whole bridge, and it
     * is what this should have been from the start. */
    plic_disable(nvme_plic_source);
    write_reg(NVME_INTMS, 1u);
    {
        uint32_t intx = pci_bridge_intx_clear();
        /* **Once, and only to establish that this runs at all.** The wait
         * region does not terminate although it spins against a deadline,
         * and the two explanations — the handler never runs, or it runs and
         * something after it does not return — want opposite
         * investigations. One line, guarded, cannot flood even if this is
         * being re-entered. */
        if (!nvme_handler_spoke) {
            nvme_handler_spoke = true;
            kprintf("nvme: the handler ran; intx %x\n", intx);
        }
        return intx;
    }
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
    /* **This driver already kept the bus addresses separately** — `sq_phys`
     * and `cq_phys` below — so only the kernel's own view needed changing.
     * The separation was there for the controller's sake and happens to be
     * exactly what an un-mapped kernel needs. */
    q->sq = (struct nvme_command *)mmu_phys_to_virt(sq);
    q->cq = (struct nvme_completion *)mmu_phys_to_virt(cq);
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
    /* **Stale firings are forgotten before the doorbell, not after it.**
     *
     * A legacy-pin device holds its line asserted until the completion is
     * consumed, and the completion is consumed by the loop below advancing
     * the head — so between the wait returning and the head moving, the PLIC
     * can re-raise and set `fired` again. Clearing it here rather than after
     * the transfer means the next wait cannot return immediately on the
     * last transfer's echo and then find no completion in the ring. */
    if (nvme_irq_armed && !nvme_said_how_the_first_went) {
        /* **Which side of the wait the hang is on.**
         *
         * `irqwait_wait` spins against a deadline and cannot hang, yet the
         * report after it never appears — so either the first command is
         * never submitted, or the hart is re-entering the trap and the spin
         * never advances. This says which, and the two want opposite
         * investigations. */
        kprintf("nvme: submitting the first waited command\n");
    }
    irqwait_rearm(&nvme_done);
    /* **Unmasked per command, not once at arming.** The handler masks to
     * stop a level-triggered pin re-raising, so something has to lift it;
     * doing it here makes the lift belong to the command that needs it and
     * makes a mask left over from an abandoned command self-healing. */
    if (nvme_irq_armed) {
        write_reg(NVME_INTMC, 1u);
    }
    ring(q->id, false, q->sq_tail);
    /* **The interrupt ends the sleep; the phase bit remains the truth.**
     *
     * The loop below is unchanged and still decides whether a completion is
     * there. All the wait does is stop this hart spinning — and therefore
     * stop it holding the kernel lock — for the length of the transfer. A
     * timeout is not an error here: it falls into the same loop and the
     * command still completes, just as slowly as before. That is what makes
     * this safe to add to a working driver, and `irqwait_counts` is what
     * says whether it is actually helping. */
    if (nvme_irq_armed) {
        int waited = irqwait_wait(&nvme_done, COMMAND_TIMEOUT_NS);
        if (waited != KERR_OK) {
            nvme_interrupts_missed++;
        }
        /* **Once, for the first command only.** Whether the very first
         * interrupt-driven command was woken or fell back to its timeout is
         * the one fact that separates "the source and the demultiplexer are
         * right" from "something else is". Printing it per command would
         * flood; printing it at the end of the boot is no use when the boot
         * does not reach the end, which is exactly the case being
         * diagnosed. */
        if (!nvme_said_how_the_first_went) {
            nvme_said_how_the_first_went = true;
            kprintf("nvme: the first waited command was %s (intx %u)\n",
                    waited == KERR_OK ? "woken" : "left to its timeout", nvme_done.status);
        }
    }
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
            /* **The head has moved, so the drive has stopped asserting**:
             * the source the handler masked can be let back in. Doing it
             * here rather than at the top of the next command means an
             * interrupt that arrives between two commands is still
             * delivered rather than silently dropped. */
            if (nvme_irq_armed) {
                plic_enable(nvme_plic_source);
            }
            return status == 0 ? KERR_OK : KERR_IO;
        }
    }
    return KERR_TIMEDOUT;
}

/* ── waiting for a completion instead of watching for one ──────────────── */

/* **Why this driver polled, and why it need not.**
 *
 * A completion is a phase bit in a ring, so spinning on it is correct, and it
 * was the right first implementation. What it costs is not the spinning: one
 * hart is inside this kernel at a time, so a hart spinning on a completion is
 * a hart no other hart can replace, and the lock is held for the whole
 * transfer. On the fastest device on the board that is the longest the lock
 * is held by anything.
 *
 * **The polled path stays, and is reached every boot.** The admin queue
 * identifies the controller and creates the I/O queues during device
 * bring-up, before the interrupt controller exists, so the first commands
 * have no interrupt to wait for — and the root filesystem may be on this
 * disk, which is to say the kernel may be reading it before it can be told
 * about it.
 *
 * **The mask is the ordering trick.** A completion queue's interrupt is
 * requested when the queue is created, which happens long before a handler
 * exists; an interrupt nobody claims is one the PLIC re-raises for ever, and
 * then the machine does nothing else. So the controller's own mask
 * (`INTMS`) is set before any queue is created and cleared only once the
 * handler is installed — the same "mask last" rule the SD driver records,
 * enforced one level further out because here the device is configured
 * before the kernel is ready for it.
 */

/* Waits that ended in a timeout rather than an interrupt.
 *
 * Reported for the reason `irqwait.h` gives: a driver that has silently
 * fallen back to its timeout on every transfer is slower than the polling it
 * replaced and looks identical from outside. */
uint64_t nvme_interrupts_missed_count(void) {
    return nvme_interrupts_missed;
}

static int nvme_read(void *self, uint64_t lba, uint32_t count, void *buf);

/* Finds which word of the bridge's window a completion moves, and which bit.
 *
 * **The measurement this driver's one guess needs.** `ISTATUS_LOCAL 0x184`
 * and `INTA` at bit 24 are the PLDA offsets as Linux uses them for this IP;
 * with them the first interrupt-driven command hangs inside the trap
 * handler, which is what an uncleared latch looks like. Four register maps
 * in this kernel have been guessed wrong and every one was settled by
 * dumping the window in a single boot, so this does that: snapshot the
 * window, let the drive complete a command with its own interrupt
 * unmasked, snapshot again, and print what differs.
 *
 * Nothing is enabled at the interrupt controller, so a wrong guess costs a
 * boot's output rather than a hung board. The drive's own mask is opened
 * because a device that is not asserting cannot make the bridge latch
 * anything — that is the whole point of the exercise.
 */
void nvme_measure_intx(const struct fdt *fdt) {
    enum { FIRST = 0x100u / 4u, LAST = 0x200u / 4u };
    const volatile uint32_t *apb;
    uint64_t bytes = 0;
    uint32_t before[LAST - FIRST];
    uint8_t block[512];
    if (!nvme.up) {
        return;
    }
    apb = pci_bridge_apb(fdt, &bytes);
    if (apb == NULL || bytes < 0x200u) {
        kprintf("nvme: no apb window to measure\n");
        return;
    }
    for (uint32_t i = 0; i < LAST - FIRST; i++) {
        before[i] = apb[FIRST + i];
    }
    write_reg(NVME_INTMC, 1u); /* let the drive assert; nothing listens */
    (void)nvme_read(NULL, 0, 1, block);
    for (uint32_t i = 0; i < LAST - FIRST; i++) {
        uint32_t now = apb[FIRST + i];
        if (now != before[i]) {
            kprintf("nvme: apb +%03x changed %08x -> %08x\n", (FIRST + i) * 4u, before[i], now);
        }
    }
    kprintf("nvme: apb +180 %08x +184 %08x (the assumed mask and status)\n",
            apb[0x180u / 4u], apb[0x184u / 4u]);
    /* **Does the latch answer to a write of 1s?** Nothing read can say, and
     * it is the question the hang turns on: if bit 24 clears and stays
     * clear, the offsets are right and the fault is re-assertion; if it
     * does not clear, the register or its semantics are wrong. The drive is
     * masked first so it cannot be re-asserting while this is tried. */
    write_reg(NVME_INTMS, 1u);
    pci_bridge_apb_poke(fdt, 0x184u, 0x0f000000u);
    kprintf("nvme: after writing 1s to +184 with the drive masked: %08x\n",
            apb[0x184u / 4u]);
    pci_bridge_apb_poke(fdt, 0x184u, 0xffffffffu);
    kprintf("nvme: after writing 1s to every bit of +184: %08x\n", apb[0x184u / 4u]);
    /* **Do bits 19 and 23 come back?** That is the question the hang turns
     * on now. If they re-assert on their own they are a live source, and
     * whether they can pull the line depends on the mask's polarity —
     * `+180` reads `ff77ffff`, which either enables everything but those
     * two or enables only those two, and the two readings demand opposite
     * writes. A source that re-asserts while enabled is a permanent
     * assertion, which is what arming the PLIC walks into. */
    for (uint32_t look = 0; look < 3u; look++) {
        kprintf("nvme: +184 a moment later: %08x\n", apb[0x184u / 4u]);
    }
}

int nvme_use_interrupts(const struct fdt *fdt) {
    const char *bridge = pci_bridge_path();
    uint32_t source;
    int rc;
    if (!nvme.up || nvme_irq_armed) {
        return nvme.up ? KERR_OK : KERR_NODEV;
    }
    if (bridge == NULL) {
        kprintf("nvme: no host bridge path; completions stay polled\n");
        return KERR_NOENT;
    }
    /* The bridge's line, not the device's: see `pci_bridge_path`. */
    source = fdt_get_u32(fdt, bridge, "interrupts", 0);
    if (source == 0u) {
        kprintf("nvme: no interrupt in the tree for %s; completions stay polled\n", bridge);
        return KERR_NOENT;
    }
    /* **The whole property, before trusting one cell of it.**
     *
     * Enabling a PLIC source that is already asserted — or that belongs to
     * something else — is a machine that takes one interrupt for ever, and
     * two boots were spent that way on the first cell of this property. A
     * PCIe bridge node commonly carries several interrupts (the legacy pins
     * and the MSI aggregate are not the same line), so the cells and the
     * names are reported and the choice is made from facts. This kernel has
     * paid for guessing a register map four times; a source number is the
     * same kind of guess. */
    {
        struct fdt_prop p;
        if (fdt_get_prop(fdt, bridge, "interrupts", &p) == KERR_OK) {
            kprintf("nvme: %s interrupts: %u bytes:", bridge, p.len);
            for (uint32_t i = 0; i + 3u < p.len && i < 64u; i += 4u) {
                uint32_t v = ((uint32_t)p.value[i] << 24) | ((uint32_t)p.value[i + 1] << 16) |
                             ((uint32_t)p.value[i + 2] << 8) | (uint32_t)p.value[i + 3];
                kprintf(" %u", v);
            }
            kprintf("\n");
        }
        if (fdt_get_prop(fdt, bridge, "interrupt-names", &p) == KERR_OK) {
            kprintf("nvme: names:");
            for (uint32_t i = 0; i < p.len && i < 96u; i++) {
                kprintf("%c", p.value[i] == 0 ? ' ' : (char)p.value[i]);
            }
            kprintf("\n");
        }
        /* **The bridge's register windows, by name.**
         *
         * Finishing this needs the controller's own interrupt status, which
         * is not in ECAM — it is in a second window the node names. Which
         * window, and what it is called, is a fact of this board rather
         * than of the IP, so it is read rather than assumed: four register
         * maps have been guessed wrong in this tree and each cost a boot.
         * The PLDA XpressRICH registers this needs (`IMASK_LOCAL 0x180`,
         * `ISTATUS_LOCAL 0x184`, INTA-INTD at bits 24-27) are offsets
         * *within* whichever of these is the bridge's control space. */
        if (fdt_get_prop(fdt, bridge, "reg-names", &p) == KERR_OK) {
            kprintf("nvme: reg-names:");
            for (uint32_t i = 0; i < p.len && i < 96u; i++) {
                kprintf("%c", p.value[i] == 0 ? ' ' : (char)p.value[i]);
            }
            kprintf("\n");
        }
        for (uint32_t which = 0; which < 4u; which++) {
            phys_t base = 0;
            uint64_t size = 0;
            if (fdt_device_reg(fdt, bridge, which, &base, &size) != KERR_OK) {
                break;
            }
            kprintf("nvme: reg[%u] %016llx + %llx\n", which, (unsigned long long)base,
                    (unsigned long long)size);
        }
        if (fdt_get_prop(fdt, bridge, "msi-parent", &p) == KERR_OK) {
            kprintf("nvme: the bridge has an msi-parent, so its pins may be MSI-routed\n");
        }
    }
    /* `minikern.nvmeirqprobe` reports and stops, which is how this number
     * gets read without risking the storm that reading it wrongly causes. */
    if (nvme_probe_only) {
        return KERR_NOENT;
    }
    /* **Said before it is acted on.** Unmasking a source nothing claims is a
     * machine that does nothing else, including printing, so the number this
     * is about to trust goes out while the console still works. The first
     * attempt hung here with nothing on the wire, which is what that costs. */
    kprintf("nvme: bridge %s says interrupt %u\n", bridge, source);
    /* **The demultiplexer before the handler**, because the handler clears
     * through it: armed the other way round, the first interrupt would be
     * acknowledged at the drive and left latched at the bridge. */
    rc = pci_bridge_intx_init(fdt);
    if (rc != KERR_OK) {
        kprintf("nvme: the bridge's INTx cannot be demultiplexed (rc %d); staying polled\n", rc);
        return rc;
    }
    /* **The number the handler needs, before the handler can run.**
     *
     * `irqwait_arm` installs the handler *and* enables the source, so
     * anything set afterwards is not set for the first interrupt — and this
     * handler masks `nvme_plic_source`, which would still be zero. That is
     * this file's own recorded rule, broken here: a port installed late is
     * a port that was not installed. It has cost this tree three bugs, and
     * reading the code for a different reason is how it was noticed. */
    nvme_plic_source = source;
    rc = irqwait_arm(&nvme_done, source, quiet_the_pin);
    if (rc != KERR_OK) {
        kprintf("nvme: interrupt %u could not be armed (rc %d); completions stay polled\n",
                source, rc);
        return rc;
    }
    /* **Left masked.** `submit` unmasks before each command, so there is no
     * window here in which the controller can raise into a kernel that is
     * not yet waiting for it. */
    nvme_irq_armed = true;
    kprintf("nvme: completion interrupt %u armed; the lock is free while a command runs\n",
            source);
    return KERR_OK;
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
    /* **A bus address, because the controller does the reading.** */
    cmd.prp1 = (uint64_t)mmu_virt_to_phys(buf);
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

/* Whether the drive takes deallocate (TRIM) at all: the controller's
 * optional-command bits. A drive that lacks it answers the command with an
 * error, so it is never sent to one. */
static void take_capabilities(const uint8_t *identify_page) {
    uint16_t oncs = (uint16_t)(identify_page[IDENTIFY_ONCS] |
                               (identify_page[IDENTIFY_ONCS + 1u] << 8));
    nvme.can_deallocate = (oncs & ONCS_DSM) != 0;
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
    nvme.dealloc_zeroes = (ns_page[IDENTIFY_DLFEAT] & 7u) == DLFEAT_READS_ZERO;
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
    /* Bit 0 contiguous, bit 1 interrupts enabled, vector 0 in the high half.
     * **The interrupt is requested here although nothing can handle it yet**:
     * it is masked at the controller (below, before this runs) and unmasked
     * by `nvme_use_interrupts` once a handler exists. Asking for it later
     * would mean deleting and recreating a working queue. */
    cmd.cdw11 = 1u | 2u;
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

/* One transfer.
 *
 * **A PRP entry addresses from its offset to the end of its page, and no
 * further.** That is the whole of the subtlety here, and it is easy to miss
 * because a page-aligned buffer hides it completely: `prp1` then covers a
 * full page and a 4 KiB request needs nothing else. Hand the same driver a
 * buffer eight bytes into a page — which is exactly what a kernel heap
 * returns, since it puts a header in front of what it hands out — and the
 * first entry covers 4088 bytes while the last eight go nowhere. The symptom
 * was precisely that: byte 4088 of a 4 KiB block read back as zero.
 *
 * So `prp2` is set whenever the transfer runs past the end of `prp1`'s page,
 * where it addresses the **next page**. Two entries cover any transfer up to
 * two pages from any offset, which is every request this kernel makes; a
 * longer one would need a PRP *list* in memory, and is refused rather than
 * silently truncated. */
/* The page list a transfer of more than two pages needs.
 *
 * A command describes its buffer as at most two addresses: the first page
 * (which may start part-way in) and a second. Anything longer puts the
 * remaining pages' addresses in a list, and `prp2` points at the list
 * instead. One 4 KiB page of list addresses 512 further pages, so 2 MiB in a
 * single command — against the 8 KiB a two-address command can carry.
 *
 * Page-aligned by construction for the same reason the identify buffer is:
 * the controller reads it through the same mechanism, and a list that
 * straddles a page boundary is a list whose far half the controller cannot
 * reach. One list is enough because one command is in flight at a time. */
static uint64_t prp_list[PAGE_BYTES / sizeof(uint64_t)] __attribute__((aligned(4096)));

/* Fills the list with the pages after the first, and returns what `prp2`
 * should be: a page address when there are exactly two, and the list's own
 * address when there are more. */
static uint64_t describe_buffer(uint64_t at, uint64_t bytes) {
    uint64_t first_page_has = PAGE_BYTES - (at & (PAGE_BYTES - 1u));
    uint64_t next = (at & ~(uint64_t)(PAGE_BYTES - 1u)) + PAGE_BYTES;
    uint64_t remaining, pages;
    if (bytes <= first_page_has) {
        return 0; /* it all fits in the first page */
    }
    remaining = bytes - first_page_has;
    pages = (remaining + PAGE_BYTES - 1u) / PAGE_BYTES;
    if (pages == 1u) {
        return next;
    }
    for (uint64_t i = 0; i < pages && i < sizeof prp_list / sizeof prp_list[0]; i++) {
        prp_list[i] = next + i * PAGE_BYTES;
    }
    /* The list itself is read by the controller, so it too is named by its
     * bus address rather than by where this kernel keeps it. */
    return (uint64_t)mmu_virt_to_phys(prp_list);
}

/* The largest transfer this driver will issue in one command, in bytes: what
 * one list page can describe, less the partial first page. */
#define MAX_TRANSFER_BYTES ((PAGE_BYTES / sizeof(uint64_t)) * PAGE_BYTES)

static int transfer(uint8_t opcode, uint64_t lba, uint32_t blocks, void *buf) {
    struct nvme_command cmd = {0};
    uint64_t at = (uint64_t)mmu_virt_to_phys(buf);
    uint64_t bytes = (uint64_t)blocks * nvme.block_size;
    if (bytes > MAX_TRANSFER_BYTES) {
        return KERR_INVAL; /* the caller splits it: one list page is the limit */
    }
    cmd.opcode = opcode;
    cmd.namespace_id = 1;
    cmd.prp1 = at;
    cmd.prp2 = describe_buffer(at, bytes);
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = blocks - 1u; /* zero-based, as the specification has it */
    return submit(&nvme.io, &cmd, NULL);
}

/* Splits a request into transfers this driver can describe, so a caller
 * never has to know the limit. */
static int transfer_all(uint8_t opcode, uint64_t lba, uint32_t blocks, void *buf) {
    uint32_t per = (uint32_t)(MAX_TRANSFER_BYTES / nvme.block_size);
    uint8_t *to = buf;
    while (blocks > 0) {
        uint32_t n = blocks < per ? blocks : per;
        int rc = transfer(opcode, lba, n, to);
        if (rc != KERR_OK) {
            return rc;
        }
        lba += n;
        blocks -= n;
        to += (size_t)n * nvme.block_size;
    }
    return KERR_OK;
}

static int nvme_read(void *self, uint64_t lba, uint32_t count, void *buf) {
    (void)self;
    if (!nvme.up || count == 0 || lba + count > nvme.blocks) {
        return KERR_INVAL;
    }
    return transfer_all(IO_READ, lba, count, buf);
}

static int nvme_write(void *self, uint64_t lba, uint32_t count, const void *buf) {
    (void)self;
    if (!nvme.up || count == 0 || lba + count > nvme.blocks) {
        return KERR_INVAL;
    }
    return transfer_all(IO_WRITE, lba, count, (void *)(uintptr_t)buf);
}

/* Nothing to flush: this driver writes through, with no volatile cache of
 * its own. The drive may have one, and flushing it is an NVMe command this
 * kernel will need when it starts caring about power loss. */
static int nvme_flush(void *self) {
    (void)self;
    return nvme.up ? KERR_OK : KERR_NODEV;
}

/* One Dataset Management range, as the controller reads it. */
struct dsm_range {
    uint32_t attributes;
    uint32_t blocks;
    uint64_t lba;
};

/* The range list, page-aligned for the reason `identify_buffer` is. One
 * range per command: a filesystem's free extents arrive one at a time. */
static struct dsm_range dsm_ranges[PAGE_BYTES / sizeof(struct dsm_range)]
    __attribute__((aligned(4096)));

static int deallocate(uint64_t lba, uint32_t blocks) {
    struct nvme_command cmd = {0};
    dsm_ranges[0] = (struct dsm_range){.attributes = 0, .blocks = blocks, .lba = lba};
    cmd.opcode = IO_DSM;
    cmd.namespace_id = 1;
    cmd.prp1 = (uint64_t)mmu_virt_to_phys(dsm_ranges);
    cmd.cdw10 = 0; /* one range, zero-based */
    cmd.cdw11 = DSM_DEALLOCATE;
    return submit(&nvme.io, &cmd, NULL);
}

static int nvme_discard(void *self, uint64_t lba, uint32_t count) {
    (void)self;
    if (!nvme.up || !nvme.can_deallocate || count == 0 || lba + count > nvme.blocks) {
        return KERR_INVAL;
    }
    return deallocate(lba, count);
}

/* What the drive said about TRIM, in the boot log: whether a filesystem's
 * trim will reach it is otherwise invisible. */
static void say_deallocate(void) {
    if (!nvme.can_deallocate) {
        kprintf("nvme: deallocate not supported\n");
        return;
    }
    kprintf("nvme: deallocate supported, a deallocated block reads back %s\n",
            nvme.dealloc_zeroes ? "as zeroes" : "undefined");
}

bool nvme_can_deallocate(void) {
    return nvme.up && nvme.can_deallocate;
}

bool nvme_deallocated_reads_zero(void) {
    return nvme.dealloc_zeroes;
}

/* Brings the controller up: reset, admin queues, enable, identify. */
static int start_controller(void) {
    uint64_t cap = (uint64_t)read_reg(NVME_CAP) | ((uint64_t)read_reg(NVME_CAP + 4u) << 32);
    int rc;
    nvme.doorbell_stride = 4u << ((cap >> 32) & 0xfu);
    /* **Masked before anything can raise.** The queues below ask for
     * completion interrupts, and the handler for them is installed much
     * later — the interrupt controller does not exist during device
     * bring-up. An unclaimed interrupt is one the PLIC re-raises for ever,
     * which is a machine that does nothing else, so the window between
     * "the queue will raise" and "something will answer" is closed here
     * rather than hoped about. `0xffffffff` masks all 32 vectors; only
     * vector 0 is ever used, and masking the rest costs one store. */
    write_reg(NVME_INTMS, 0xffffffffu);
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

/* The buffer an identify command writes into.
 *
 * **Page-aligned by construction, not by allocation.** A command's data
 * pointer is a PRP entry: a page address plus an offset, and identify
 * requires that offset to be zero. The heap returns 8-byte-aligned memory,
 * so a `kzalloc(4096)` here gave the controller a pointer into the middle of
 * a page and the command came back with an error — on real silicon. QEMU's
 * model accepts it, which is why the queues were allocated as pages and this
 * one buffer was not: the mistake was invisible for as long as there was no
 * drive to refuse it.
 *
 * Static rather than allocated because it is used twice during start-up and
 * never afterwards, and because an alignment that the type system states is
 * one a later edit cannot quietly lose. */
static uint8_t identify_buffer[4096] __attribute__((aligned(4096)));

/* What the drive is — model, optional commands, namespace 1's geometry —
 * and then the I/O queues it will be used through. */
static int identify_drive(uint8_t *page) {
    int rc;
    for (uint32_t i = 0; i < sizeof identify_buffer; i++) {
        identify_buffer[i] = 0;
    }
    rc = identify(1u, 0, page); /* the controller */
    if (rc == KERR_OK) {
        take_model(page);
        take_capabilities(page);
        rc = identify(0u, 1u, page); /* namespace 1 */
    }
    if (rc == KERR_OK) {
        take_geometry(page);
        rc = create_io_queues();
    }
    return rc;
}

int nvme_init(struct blockdev *out) {
    uint8_t *page = identify_buffer;
    int rc = pci_find_class(NVME_PCI_CLASS, NVME_PCI_CLASS_MASK, &nvme.pci);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The mapped address, not the bus address: `bar0` is what the device
     * decodes and `bar0_virt` is what this kernel can load from. */
    nvme.regs = (volatile uint32_t *)nvme.pci.bar0_virt;
    /* The controller's capability register, before anything is asked of it.
     * A drive that is mapped but not reachable reads all-ones or zeros here,
     * which separates "the bridge does not forward this address" from "the
     * controller refused to start" — two failures that otherwise look the
     * same from outside. */
    kprintf("nvme: bar0 %016llx, cap %08x%08x\n", (unsigned long long)nvme.pci.bar0,
            (unsigned)nvme.regs[1], (unsigned)nvme.regs[0]);
    rc = start_controller();
    if (rc != KERR_OK) {
        kprintf("nvme: the controller would not start (rc %d)\n", rc);
        return rc;
    }
    rc = identify_drive(page);
    if (rc != KERR_OK) {
        kprintf("nvme: identify or queue setup failed (rc %d)\n", rc);
        return rc;
    }
    nvme.up = true;
    say_deallocate();
    *out = (struct blockdev){.read = nvme_read, .write = nvme_write, .flush = nvme_flush,
                             .discard = nvme.can_deallocate ? nvme_discard : NULL,
                             .self = NULL, .block_size = nvme.block_size, .blocks = nvme.blocks,
                             .read_only = false, .discard_zeroes = nvme.dealloc_zeroes};
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
