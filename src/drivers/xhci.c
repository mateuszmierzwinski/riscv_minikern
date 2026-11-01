#include "drivers/xhci.h"

#include "arch/riscv/mmu.h"
#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/pci.h"
#include "kernel/mem.h"

#include <stddef.h>

/* The class code for a USB controller with the XHCI programming interface:
 * serial bus (0x0c), USB (0x03), XHCI (0x30). */
#define XHCI_PCI_CLASS 0x000c0330u
#define XHCI_PCI_CLASS_MASK 0x00ffffffu

/* ── capability registers, at the start of the window ──────────────────── */
#define CAP_CAPLENGTH 0x00u /* byte 0: where the operational registers begin */
#define CAP_HCIVERSION 0x00u /* bits 16-31 of the same word */
#define CAP_HCSPARAMS1 0x04u
#define CAP_HCSPARAMS2 0x08u /* scratchpad buffer count, bits 31:27 and 25:21 */
#define CAP_HCCPARAMS1 0x10u
#define CAP_DBOFF 0x14u   /* the doorbell array, from the window's start */
#define CAP_RTSOFF 0x18u  /* the runtime registers, likewise */

/* ── operational registers, at CAPLENGTH ───────────────────────────────── */
#define OP_USBCMD 0x00u
#define OP_USBSTS 0x04u
#define OP_PAGESIZE 0x08u
#define OP_DNCTRL 0x14u
#define OP_CRCR 0x18u
#define OP_DCBAAP 0x30u
#define OP_CONFIG 0x38u
/* Each port has four words, starting here. */
#define OP_PORTSC(n) (0x400u + ((n) - 1u) * 0x10u)

#define USBCMD_RS (1u << 0)    /* run */
#define USBCMD_HCRST (1u << 1) /* host controller reset */
#define USBCMD_INTE (1u << 2)

#define USBSTS_HCH (1u << 0)  /* halted */
#define USBSTS_CNR (1u << 11) /* controller not ready */

#define PORTSC_CCS (1u << 0) /* a device is connected */
#define PORTSC_PED (1u << 1) /* the port is enabled */
#define PORTSC_PR (1u << 4)  /* reset in progress */
#define PORTSC_PP (1u << 9)  /* port power */
#define PORTSC_CSC (1u << 17)
#define PORTSC_PRC (1u << 21)
/* **Write-1-to-clear bits in a read-modify-write register.** PORTSC mixes
 * ordinary control bits with change bits that clear when written as one, so
 * a plain read-modify-write of the whole word acknowledges changes the
 * driver never looked at. Every write masks them off. */
#define PORTSC_CHANGE_BITS 0x00fe0000u
/* **The bits a write may carry back**: power, the indicator and the wake
 * enables. PED is write-one-to-*clear* — writing a read value back with it
 * set disables the port — and the change bits are write-one-to-clear too.
 * Every write starts from `portsc_neutral`, never from the value read: the
 * reset's own acknowledgement wrote PED back and switched off the port it had
 * just enabled, which was "connected, speed 3, Polling, not enabled". */
#define PORTSC_PRESERVE ((1u << 9) | (3u << 14) | (7u << 25))

/* ── runtime registers ─────────────────────────────────────────────────── */
#define RT_IMAN 0x20u /* interrupter 0's management */
#define RT_IMOD 0x24u
#define RT_ERSTSZ 0x28u
#define RT_ERSTBA 0x30u
#define RT_ERDP 0x38u

/* **The context size, which is not a constant.** `HCCPARAMS1` bit 2 says
 * whether slot and endpoint contexts are 32 or 64 bytes, and a driver that
 * assumes the smaller one on a controller using the larger writes every
 * field of the endpoint context into the middle of the slot context. The
 * symptom is an Address Device command that fails with a context-parameter
 * error, which says nothing about why. */
#define HCCPARAMS1_CSZ (1u << 2)
/* Where the extended capabilities begin, in 32-bit words from the window's
 * start. This is how a controller says which of its ports are USB2 and which
 * are USB3 — a question that cannot be answered by looking at a port,
 * because a connector's two logical ports differ in how they must be reset
 * and a SuperSpeed port left in Polling looks exactly like a broken one. */
#define HCCPARAMS1_XECP(v) (((v) >> 16) & 0xffffu)
#define XECP_SUPPORTED_PROTOCOL 2u
/* A warm reset, which is bit 31 and is the one a USB3 port stuck in Polling
 * needs; the hot reset at bit 4 does not retrain a SuperSpeed link. */
#define PORTSC_WPR (1u << 31)

/* TRB types, from the specification's table. */
#define TRB_NORMAL 1u
#define TRB_SETUP 2u
#define TRB_DATA 3u
#define TRB_STATUS 4u
#define TRB_LINK 6u
#define TRB_ENABLE_SLOT 9u
#define TRB_ADDRESS_DEVICE 11u
#define TRB_EVENT_TRANSFER 32u
#define TRB_EVENT_COMMAND 33u
#define TRB_EVENT_PORT 34u

#define TRB_TYPE(c) (((c) >> 10) & 0x3fu)
#define TRB_SET_TYPE(t) ((uint32_t)(t) << 10)
#define TRB_CYCLE (1u << 0)
#define TRB_IOC (1u << 5)   /* interrupt on completion: without it, no event */
#define TRB_IDT (1u << 6)   /* immediate data: the setup bytes are in the TRB */
#define TRB_SLOT(id) ((uint32_t)(id) << 24)
#define COMPLETION_CODE(status) (((status) >> 24) & 0xffu)
#define COMPLETION_SUCCESS 1u

