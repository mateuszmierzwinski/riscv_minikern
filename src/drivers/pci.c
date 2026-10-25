#include "drivers/pci.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"

/* ECAM: bus, device and function index a 4 KiB window each. */
#define ECAM_FUNCTION_SHIFT 12u
#define ECAM_SLOT_SHIFT 15u
#define ECAM_BUS_SHIFT 20u

#define PCI_NO_VENDOR 0xffffu
#define PCI_HEADER_BRIDGE 0x01u
#define PCI_HEADER_MULTIFUNCTION 0x80u

#define MAX_BUSES 4u    /* this kernel does not walk behind bridges yet */
#define MAX_SLOTS 32u
#define MAX_FUNCTIONS 8u

/* A BAR's low bits describe it rather than address it. */
#define BAR_IO 0x1u
#define BAR_TYPE_64 0x4u
#define BAR_MEM_MASK 0xfffffff0u

static struct {
    volatile uint8_t *ecam;
    phys_t ecam_base;
    uint64_t ecam_size;
    /* The 32-bit memory window the bridge publishes, and how much of it has
     * been handed out. A bump allocator: nothing here ever releases a BAR,
     * because a device this kernel has configured stays configured. */
    phys_t mem_base;
    uint64_t mem_size;
    uint64_t mem_used;
    uint32_t functions;
    bool up;
} pci;

static volatile uint8_t *config_of(uint8_t bus, uint8_t slot, uint8_t function) {
    return pci.ecam + ((uint64_t)bus << ECAM_BUS_SHIFT) + ((uint64_t)slot << ECAM_SLOT_SHIFT) +
           ((uint64_t)function << ECAM_FUNCTION_SHIFT);
}

static uint32_t read32(uint8_t bus, uint8_t slot, uint8_t fn, uint32_t off) {
    return *(volatile uint32_t *)(config_of(bus, slot, fn) + off);
}

static void write32(uint8_t bus, uint8_t slot, uint8_t fn, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(config_of(bus, slot, fn) + off) = v;
}

uint32_t pci_config_read32(const struct pci_device *d, uint32_t offset) {
    return read32(d->bus, d->slot, d->function, offset);
}

void pci_config_write32(const struct pci_device *d, uint32_t offset, uint32_t value) {
    write32(d->bus, d->slot, d->function, offset, value);
}

/* The bridge's `ranges` is a list of (child address, parent address, size)
 * entries, each tagged by the top cell: 0x02000000 is the 32-bit memory
 * window, which is the one BARs are assigned from here. 64-bit windows
 * exist too and are not used yet — a device needing one would have to be
 * given an address above 4 GiB, which works but has nothing asking for it. */
static int read_memory_window(const struct fdt *fdt, const char *path) {
    struct fdt_prop ranges;
    int rc = fdt_get_prop(fdt, path, "ranges", &ranges);
    uint32_t entries;
    if (rc != KERR_OK) {
        return rc;
    }
    entries = ranges.len / (7u * 4u); /* 3 child + 2 parent + 2 size cells */
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t tag = 0, parent = 0, size = 0;
        if (fdt_prop_cells(&ranges, i * 7u, 1, &tag) != KERR_OK ||
            fdt_prop_cells(&ranges, i * 7u + 3u, 2, &parent) != KERR_OK ||
            fdt_prop_cells(&ranges, i * 7u + 5u, 2, &size) != KERR_OK) {
            return KERR_INVAL;
        }
        /* **Only the space code identifies the window.** The top byte of a
         * `ranges` tag carries three independent flags above the two-bit
         * space code — non-relocatable, prefetchable, aliased — and this
         * compared the whole byte against `0x02`, the value QEMU happens to
         * emit. A VisionFive 2 emits `0x82` for its 32-bit window and `0xc3`
         * for its 64-bit one, so both were rejected and the board appeared
         * to have no host bridge at all.
         *
         * 0b10 is a 32-bit memory window and 0b11 a 64-bit one; either will
         * do for a BAR, and the 32-bit one is preferred because a 32-bit
         * BAR cannot be placed above 4 GiB. */
        uint32_t space = (uint32_t)((tag >> 24) & 0x03u);
        if (space == 0x02u) {
            pci.mem_base = parent;
            pci.mem_size = size;
            return KERR_OK;
        }
        if (space == 0x03u && pci.mem_size == 0) {
            pci.mem_base = parent; /* a fallback if there is no 32-bit window */
            pci.mem_size = size;
        }
    }
    return KERR_NOENT;
}

