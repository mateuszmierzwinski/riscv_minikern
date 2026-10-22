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
        if ((tag >> 24) == 0x02u) {
            pci.mem_base = parent;
            pci.mem_size = size;
            return KERR_OK;
        }
    }
    return KERR_NOENT;
}

int pci_init(const struct fdt *fdt) {
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    int rc = fdt_find_compatible(fdt, "pci-host-ecam-generic", path, sizeof path);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = fdt_device_reg(fdt, path, 0, &base, &size);
    rc = rc != KERR_OK ? rc : read_memory_window(fdt, path);
    if (rc != KERR_OK) {
        return rc;
    }
    /* Only the buses this kernel walks are mapped: the whole ECAM region is
     * 256 MiB of address space for 256 buses, and mapping it all would be
     * 65536 pages of page tables for devices that are not there. */
    rc = mmu_map_device(base, (uint64_t)MAX_BUSES << ECAM_BUS_SHIFT);
    if (rc != KERR_OK) {
        return rc;
    }
    pci.ecam = (volatile uint8_t *)(uintptr_t)base;
    pci.ecam_base = base;
    pci.ecam_size = size;
    pci.up = true;
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
    return mmu_map_device(at, size);
}

/* Lets the device decode memory **and master the bus**. The second is the
 * one that is easy to forget and impossible to diagnose: a controller
 * without it initialises perfectly, accepts commands, and never completes
 * one, because it cannot read the queues the driver built for it. */
static void enable_device(struct pci_device *d) {
    uint32_t cmd = pci_config_read32(d, PCI_COMMAND);
    pci_config_write32(d, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

static bool matches_class(uint32_t class_revision, uint32_t wanted, uint32_t mask) {
    return ((class_revision >> 8) & mask) == (wanted & mask);
}

int pci_find_class(uint32_t class_code, uint32_t mask, struct pci_device *out) {
    if (!pci.up) {
        return KERR_NODEV;
    }
    pci.functions = 0;
    for (uint8_t bus = 0; bus < MAX_BUSES; bus++) {
        for (uint8_t slot = 0; slot < MAX_SLOTS; slot++) {
            for (uint8_t fn = 0; fn < MAX_FUNCTIONS; fn++) {
                uint32_t id = read32(bus, slot, fn, PCI_VENDOR_ID);
                struct pci_device d;
                if ((id & 0xffffu) == PCI_NO_VENDOR) {
                    break; /* nothing in this slot: its other functions are absent too */
                }
                pci.functions++;
                d = (struct pci_device){.bus = bus, .slot = slot, .function = fn,
                                        .vendor = (uint16_t)id, .device = (uint16_t)(id >> 16),
                                        .class_code = read32(bus, slot, fn, PCI_CLASS_REVISION)};
                if (!matches_class(d.class_code, class_code, mask)) {
                    continue;
                }
                if (assign_bar0(&d) != KERR_OK) {
                    continue;
                }
                enable_device(&d);
                *out = d;
                return KERR_OK;
            }
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