/* How long a command or transfer may take before the driver gives up. The
 * specification's own limit for a command is 5 seconds; a control transfer
 * to a device that is present is milliseconds. */
#define EVENT_TIMEOUT_NS 2000000000ull

/* How long the controller may take to come out of reset. The specification
 * allows 500 ms for `CNR` to clear; anything past that is a controller that
 * is not going to answer. */
#define READY_TIMEOUT_NS 1000000000ull
#define RESET_TIMEOUT_NS 1000000000ull
/* A port reset is specified as 50 ms of signalling; twice that is generous
 * and still bounded. */
#define PORT_RESET_TIMEOUT_NS 200000000ull
/* How long ports are given to settle after the controller starts running.
 * The specification's figure is 20 ms; ten times that costs a fifth of a
 * second once per boot and removes a whole class of "it works on the second
 * try" from the port path. */
#define PORT_SETTLE_NS 200000000ull

/* Ring sizes, in entries of 16 bytes. One page each, which is the unit this
 * kernel allocates in and far more than the few commands this driver issues. */
#define RING_ENTRIES 256u

/* A transfer, command or event block: four words, the fourth carrying the
 * type and the cycle bit. */
struct trb {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
};

/* An event-ring segment table entry. */
struct erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
};

static struct {
    struct pci_device pci;
    volatile uint8_t *cap;   /* the window's start */
    volatile uint8_t *op;    /* the operational registers */
    volatile uint8_t *rt;    /* the runtime registers */
    volatile uint32_t *db;   /* the doorbell array */
    uint32_t ports;
    uint32_t slots;
    uint32_t version;
    /* The device-context base address array: the controller reads a slot's
     * context pointer from here, so it is the controller's view of what
     * devices exist. */
    uint64_t *dcbaa;
    struct trb *command_ring;
    phys_t command_ring_phys;
    struct trb *event_ring;
    struct erst_entry *erst;
    uint32_t command_index;
    uint32_t event_index;
    bool command_cycle;
    bool event_cycle;
    bool running;
    /* 32 or 64, from `HCCPARAMS1`. */
    uint32_t context_bytes;
    phys_t event_ring_phys;
    /* The USB major version of each port, one-based, from the Supported
     * Protocol capability. 0 means the controller did not say. */
    uint8_t port_major[32];
    bool trace_ports;
} xhci;

static uint32_t rd32(volatile uint8_t *at, uint32_t off) {
    return *(volatile uint32_t *)(at + off);
}

static void wr32(volatile uint8_t *at, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(at + off) = v;
}

/* A 64-bit operational register, written as two 32-bit halves.
 *
 * **Low half last is not optional.** Several of these registers act on the
 * write to their low half — `CRCR` takes the ring pointer then, and `ERDP`
 * acknowledges events — so writing the low half first makes the controller
 * act on a pointer whose top 32 bits are still the previous value. */
/* **Low dword first, then high**, as the specification orders it. The VL805
 * commits a 64-bit register when its high half is written: written the other
 * way round, every ring pointer was committed as {old low = 0, high} —
 * address 0 — so the controller ran, took commands and posted every event
 * somewhere else. Control: the old order, same image, no completion. */
static void wr64(volatile uint8_t *at, uint32_t off, uint64_t v) {
    wr32(at, off, (uint32_t)v);
    wr32(at, off + 4u, (uint32_t)(v >> 32));
}

/* Defined with the ring machinery below; the port code needs it, because a
 * driver must service the event ring even when it is not waiting on it. */
static int next_event(struct trb *out);

/* ── bringing the controller up ─────────────────────────────────────────── */

/* One page of DMA memory, with both views kept.
 *
 * **The controller needs the bus address and this kernel needs the mapped
 * one**, and since the kernel moved to its own half of the address space
 * those are different numbers. Handing a kernel address to a device makes it
 * read memory nobody owns; `nvme.c` paid for this lesson already. */
static void *dma_page(phys_t *phys_out) {
    phys_t phys = 0;
    if (mem_alloc_pages(1, 1, &phys) != KERR_OK) {
        return NULL;
    }
    *phys_out = phys;
    /* **Zeroed**: the allocator promises nothing about a frame's contents,
     * and a ring or context with a stale cycle bit or pointer in it is one
     * the controller acts on. */
    for (uint32_t i = 0; i < 4096u / 8u; i++) {
        ((volatile uint64_t *)mmu_phys_to_virt(phys))[i] = 0;
    }
    return mmu_phys_to_virt(phys);
}

/* Waits for a status bit to clear. */
static int wait_until_clear(uint32_t off, uint32_t bit, uint64_t limit_ns) {
    uint64_t until = timer_ns() + limit_ns;
    while (timer_ns() < until) {
        if ((rd32(xhci.op, off) & bit) == 0) {
            return KERR_OK;
        }
    }
    return KERR_TIMEDOUT;
}

/* Resets the controller and waits for it to be ready.
 *
 * The order matters: the controller must be **halted** before it is reset,
 * because resetting a running controller is undefined and in practice
 * leaves it in a state where nothing afterwards completes. */