/* The host bridges this kernel can read config space through.
 *
 * `pci-host-ecam-generic` is the emulator's. **`starfive,jh7110-pcie` is a
 * PLDA XpressRich controller**, and this kernel uses it on one explicit
 * condition: the firmware has already brought it up. Its first `reg` is a
 * 16 MiB window addressed exactly as ECAM is — bus at bit 20, device at 15,
 * function at 12 — so once the clocks, the PHY and link training are done,
 * reading a device's configuration space is the same operation on both
 * bridges.
 *
 * That dependency is real and is stated rather than hidden: U-Boot prints
 * `Starfive PCIe bus probed` before handing over, and this kernel inherits a
 * trained link the way it inherits OpenSBI's trap handling. A cold boot into
 * this kernel with no firmware PCIe initialisation would need the
 * controller's own bring-up — six resets, four clocks and a syscon
 * handshake, per the node — which is not written. The honest consequence: no
 * device appears, rather than something subtly wrong.
 *
 * **Both are tried, and every instance of each.** This board has two PCIe
 * controllers, `pcie@940000000` and `pcie@9c0000000`, and the M.2 slot is on
 * one of them — the same lesson the two MMC controllers taught an hour
 * earlier (CLAUDE.md §5). */
static const char *const host_bridges[] = {"pci-host-ecam-generic", "starfive,jh7110-pcie"};

/* Maps one candidate bridge's config space and adopts it. */
/* **The bridge's own device-tree path, kept rather than discarded.**
 *
 * A PCIe device's interrupt does not belong to the device: `INTA`–`INTD` are
 * the bridge's pins, routed by its `interrupt-map`, and a legacy-pin device
 * raises the line the *bridge* owns. So the number a driver needs is on the
 * host-bridge node, and the only place that node's path is known is where
 * the bridge was adopted. Keeping it costs sixty-four bytes and is the
 * difference between a driver that can wait for its device and one that has
 * to spin. */
static char bridge_path[64];

const char *pci_bridge_path(void) {
    return bridge_path[0] == '\0' ? NULL : bridge_path;
}

/* ── the bridge's INTx demultiplexer ───────────────────────────────────── */

/* **Why a PCIe device's interrupt needs code here at all.**
 *
 * The JH7110's controller does not give each device a line. `INTA`-`INTD`
 * and MSI are aggregated into one PLIC source — 57, the single cell the node
 * declares — and the controller latches which of them fired in its own
 * status register. So the PLIC line stays asserted until *that* is cleared,
 * and a driver that acknowledges only its own device leaves the bridge
 * asserting: the trap re-enters for ever and the machine does nothing else.
 * Two boots were spent discovering that from the outside.
 *
 * **The offsets below are assumed, and that is the open question.** They are
 * the PLDA XpressRICH set as Linux's driver for the same IP uses them, and
 * with them the demultiplexer maps and the interrupt arms cleanly — but the
 * first command then hangs with the hart inside the trap handler, which is
 * what an uncleared latch looks like: `irqwait_wait` spins against a
 * deadline and cannot hang, so the hang is re-entry rather than waiting.
 * Either `ISTATUS_LOCAL` is not at `0x184` on this variant, or its bits are
 * not write-1-to-clear, or `INTA` is not at bit 24.
 *
 * This tree has guessed a register map wrong four times — the SD
 * controller, the GMAC generation, the watchdog and the temperature sensor —
 * and each time the answer came from dumping the window on the board rather
 * than from a document. That is the next step here, and it is why
 * `minikern.nvmeirq` stays off by default: the polled path is correct and
 * the root filesystem is on this drive.
 *
 * The registers are the PLDA XpressRICH set, at offsets within the window
 * the node calls `apb` — read by name rather than by index, because the
 * order of a `reg` list is not a promise. On this board that is
 * `0x2c000000`, and `cfg` is the ECAM window the rest of this file uses.
 *
 * **The order in the handler is the whole trick.** The device must be
 * quieted first and the latch cleared second: clearing a latch while the
 * device still asserts simply sets it again. That is why this is two
 * functions and not one, and why `nvme.c` calls them in that order. */