static int reset_the_controller(void) {
    int rc;
    wr32(xhci.op, OP_USBCMD, 0);
    rc = wait_until_clear(OP_USBSTS, 0u, 0u); /* a no-op read, to order the write */
    (void)rc;
    {
        uint64_t until = timer_ns() + RESET_TIMEOUT_NS;
        while ((rd32(xhci.op, OP_USBSTS) & USBSTS_HCH) == 0 && timer_ns() < until) {
        }
        if ((rd32(xhci.op, OP_USBSTS) & USBSTS_HCH) == 0) {
            return KERR_TIMEDOUT; /* it would not halt */
        }
    }
    wr32(xhci.op, OP_USBCMD, USBCMD_HCRST);
    rc = wait_until_clear(OP_USBCMD, USBCMD_HCRST, RESET_TIMEOUT_NS);
    if (rc != KERR_OK) {
        return rc;
    }
    /* `CNR` is the controller saying "do not write my registers yet". A
     * driver that configured through this window would have its writes
     * discarded, and nothing would report it. */
    return wait_until_clear(OP_USBSTS, USBSTS_CNR, READY_TIMEOUT_NS);
}

/* **Scratchpad pages, when the controller asks for them** (HCSPARAMS2): its
 * own working memory, handed over through entry 0 of the context array. A
 * controller that wants them and is given none may run, report ports, and
 * then never complete a command — the first build gave none. */
static int give_the_scratchpads(void) {
    uint32_t p2 = rd32(xhci.cap, CAP_HCSPARAMS2);
    uint32_t count = (((p2 >> 21) & 0x1fu) << 5) | ((p2 >> 27) & 0x1fu);
    phys_t array_phys = 0;
    uint64_t *array;
    kprintf("xhci: %u scratchpad pages asked for\n", count);
    if (count == 0u) {
        return KERR_OK;
    }
    array = dma_page(&array_phys);
    if (array == NULL || count > 4096u / sizeof(uint64_t)) {
        return KERR_NOMEM;
    }
    for (uint32_t i = 0; i < count; i++) {
        phys_t page = 0;
        if (dma_page(&page) == NULL) {
            return KERR_NOMEM;
        }
        array[i] = page;
    }
    xhci.dcbaa[0] = array_phys;
    return KERR_OK;
}

/* The command ring, the event ring and the context array. */
static int build_the_rings(void) {
    phys_t dcbaa_phys = 0, cmd_phys = 0, event_phys = 0, erst_phys = 0;
    xhci.dcbaa = dma_page(&dcbaa_phys);
    xhci.command_ring = dma_page(&cmd_phys);
    xhci.event_ring = dma_page(&event_phys);
    xhci.erst = dma_page(&erst_phys);
    if (xhci.dcbaa == NULL || xhci.command_ring == NULL || xhci.event_ring == NULL ||
        xhci.erst == NULL) {
        return KERR_NOMEM;
    }
    /* How many slots the controller may use. It must be told, or it accepts
     * no device at all — the register defaults to zero. */
    wr32(xhci.op, OP_CONFIG, xhci.slots);
    if (give_the_scratchpads() != KERR_OK) {
        return KERR_NOMEM;
    }
    wr64(xhci.op, OP_DCBAAP, dcbaa_phys);
    /* The command ring's pointer, with the cycle bit: the controller and the
     * driver agree on whose turn an entry is by that bit, and starting them
     * disagreeing means the controller sees an empty ring for ever. */
    xhci.command_cycle = true;
    xhci.command_index = 0;
    xhci.command_ring_phys = cmd_phys;
    wr64(xhci.op, OP_CRCR, cmd_phys | 1u);
    /* One event-ring segment, described by the table the controller reads. */
    xhci.event_ring_phys = event_phys;
    xhci.erst[0].base = event_phys;
    xhci.erst[0].size = RING_ENTRIES;
    xhci.event_cycle = true;
    xhci.event_index = 0;
    /* **Fenced before the controller is told where any of it is.** The rings
     * were written by this hart and are read by a device; `fence w, w` is
     * the idiom `virtio.c` and `dwmac.c` use for exactly that, and without
     * it the controller may read a ring that is still in a store buffer. */
    __asm__ volatile("fence w, w" : : : "memory");
    wr32(xhci.rt, RT_ERSTSZ, 1u);
    /* **The dequeue pointer before the table base.** Writing the base makes
     * the controller read the table, so a dequeue pointer written afterwards
     * is a pointer it has already used the old value of. */
    wr64(xhci.rt, RT_ERDP, event_phys);
    wr64(xhci.rt, RT_ERSTBA, erst_phys);
    return KERR_OK;
}

/* Walks the extended capabilities and records which USB version each port
 * speaks.
 *
 * **Read rather than guessed.** The board shows one port connected, at
 * "speed 3", stuck in Polling — and whether that is a USB2 port that failed
 * to reset or a USB3 port whose link is training changes both the diagnosis
 * and the remedy. The controller publishes the answer, so it is read: each
 * Supported Protocol capability names a first port, a count and a version. */
static void learn_the_port_versions(void) {
    uint32_t at = HCCPARAMS1_XECP(rd32(xhci.cap, CAP_HCCPARAMS1)) * 4u;
    for (uint32_t guard = 0; guard < 64u && at != 0; guard++) {
        uint32_t word0 = rd32(xhci.cap, at);
        uint32_t id = word0 & 0xffu;
        uint32_t next = ((word0 >> 8) & 0xffu) * 4u;
        if (id == XECP_SUPPORTED_PROTOCOL) {
            uint32_t word2 = rd32(xhci.cap, at + 8u);
            uint32_t first = word2 & 0xffu;
            uint32_t count = (word2 >> 8) & 0xffu;
            uint32_t major = (word0 >> 24) & 0xffu;
            kprintf("xhci: ports %u..%u speak USB %u (%.4s)\n", first, first + count - 1u, major,
                    (const char *)&xhci.cap[at + 4u]);
            for (uint32_t n = first; n < first + count && n <= 32u; n++) {
                xhci.port_major[n - 1u] = (uint8_t)major;
            }
        }
        if (next == 0) {
            return;
        }
        at += next;
    }
}

int xhci_init(void) {
    uint32_t caplength, hcsparams1;
    int rc = pci_find_class(XHCI_PCI_CLASS, XHCI_PCI_CLASS_MASK, &xhci.pci);
    if (rc != KERR_OK) {
        return rc;
    }
    xhci.cap = xhci.pci.bar0_virt;
    if (xhci.cap == NULL) {
        return KERR_NOMEM;
    }
    /* The first word carries the capability-register length in its low byte
     * and the interface version above it. A window that is mapped but not
     * reachable reads all-ones here, which is not a plausible length. */
    caplength = rd32(xhci.cap, CAP_CAPLENGTH) & 0xffu;
    xhci.version = (rd32(xhci.cap, CAP_HCIVERSION) >> 16) & 0xffffu;
    if (caplength < 0x20u || caplength > 0x100u) {
        kprintf("xhci: implausible capability length %u — the window is wrong\n", caplength);
        return KERR_IO;
    }
    hcsparams1 = rd32(xhci.cap, CAP_HCSPARAMS1);
    xhci.context_bytes = (rd32(xhci.cap, CAP_HCCPARAMS1) & HCCPARAMS1_CSZ) != 0 ? 64u : 32u;
    xhci.slots = hcsparams1 & 0xffu;
    xhci.ports = (hcsparams1 >> 24) & 0xffu;
    xhci.op = xhci.cap + caplength;
    xhci.rt = xhci.cap + (rd32(xhci.cap, CAP_RTSOFF) & ~0x1fu);
    xhci.db = (volatile uint32_t *)(xhci.cap + (rd32(xhci.cap, CAP_DBOFF) & ~0x3u));
    learn_the_port_versions();
    rc = reset_the_controller();
    if (rc != KERR_OK) {
        kprintf("xhci: the controller would not reset (rc %d)\n", rc);
        return rc;
    }
    rc = build_the_rings();
    if (rc != KERR_OK) {
        return rc;
    }
    wr32(xhci.op, OP_USBCMD, USBCMD_RS);
    {
        /* Running means `HCH` clear. Checked rather than assumed, because
         * everything after this waits on a controller that may never have
         * started. */
        uint64_t until = timer_ns() + READY_TIMEOUT_NS;
        while ((rd32(xhci.op, OP_USBSTS) & USBSTS_HCH) != 0 && timer_ns() < until) {
        }
        xhci.running = (rd32(xhci.op, OP_USBSTS) & USBSTS_HCH) == 0;
    }
    if (xhci.running) {
        /* **Ports are not ready the instant the controller runs.** The
         * specification allows them time to settle after `USBCMD.RS`, and
         * resetting one immediately is how a connected high-speed device
         * ends up acknowledged, reset, and still not enabled — which is
         * what the board showed before this wait existed. */
        uint64_t until = timer_ns() + PORT_SETTLE_NS;
        while (timer_ns() < until) {
        }
    }
    return xhci.running ? KERR_OK : KERR_TIMEDOUT;
}

void xhci_describe(uint32_t *version, uint32_t *ports, uint32_t *slots) {
    *version = xhci.version;
    *ports = xhci.ports;
    *slots = xhci.slots;
}

uint32_t xhci_context_bytes(void) {
    return xhci.context_bytes;
}

void xhci_trace_ports(bool on) {
    xhci.trace_ports = on;
}

uint8_t xhci_port_major(uint32_t n) {
    return n > 0 && n <= 32u ? xhci.port_major[n - 1u] : 0;
}


bool xhci_running(void) {
    return xhci.running;
}

uint32_t xhci_port_status(uint32_t n) {
    if (!xhci.running || n == 0 || n > xhci.ports) {
        return 0;
    }
    return rd32(xhci.op, OP_PORTSC(n));
}

/* A port's register value with nothing in it a write would act on. */
static uint32_t portsc_neutral(uint32_t sc) {
    return sc & PORTSC_PRESERVE;
}

/* Acknowledges the change bits that are set, and only those.
 *
 * **This is the step the first version left out entirely.** Every write in
 * that version masked the change bits off — which is right for a write whose
 * purpose is something else, since they are write-one-to-clear and a
 * read-modify-write would acknowledge changes the driver never looked at.
 * But it means nothing ever acknowledged them, and a port with its
 * connect-status change still pending does not complete a reset: the board
 * showed a high-speed device connected, powered, and stuck in Polling with
 * the change bit set. */
static void acknowledge_changes(uint32_t n) {
    uint32_t sc = rd32(xhci.op, OP_PORTSC(n));
    uint32_t pending = sc & PORTSC_CHANGE_BITS;
    if (pending != 0) {
        wr32(xhci.op, OP_PORTSC(n), portsc_neutral(sc) | pending);
    }
}