#define PLDA_IMASK_LOCAL 0x180u
#define PLDA_ISTATUS_LOCAL 0x184u
/* `INTA`-`INTD` occupy bits 24-27 of both registers. */
#define PLDA_INTX_SHIFT 24u
#define PLDA_INTX_MASK (0xfu << PLDA_INTX_SHIFT)

static volatile uint32_t *plda_apb;

/* Whether `n` bytes at `here` are exactly the string `want`. */
static bool same_name(const char *here, uint32_t n, const char *want) {
    uint32_t i = 0;
    while (i < n) {
        if (want[i] == '\0' || here[i] != want[i]) {
            return false;
        }
        i++;
    }
    return want[i] == '\0';
}

/* The index in `reg` of the window named `want`, or -1.
 *
 * `reg-names` is a run of NUL-terminated strings in the order the `reg`
 * entries appear, which is the only thing that ties a name to a window. */
static int reg_index_named(const struct fdt *fdt, const char *path, const char *want) {
    struct fdt_prop names;
    uint32_t at = 0;
    int which = 0;
    if (fdt_get_prop(fdt, path, "reg-names", &names) != KERR_OK) {
        return -1;
    }
    while (at < names.len) {
        const char *here = (const char *)names.value + at;
        uint32_t n = 0;
        while (at + n < names.len && here[n] != '\0') {
            n++;
        }
        if (same_name(here, n, want)) {
            return which;
        }
        at += n + 1u;
        which++;
    }
    return -1;
}

int pci_bridge_intx_init(const struct fdt *fdt) {
    const char *path = pci_bridge_path();
    phys_t base = 0;
    uint64_t size = 0;
    int which;
    if (path == NULL) {
        return KERR_NODEV;
    }
    if (plda_apb != NULL) {
        return KERR_OK;
    }
    which = reg_index_named(fdt, path, "apb");
    if (which < 0) {
        kprintf("pci: %s has no apb window; its INTx cannot be demultiplexed\n", path);
        return KERR_NOENT;
    }
    if (fdt_device_reg(fdt, path, (uint32_t)which, &base, &size) != KERR_OK ||
        size < PLDA_ISTATUS_LOCAL + 4u) {
        kprintf("pci: %s apb window is unusable\n", path);
        return KERR_INVAL;
    }
    /* **`mmu_map_device`, not `mmu_phys_to_virt`.** The apb window is at
     * `0x2c000000`, below DRAM: it is not in the direct map, so converting
     * it as if it were yields an address that faults on the first load. The
     * ECAM window twenty lines below has always been mapped this way; this
     * is the same kind of memory and wants the same treatment. The first
     * attempt used the direct map and the boot ended in a register dump. */
    plda_apb = (volatile uint32_t *)mmu_map_device(base, size);
    if (plda_apb == NULL) {
        kprintf("pci: %s apb window could not be mapped\n", path);
        return KERR_NOMEM;
    }
    /* **Cleared before unmasked.** Anything latched from before the kernel
     * was looking would otherwise arrive the moment the mask opens, as an
     * interrupt for a completion nobody is waiting for. */
    plda_apb[PLDA_ISTATUS_LOCAL / 4u] = PLDA_INTX_MASK;
    plda_apb[PLDA_IMASK_LOCAL / 4u] = plda_apb[PLDA_IMASK_LOCAL / 4u] | PLDA_INTX_MASK;
    kprintf("pci: %s INTx demultiplexer at %016llx\n", path, (unsigned long long)base);
    return KERR_OK;
}