/* Powers a port, resets it, and says whether a device ended up enabled.
 *
 * The order is the specification's and each step matters: power, because an
 * unpowered port reports nothing attached whatever is in it; acknowledge,
 * because a pending change blocks the reset; reset; wait for the reset's own
 * change bit; acknowledge that one alone; and only then read whether the
 * port is enabled. */
static bool reset_one_port_once(uint32_t n) {
    uint32_t sc = rd32(xhci.op, OP_PORTSC(n));
    uint64_t until;
    if ((sc & PORTSC_PP) == 0) {
        wr32(xhci.op, OP_PORTSC(n), portsc_neutral(sc) | PORTSC_PP);
        for (volatile uint32_t i = 0; i < 200000u; i++) {
        }
    }
    acknowledge_changes(n);
    sc = rd32(xhci.op, OP_PORTSC(n));
    if ((sc & PORTSC_CCS) == 0) {
        return false;
    }
    if (xhci.trace_ports) {
        kprintf("xhci: port %u before the reset: %08x\n", n, sc);
    }
    /* **A warm reset for a SuperSpeed port, a hot one otherwise.** Bit 4
     * does not retrain a USB3 link, so a port whose device is connected and
     * whose link is Polling stays there for ever under it — which is exactly
     * what the board showed. */
    wr32(xhci.op, OP_PORTSC(n),
         portsc_neutral(sc) |
             (xhci_port_major(n) >= 3u && ((sc >> 5) & 0xfu) == 7u ? PORTSC_WPR : PORTSC_PR));
    if (xhci.trace_ports) {
        /* **Read straight back.** "The reset was requested" and "the reset
         * bit is set" are different claims, and three board runs have now
         * been spent on the first without ever checking the second. */
        kprintf("xhci: port %u immediately after writing the reset: %08x\n", n,
                rd32(xhci.op, OP_PORTSC(n)));
    }
    until = timer_ns() + PORT_RESET_TIMEOUT_NS;
    while (timer_ns() < until) {
        sc = rd32(xhci.op, OP_PORTSC(n));
        if ((sc & PORTSC_PR) != 0) {
            continue; /* still resetting */
        }
        if ((sc & PORTSC_PRC) != 0) {
            /* The reset's change bit alone, so the next read is about now. */
            wr32(xhci.op, OP_PORTSC(n), portsc_neutral(sc) | PORTSC_PRC);
        }
        /* **Waits for `PED` itself, not for the change bit.** The change bit
         * says the reset finished; it does not say the port came up, and a
         * driver that returned on it read `PED` a moment too early. */
        if ((rd32(xhci.op, OP_PORTSC(n)) & PORTSC_PED) != 0) {
            return true;
        }
    }
    return (rd32(xhci.op, OP_PORTSC(n)) & PORTSC_PED) != 0;
}

/* **Tried more than once.** A port that does not come up on the first reset
 * often does on the second, and a driver that gave up after one would report
 * hardware as broken because it was impatient. Three is enough that a real
 * failure is a real failure. */
/* Takes whatever events are waiting and throws them away.
 *
 * **A driver must service the event ring even when it is not waiting for
 * anything.** The controller posts a Port Status Change event for every
 * transition, and it advances its own producer pointer; the dequeue pointer
 * is the driver's job. Leaving it stale means the controller eventually sees
 * a ring it cannot write to, and it stops reporting — so a port that would
 * have come up on the second attempt never reports that it did. */
static void drain_events(void) {
    struct trb ignored;
    for (uint32_t i = 0; i < 32u; i++) {
        volatile struct trb *e = &xhci.event_ring[xhci.event_index];
        if (((e->control & TRB_CYCLE) != 0) != xhci.event_cycle) {
            return; /* nothing more this lap */
        }
        if (next_event(&ignored) != KERR_OK) {
            return;
        }
    }
}

static bool reset_one_port(uint32_t n) {
    for (uint32_t attempt = 0; attempt < 3u; attempt++) {
        drain_events();
        if (reset_one_port_once(n)) {
            drain_events();
            return true;
        }
    }
    drain_events();
    return false;
}

/* The decoded fields, so the log states what the register says rather than a
 * conclusion drawn from it. The first version printed "has a device,
 * enabled" for a port whose `PED` was clear and for one with nothing
 * connected at all; both were visible in the hex it printed beside the
 * claim, which is the only reason it was caught. */
static void report_one_port(uint32_t n) {
    uint32_t sc = rd32(xhci.op, OP_PORTSC(n));
    static const char *const link[] = {"U0", "U1", "U2", "U3", "disabled", "rx-detect",
                                       "inactive", "polling", "recovery", "hot-reset",
                                       "compliance", "test"};
    uint32_t pls = (sc >> 5) & 0xfu;
    kprintf("xhci: port %u: %s, %s, speed %u, link %s, portsc %08x\n", n,
            (sc & PORTSC_CCS) != 0 ? "connected" : "empty",
            (sc & PORTSC_PED) != 0 ? "enabled" : "not enabled", (sc >> 10) & 0xfu,
            pls < 12u ? link[pls] : "?", sc);
}

uint32_t xhci_scan_ports(void) {
    uint32_t enabled = 0;
    if (!xhci.running) {
        return 0;
    }
    for (uint32_t n = 1; n <= xhci.ports; n++) {
        bool up = reset_one_port(n);
        if ((rd32(xhci.op, OP_PORTSC(n)) & PORTSC_CCS) != 0) {
            report_one_port(n);
        }
        enabled += up ? 1u : 0u;
    }
    return enabled;
}

/* ── the command ring and the event ring ───────────────────────────────── */

/* Rings the doorbell for a slot. Slot 0 is the command ring's, and any other
 * is a device's, with the endpoint in the low bits. */
static void ring_doorbell(uint32_t slot, uint32_t endpoint) {
    __asm__ volatile("fence w, w" : : : "memory");
    xhci.db[slot] = endpoint;
}

/* Takes the next event, or times out.
 *
 * **The cycle bit is what says an entry is the controller's work rather than
 * stale memory.** The ring is not emptied; the controller writes round it and
 * flips the meaning of the bit each lap, so a driver that looked for a
 * non-zero entry would re-read last lap's events for ever. */
static int next_event(struct trb *out) {
    uint64_t until = timer_ns() + EVENT_TIMEOUT_NS;
    while (timer_ns() < until) {
        volatile struct trb *e = &xhci.event_ring[xhci.event_index];
        uint32_t control = e->control;
        if (((control & TRB_CYCLE) != 0) != xhci.event_cycle) {
            continue; /* not yet written this lap */
        }
        out->parameter = e->parameter;
        out->status = e->status;
        out->control = control;
        xhci.event_index++;
        if (xhci.event_index == RING_ENTRIES) {
            xhci.event_index = 0;
            xhci.event_cycle = !xhci.event_cycle;
        }
        /* **The dequeue pointer, with the handler-busy bit set.** Telling
         * the controller where the driver has read to is what lets it reuse
         * the entries; leaving it stale makes the ring appear full and the
         * controller stops raising events entirely. */
        wr64(xhci.rt, RT_ERDP,
             (xhci.event_ring_phys + xhci.event_index * sizeof(struct trb)) | (1ull << 3));
        return KERR_OK;
    }
    return KERR_TIMEDOUT;
}

/* Waits for an event of `want`, discarding the port-status changes that
 * arrive unbidden while a port is being reset. */
static int await_event(uint32_t want, struct trb *out) {
    for (uint32_t tries = 0; tries < 16u; tries++) {
        int rc = next_event(out);
        if (rc != KERR_OK) {
            return rc;
        }
        if (TRB_TYPE(out->control) == want) {
            return KERR_OK;
        }
        /* Port-status events are expected here and are not the answer. */
    }
    return KERR_TIMEDOUT;
}

/* **What the controller says when a command gets no answer**: its status
 * (halted, host-system error), whether the command ring is running, and
 * where the rings are — numbers to reason from, where "timed out" is none. */
static void report_a_silent_command(uint32_t control) {
    kprintf("xhci: command type %u got no completion: usbsts %08x crcr %08x, "
            "ring %llx, events %llx, ac64 %u\n",
            TRB_TYPE(control), rd32(xhci.op, OP_USBSTS), rd32(xhci.op, OP_CRCR),
            (unsigned long long)xhci.command_ring_phys,
            (unsigned long long)xhci.event_ring_phys,
            rd32(xhci.cap, CAP_HCCPARAMS1) & 1u);
    kprintf("xhci: erstsz %u erstba %08x erdp %08x, reading index %u cycle %u; "
            "events[0..3] control %08x %08x %08x %08x\n",
            rd32(xhci.rt, RT_ERSTSZ), rd32(xhci.rt, RT_ERSTBA), rd32(xhci.rt, RT_ERDP),
            xhci.event_index, xhci.event_cycle ? 1u : 0u, xhci.event_ring[0].control,
            xhci.event_ring[1].control, xhci.event_ring[2].control, xhci.event_ring[3].control);
}

/* Puts one command on the ring and waits for its completion event. */
static int do_command(uint64_t parameter, uint32_t control, struct trb *event) {
    struct trb *slot_trb = &xhci.command_ring[xhci.command_index];
    int rc;
    slot_trb->parameter = parameter;
    slot_trb->status = 0;
    /* The cycle bit last, as with every descriptor a device reads: the
     * controller may act the moment it sees the bit, and the rest of the
     * entry has to be there already. */
    __asm__ volatile("fence w, w" : : : "memory");
    slot_trb->control = control | (xhci.command_cycle ? TRB_CYCLE : 0u);
    xhci.command_index++;
    if (xhci.command_index >= RING_ENTRIES - 1u) {
        /* **A link TRB back to the start, rather than wrapping the index.**
         * The controller follows the ring by reading entries, so the end of
         * the ring has to *say* where the ring continues. */
        struct trb *link = &xhci.command_ring[RING_ENTRIES - 1u];
        link->parameter = xhci.command_ring_phys; /* back to the start */
        link->status = 0;
        link->control = TRB_SET_TYPE(TRB_LINK) | (1u << 1) |
                        (xhci.command_cycle ? TRB_CYCLE : 0u);
        xhci.command_index = 0;
        xhci.command_cycle = !xhci.command_cycle;
    }
    ring_doorbell(0, 0);
    rc = await_event(TRB_EVENT_COMMAND, event);
    if (rc != KERR_OK) {
        report_a_silent_command(control);
        return rc;
    }
    if (COMPLETION_CODE(event->status) != COMPLETION_SUCCESS) {
        kprintf("xhci: a command failed, completion code %u\n", COMPLETION_CODE(event->status));
        return KERR_IO;
    }
    return KERR_OK;
}

/* ── enumerating one device ────────────────────────────────────────────── */

/* Where a field lives in a context, given that contexts are 32 or 64 bytes.
 * Every access goes through this rather than through a struct, because a
 * struct would have to be one size or the other and the controller decides. */