uint32_t pci_bridge_intx_clear(void) {
    uint32_t status;
    if (plda_apb == NULL) {
        return 0;
    }
    status = plda_apb[PLDA_ISTATUS_LOCAL / 4u] & PLDA_INTX_MASK;
    if (status != 0) {
        plda_apb[PLDA_ISTATUS_LOCAL / 4u] = status;
    }
    return status >> PLDA_INTX_SHIFT;
}

static int adopt_bridge(const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    int rc = fdt_device_reg(fdt, path, 0, &base, &size);
    if (rc != KERR_OK) {
        kprintf("pci: %s has no readable reg (rc %d)\n", path, rc);
        return rc;
    }
    rc = read_memory_window(fdt, path);
    if (rc != KERR_OK) {
        kprintf("pci: %s: could not read its memory window (rc %d)\n", path, rc);
        return rc;
    }
    /* Only the buses this kernel walks are mapped: the whole ECAM region is
     * 256 MiB of address space for 256 buses, and mapping it all would be
     * 65536 pages of page tables for devices that are not there. */
    {
        void *at = mmu_map_device(base, (uint64_t)MAX_BUSES << ECAM_BUS_SHIFT);
        if (at == NULL) {
            return KERR_NOMEM;
        }
        pci.ecam = (volatile uint8_t *)at;
    }
    pci.ecam_base = base;
    pci.ecam_size = size;
    pci.up = true;
    return KERR_OK;
}

/* True when a bridge has anything behind it. A controller with no link reads
 * every vendor id as all-ones, and adopting that one would hide the bridge
 * that does have the drive on it. */
static bool bridge_has_a_device(void) {
    return read32(0, 0, 0, PCI_VENDOR_ID) != 0xffffffffu &&
           (read32(0, 0, 0, PCI_VENDOR_ID) & 0xffffu) != PCI_NO_VENDOR;
}

/* **Every** host bridge, not the first one with something behind it.
 *
 * A VisionFive 2 has two PCIe controllers. The USB 3 controller sits behind
 * one of them and the M.2 slot behind the other, so a kernel that adopted
 * the first bridge that answered found a USB controller, concluded there was
 * no NVMe drive, and fell back to the SD card — with a drive fitted and
 * working. It is the two-MMC-controllers lesson for the third time in one
 * evening (CLAUDE.md §5), which is how often a board has two of something.
 *
 * Each is kept with its own window, and a search walks them in turn. */
static struct host_bridge {
    phys_t ecam_base;
    uint64_t ecam_size;
    phys_t mem_base;
    uint64_t mem_size;
} bridges[4];
static uint32_t bridge_count;

/* Points the config-space accessors at one of the bridges found. */
static bool select_bridge(uint32_t i) {
    /* **Remapped rather than cast**, and so able to fail — which is why
     * this returns something now. The stored base is what the bridge
     * decodes; reaching it needs a mapping of its own, and an earlier call
     * for the same window is reused rather than mapped twice. */
    pci.ecam = mmu_map_device(bridges[i].ecam_base, (uint64_t)MAX_BUSES << ECAM_BUS_SHIFT);
    if (pci.ecam == NULL) {
        return false;
    }
    pci.ecam_base = bridges[i].ecam_base;
    pci.ecam_size = bridges[i].ecam_size;
    pci.mem_base = bridges[i].mem_base;
    pci.mem_size = bridges[i].mem_size;
    pci.mem_used = 0;
    pci.up = true;
    return true;
}

int pci_init(const struct fdt *fdt) {
    bridge_count = 0;
    for (uint32_t which = 0; which < sizeof host_bridges / sizeof host_bridges[0]; which++) {
        for (uint32_t skip = 0; skip < 4u; skip++) {
            char path[64];
            if (fdt_find_compatible_nth(fdt, host_bridges[which], skip, path, sizeof path) !=
                KERR_OK) {
                break;
            }
            if (adopt_bridge(fdt, path) != KERR_OK ||
                bridge_count >= sizeof bridges / sizeof bridges[0]) {
                continue;
            }
            if (!bridge_has_a_device()) {
                kprintf("pci: %s: nothing behind it\n", path);
                pci.up = false;
                continue;
            }
            bridges[bridge_count++] = (struct host_bridge){.ecam_base = pci.ecam_base,
                                                           .ecam_size = pci.ecam_size,
                                                           .mem_base = pci.mem_base,
                                                           .mem_size = pci.mem_size};
            {
                uint32_t n = 0;
                while (n + 1u < sizeof bridge_path && path[n] != '\0') {
                    bridge_path[n] = path[n];
                    n++;
                }
                bridge_path[n] = '\0';
            }
            kprintf("pci: %s at %016llx\n", path, (unsigned long long)pci.ecam_base);
        }
    }
    if (bridge_count == 0) {
        kprintf("pci: no usable host bridge\n");
        return KERR_NOENT;
    }
    (void)select_bridge(0);
    return KERR_OK;
}

/* Gives BAR0 an address out of the bridge's window.
 *
 * The size is discovered the way the specification prescribes: write all
 * ones, read back, and the bits that stayed zero are the ones the device
 * does not decode — so the complement plus one is the size. Then the base is
 * written, aligned to that size, because a BAR decodes only addresses
 * aligned to its own span. */
static int assign_bar0(struct pci_device *d) {
    uint32_t original = pci_config_read32(d, PCI_BAR0);
    uint32_t probe, mask;
    uint64_t size, at;
    if ((original & BAR_IO) != 0) {
        return KERR_NOTSUP; /* an I/O BAR: nothing here needs one */
    }
    /* **A BAR the firmware already placed is left where it is.**
     *
     * On a board whose firmware enumerated the bus — which is every board
     * that reaches this kernel through U-Boot — the root port's windows are
     * programmed to forward exactly the addresses it handed out. Choosing a
     * different address out of the bridge's `ranges` produces a BAR the
     * bridge does not forward: the device answers nothing, and the first
     * read of its registers hangs rather than failing, because a PCIe read
     * with no completer is a transaction that never completes.
     *
     * So the existing assignment is adopted and only an unprogrammed BAR is
     * given an address. An emulator that hands over a bare bus still takes
     * the second path, which is why both exist. */
    if ((original & BAR_MEM_MASK) != 0) {
        uint64_t existing = original & BAR_MEM_MASK;
        if ((original & BAR_TYPE_64) != 0) {
            existing |= (uint64_t)pci_config_read32(d, PCI_BAR0 + 4u) << 32;
        }
        pci_config_write32(d, PCI_BAR0, 0xffffffffu);
        probe = pci_config_read32(d, PCI_BAR0);
        pci_config_write32(d, PCI_BAR0, original);
        mask = probe & BAR_MEM_MASK;
        if (mask == 0) {
            return KERR_NOTSUP;
        }
        d->bar0 = existing;
        d->bar0_size = (uint64_t)(~mask) + 1u;
        kprintf("pci: %02x:%02x.%u keeping its BAR0 at %016llx, %llu KiB\n", d->bus, d->slot,
                d->function, (unsigned long long)d->bar0,
                (unsigned long long)(d->bar0_size / 1024u));
        d->bar0_virt = mmu_map_device(d->bar0, d->bar0_size);
        return d->bar0_virt == NULL ? KERR_NOMEM : KERR_OK;
    }
    pci_config_write32(d, PCI_BAR0, 0xffffffffu);
    probe = pci_config_read32(d, PCI_BAR0);
    pci_config_write32(d, PCI_BAR0, original);
    mask = probe & BAR_MEM_MASK;
    if (mask == 0) {
        return KERR_NOTSUP; /* the device decodes nothing here */
    }
    size = (uint64_t)(~mask) + 1u;
    at = (pci.mem_base + pci.mem_used + size - 1u) & ~(size - 1u);
    if (at + size > pci.mem_base + pci.mem_size) {
        return KERR_NOMEM;
    }
    pci_config_write32(d, PCI_BAR0, (uint32_t)at | (original & 0xfu));
    if ((original & BAR_TYPE_64) != 0) {
        pci_config_write32(d, PCI_BAR0 + 4u, (uint32_t)(at >> 32));
    }
    pci.mem_used = at + size - pci.mem_base;
    d->bar0 = at;
    d->bar0_size = size;
    d->bar0_virt = mmu_map_device(at, size);
    return d->bar0_virt == NULL ? KERR_NOMEM : KERR_OK;
}