static volatile uint32_t *context_word(void *context, uint32_t index, uint32_t word) {
    uint8_t *at = (uint8_t *)context + index * xhci.context_bytes;
    return (volatile uint32_t *)(at + word * 4u);
}

/* The largest a control transfer's first packet may be, which depends on the
 * speed the port reports. Getting it wrong is not a performance matter: the
 * controller splits transfers by this number, and a wrong one makes the
 * device's first descriptor read return a short or corrupt packet. */
static uint32_t max_packet_for(uint32_t speed) {
    switch (speed) {
    case 4: return 512u; /* SuperSpeed */
    case 3: return 64u;  /* High-speed */
    case 2: return 8u;   /* Low-speed */
    default: return 64u; /* Full-speed may be 8, 16, 32 or 64; 64 is asked for
                          * and the device's own descriptor corrects it */
    }
}

struct enumerated {
    uint32_t slot;
    void *input_context;
    phys_t input_context_phys;
    struct trb *ep0_ring;
    phys_t ep0_ring_phys;
    uint32_t ep0_index;
    bool ep0_cycle;
};

/* Asks for a slot, builds its contexts and addresses the device. */
static int address_the_device(uint32_t port, uint32_t speed, struct enumerated *dev) {
    struct trb event;
    phys_t device_context_phys = 0;
    void *device_context;
    int rc = do_command(0, TRB_SET_TYPE(TRB_ENABLE_SLOT), &event);
    if (rc != KERR_OK) {
        return rc;
    }
    dev->slot = (event.control >> 24) & 0xffu;
    if (dev->slot == 0 || dev->slot > xhci.slots) {
        return KERR_IO;
    }
    device_context = dma_page(&device_context_phys);
    dev->input_context = dma_page(&dev->input_context_phys);
    dev->ep0_ring = dma_page(&dev->ep0_ring_phys);
    if (device_context == NULL || dev->input_context == NULL || dev->ep0_ring == NULL) {
        return KERR_NOMEM;
    }
    /* The controller finds a slot's context through this array, so it is
     * what makes the slot exist as far as the hardware is concerned. */
    xhci.dcbaa[dev->slot] = device_context_phys;
    /* The input context: an control block, then the slot context, then
     * endpoint 0's. Index 0 is the control block, 1 the slot, 2 endpoint 0. */
    *context_word(dev->input_context, 0, 1) = 0x3u; /* add the slot and ep0 */
    /* Route string zero (a root-hub port), the speed, and one context
     * entry — the count is the *highest* endpoint index in use, and a zero
     * here means the controller reads no endpoint at all. */
    *context_word(dev->input_context, 1, 0) = (speed << 20) | (1u << 27);
    *context_word(dev->input_context, 1, 1) = port << 16;
    /* Endpoint 0: a control endpoint, three retries, and its ring. */
    *context_word(dev->input_context, 2, 1) =
        (3u << 1) | (4u << 3) | (max_packet_for(speed) << 16);
    *context_word(dev->input_context, 2, 2) = (uint32_t)(dev->ep0_ring_phys) | 1u;
    *context_word(dev->input_context, 2, 3) = (uint32_t)(dev->ep0_ring_phys >> 32);
    *context_word(dev->input_context, 2, 4) = 8u; /* average TRB length */
    dev->ep0_cycle = true;
    dev->ep0_index = 0;
    __asm__ volatile("fence w, w" : : : "memory");
    return do_command(dev->input_context_phys,
                      TRB_SET_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(dev->slot), &event);
}