/* Lets the device decode memory **and master the bus**. The second is the
 * one that is easy to forget and impossible to diagnose: a controller
 * without it initialises perfectly, accepts commands, and never completes
 * one, because it cannot read the queues the driver built for it. */
static void enable_device(struct pci_device *d) {
    uint32_t cmd = pci_config_read32(d, PCI_COMMAND);
    pci_config_write32(d, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

/* The buses this kernel looks at: bus 0, and whatever a bridge found on a
 * bus already being scanned says lies behind it.
 *
 * **Not a sweep of bus numbers.** A configuration read addressed to a bus
 * the root complex does not route is a transaction with no completer: it
 * does not return all-ones and it does not fail — it never completes, and
 * the kernel stops there with no output and nothing to read. QEMU's host
 * bridge answers for every bus number, so sweeping 0..3 was correct against
 * the emulator and hung on the first real board, immediately after the
 * bridge was adopted.
 *
 * The bus numbers a machine actually has are the ones its own bridges name,
 * in their secondary-bus register. */
static uint8_t buses[MAX_BUSES];
static uint32_t bus_count;

static void note_bus(uint8_t bus) {
    for (uint32_t i = 0; i < bus_count; i++) {
        if (buses[i] == bus) {
            return;
        }
    }
    if (bus_count < MAX_BUSES) {
        buses[bus_count++] = bus;
    }
}

static bool matches_class(uint32_t class_revision, uint32_t wanted, uint32_t mask) {
    return ((class_revision >> 8) & mask) == (wanted & mask);
}

/* Nothing in this slot.
 *
 * **Absent reads back as zero here, not as all-ones.** The specification's
 * convention — and QEMU's behaviour — is that a configuration read for a
 * function that is not there returns all-ones, and this kernel tested only
 * for that. The VisionFive 2's root complex returns **zero** instead, so an
 * empty slot presented itself as vendor 0000, device 0000, class 000000;
 * the kernel believed it, went on to read the rest of that function's
 * configuration space, and hung on a transaction with no completer.
 *
 * Vendor id 0 is not a valid vendor, so both readings mean the same thing
 * and both are checked. */
static bool no_function_at(uint32_t id) {
    return id == 0u || id == 0xffffffffu || (id & 0xffffu) == PCI_NO_VENDOR ||
           (id & 0xffffu) == 0u;
}

/* Reads one function, notes a bridge's far side, and says whether it is the
 * device being looked for. */
static bool look_at(uint8_t bus, uint8_t slot, uint8_t fn, uint32_t class_code, uint32_t mask,
                    struct pci_device *out) {
    uint32_t id = read32(bus, slot, fn, PCI_VENDOR_ID);
    uint32_t header;
    struct pci_device d;
    if (no_function_at(id)) {
        return false;
    }
    pci.functions++;
    header = read32(bus, slot, fn, PCI_HEADER_TYPE & ~3u) >> (8u * (PCI_HEADER_TYPE & 3u));
    d = (struct pci_device){.bus = bus, .slot = slot, .function = fn, .vendor = (uint16_t)id,
                            .device = (uint16_t)(id >> 16),
                            .class_code = read32(bus, slot, fn, PCI_CLASS_REVISION)};
    if ((header & 0x7fu) == 1u) {
        /* A bridge: the bus behind it is a bus that exists. */
        uint32_t secondary = read32(bus, slot, fn, PCI_SECONDARY_BUS & ~3u) >>
                             (8u * (PCI_SECONDARY_BUS & 3u));
        note_bus((uint8_t)(secondary & 0xffu));
    }
    kprintf("pci: %02x:%02x.%u  %04x:%04x  class %06x\n", bus, slot, fn, d.vendor, d.device,
            (unsigned)(d.class_code >> 8));
    if (!matches_class(d.class_code, class_code, mask) || assign_bar0(&d) != KERR_OK) {
        return false;
    }
    enable_device(&d);
    *out = d;
    return true;
}

/* Whether a slot has anything in it, and whether to look at its other
 * functions: a single-function device answers for function 0 only, and
 * reading the rest of its functions is reading nothing. */
static bool slot_is_multifunction(uint8_t bus, uint8_t slot) {
    uint32_t header = read32(bus, slot, 0, PCI_HEADER_TYPE & ~3u) >>
                      (8u * (PCI_HEADER_TYPE & 3u));
    return (header & 0x80u) != 0;
}

static bool slot_is_empty(uint8_t bus, uint8_t slot) {
    return no_function_at(read32(bus, slot, 0, PCI_VENDOR_ID));
}

/* Searches the bus tree of whichever bridge is currently selected. */
static int find_class_here(uint32_t class_code, uint32_t mask, struct pci_device *out) {
    bus_count = 0;
    note_bus(0);
    /* `bus_count` grows as bridges are found, so this walks the tree the
     * machine actually has rather than the numbers it might have used. */
    for (uint32_t i = 0; i < bus_count; i++) {
        /* **Only bus 0 has slots.** A PCIe link is point to point: the bus
         * behind a bridge has exactly one device on it, at slot 0. Reading
         * the other 31 is not merely pointless — each one is a transaction
         * the root complex answers only after a completion timeout, so a
         * full sweep of two buses spent tens of seconds producing nothing,
         * which reads exactly like a hang. Conventional PCI, where a bus
         * really can carry 32 devices, is what bus 0 is scanned for. */
        uint8_t slots = buses[i] == 0 ? MAX_SLOTS : 1u;
        for (uint8_t slot = 0; slot < slots; slot++) {
            uint8_t functions;
            if (slot_is_empty(buses[i], slot)) {
                continue;
            }
            functions = slot_is_multifunction(buses[i], slot) ? MAX_FUNCTIONS : 1u;
            for (uint8_t fn = 0; fn < functions; fn++) {
                if (look_at(buses[i], slot, fn, class_code, mask, out)) {
                    return KERR_OK;
                }
            }
        }
    }
    return KERR_NOENT;
}

int pci_find_class(uint32_t class_code, uint32_t mask, struct pci_device *out) {
    if (bridge_count == 0) {
        return KERR_NODEV;
    }
    pci.functions = 0;
    for (uint32_t i = 0; i < bridge_count; i++) {
        (void)select_bridge(i);
        if (find_class_here(class_code, mask, out) == KERR_OK) {
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

void pci_list(void) {
    if (!pci.up) {
        kprintf("pci: no host bridge\n");
        return;
    }
    for (uint8_t bus = 0; bus < MAX_BUSES; bus++) {
        for (uint8_t slot = 0; slot < MAX_SLOTS; slot++) {
            for (uint8_t fn = 0; fn < MAX_FUNCTIONS; fn++) {
                uint32_t id = read32(bus, slot, fn, PCI_VENDOR_ID);
                uint32_t cls;
                if ((id & 0xffffu) == PCI_NO_VENDOR) {
                    break;
                }
                cls = read32(bus, slot, fn, PCI_CLASS_REVISION);
                kprintf("pci: %02x:%02x.%u  %04x:%04x  class %06x\n", bus, slot, fn,
                        id & 0xffffu, id >> 16, cls >> 8);
            }
        }
    }
}

phys_t pci_ecam_base(void) {
    return pci.ecam_base;
}

uint32_t pci_functions_seen(void) {
    return pci.functions;
}

bool pci_ready(void) {
    return pci.up;
}