/* One control-transfer request: the setup packet's fields. */
struct control_request {
    uint8_t type; /* bmRequestType: direction, kind, recipient */
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

/* Puts one TRB on endpoint 0's ring, wrapping through a Link TRB at the end
 * (the ring would otherwise run off its page after ~85 transfers). */
static void ep0_put(struct enumerated *dev, uint64_t parameter, uint32_t status,
                    uint32_t control) {
    struct trb *ring = dev->ep0_ring;
    if (dev->ep0_index == RING_ENTRIES - 1u) {
        ring[dev->ep0_index].parameter = dev->ep0_ring_phys;
        ring[dev->ep0_index].status = 0;
        ring[dev->ep0_index].control =
            TRB_SET_TYPE(TRB_LINK) | (1u << 1) | (dev->ep0_cycle ? TRB_CYCLE : 0u);
        dev->ep0_index = 0;
        dev->ep0_cycle = !dev->ep0_cycle;
    }
    ring[dev->ep0_index].parameter = parameter;
    ring[dev->ep0_index].status = status;
    ring[dev->ep0_index].control = control | (dev->ep0_cycle ? TRB_CYCLE : 0u);
    dev->ep0_index++;
}

/* One control transfer: setup, an optional data stage, and the status
 * stage — the protocol, not an optimisation: a transfer missing its status
 * stage leaves the endpoint halted for everything after it. The data stage's
 * direction is the request type's top bit; the status stage goes the other
 * way, or in when there is no data. */
static int control(struct enumerated *dev, const struct control_request *r, phys_t buffer_phys) {
    struct trb event;
    bool in = (r->type & 0x80u) != 0;
    uint32_t trt = r->length == 0 ? 0u : (in ? 3u : 2u);
    uint64_t setup = (uint64_t)r->type | ((uint64_t)r->request << 8) |
                     ((uint64_t)r->value << 16) | ((uint64_t)r->index << 32) |
                     ((uint64_t)r->length << 48);
    ep0_put(dev, setup, 8u, TRB_SET_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
    if (r->length != 0) {
        ep0_put(dev, buffer_phys, r->length, TRB_SET_TYPE(TRB_DATA) | (in ? (1u << 16) : 0u));
    }
    ep0_put(dev, 0, 0, TRB_SET_TYPE(TRB_STATUS) | TRB_IOC | (in && r->length != 0 ? 0u : (1u << 16)));
    __asm__ volatile("fence w, w" : : : "memory");
    /* Endpoint 0's doorbell is 1: 0 belongs to the command ring. */
    ring_doorbell(dev->slot, 1u);
    if (await_event(TRB_EVENT_TRANSFER, &event) != KERR_OK) {
        return KERR_TIMEDOUT;
    }
    if (COMPLETION_CODE(event.status) != COMPLETION_SUCCESS) {
        kprintf("xhci: control request %02x/%02x failed, completion code %u\n", r->type,
                r->request, COMPLETION_CODE(event.status));
        return KERR_IO;
    }
    return KERR_OK;
}

/* The device the first enumeration found, kept for what comes after the
 * descriptor (a hub's ports), with a DMA page for its transfers. */
static struct enumerated first;
static uint8_t *first_buffer;
static phys_t first_buffer_phys;

int xhci_enumerate_first_port(struct xhci_device *out) {
    struct enumerated dev = {0};
    phys_t buffer_phys = 0;
    uint8_t *buffer;
    int rc;
    if (!xhci.running) {
        return KERR_NODEV;
    }
    for (uint32_t n = 1; n <= xhci.ports; n++) {
        uint32_t sc = rd32(xhci.op, OP_PORTSC(n));
        uint32_t speed = (sc >> 10) & 0xfu;
        if ((sc & PORTSC_CCS) == 0 || (sc & PORTSC_PED) == 0) {
            continue; /* nothing attached, or it did not enable */
        }
        buffer = dma_page(&buffer_phys);
        if (buffer == NULL) {
            return KERR_NOMEM;
        }
        rc = address_the_device(n, speed, &dev);
        if (rc != KERR_OK) {
            kprintf("xhci: port %u would not address (rc %d)\n", n, rc);
            continue;
        }
        /* The device descriptor: request 6, descriptor type 1, 18 bytes. */
        {
            static const struct control_request device_descriptor = {0x80u, 6u, 0x0100u, 0, 18u};
            rc = control(&dev, &device_descriptor, buffer_phys);
        }
        if (rc != KERR_OK) {
            continue;
        }
        out->port = n;
        out->slot = dev.slot;
        out->speed = speed;
        out->vendor = (uint16_t)(buffer[8] | (buffer[9] << 8));
        out->product = (uint16_t)(buffer[10] | (buffer[11] << 8));
        out->device_class = buffer[4];
        out->max_packet0 = buffer[7];
        first = dev;
        first_buffer = buffer;
        first_buffer_phys = buffer_phys;
        return KERR_OK;
    }
    return KERR_NOENT;
}

/* ── a hub on a root port ──────────────────────────────────────────────── */

#define USB_CLASS_HUB 0x09u
#define HUB_PORT_POWER 8u /* the port feature selector */

/* Picks the first configuration: what a hub (or anything) needs before its
 * class requests mean anything. */
static int configure_first(void) {
    static const struct control_request config_descriptor = {0x80u, 6u, 0x0200u, 0, 9u};
    struct control_request set = {0x00u, 9u, 0, 0, 0};
    int rc = control(&first, &config_descriptor, first_buffer_phys);
    if (rc != KERR_OK) {
        return rc;
    }
    set.value = first_buffer[5]; /* bConfigurationValue */
    return control(&first, &set, 0);
}

/* Powers every port, then waits the hub's own power-on time (its descriptor
 * says it in 2 ms units) and the 100 ms a connection takes to debounce. */
static int power_the_ports(uint32_t ports, uint32_t power_good_ms) {
    for (uint32_t p = 1; p <= ports; p++) {
        struct control_request on = {0x23u, 3u, HUB_PORT_POWER, (uint16_t)p, 0};
        int rc = control(&first, &on, 0);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    {
        uint64_t until = timer_ns() + ((uint64_t)power_good_ms + 100u) * 1000000u;
        while (timer_ns() < until) {
        }
    }
    return KERR_OK;
}

int xhci_hub_ports(uint32_t *status, uint32_t max) {
    static const struct control_request hub_descriptor = {0xa0u, 6u, 0x2900u, 0, 9u};
    uint32_t ports;
    int rc;
    if (first.slot == 0) {
        return KERR_NODEV;
    }
    rc = configure_first();
    rc = rc != KERR_OK ? rc : control(&first, &hub_descriptor, first_buffer_phys);
    if (rc != KERR_OK) {
        return rc;
    }
    ports = first_buffer[2];
    rc = power_the_ports(ports, 2u * first_buffer[5]);
    for (uint32_t p = 1; rc == KERR_OK && p <= ports && p <= max; p++) {
        struct control_request get = {0xa3u, 0u, 0, (uint16_t)p, 4u};
        rc = control(&first, &get, first_buffer_phys);
        status[p - 1u] = first_buffer[0] | (first_buffer[1] << 8) | (first_buffer[2] << 16) |
                         ((uint32_t)first_buffer[3] << 24);
    }
    return rc != KERR_OK ? rc : (int)ports;
}
