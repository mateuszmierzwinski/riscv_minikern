/* pci — enumerating a PCI Express bus, and giving its devices addresses.
 *
 * This exists for the M.2 slot. The VisionFive 2's NVMe drive is behind a
 * PCIe root complex, and so is any SD controller on a card — so one bus
 * walker serves both, and the drivers above it never touch configuration
 * space themselves.
 *
 * Configuration space is reached through **ECAM**: the host bridge maps
 * every function's 4 KiB of configuration registers into memory, at an
 * address derived from bus, device and function. That is the whole mechanism
 * — no port I/O, no indirection registers — which is why a walker is short
 * enough to be worth writing from scratch.
 *
 * **The firmware has not done this for us.** On a machine that boots Linux,
 * U-Boot or the kernel assigns the memory windows and writes them into each
 * device's base-address registers. Here nothing has, so a freshly enumerated
 * device has BARs full of zeroes and must be given addresses out of the
 * windows the device tree publishes in the bridge's `ranges`. Forgetting
 * that produces a device that answers configuration reads perfectly and
 * ignores every access to its registers.
 */
#ifndef MINIKERN_DRIVERS_PCI_H
#define MINIKERN_DRIVERS_PCI_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* Configuration-space offsets this kernel reads. */
#define PCI_VENDOR_ID 0x00u
#define PCI_DEVICE_ID 0x02u
#define PCI_COMMAND 0x04u
#define PCI_STATUS 0x06u
#define PCI_CLASS_REVISION 0x08u
#define PCI_HEADER_TYPE 0x0eu
#define PCI_BAR0 0x10u
#define PCI_SECONDARY_BUS 0x19u

/* Command-register bits. */
#define PCI_COMMAND_IO 0x0001u
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_MASTER 0x0004u

/* One function on the bus. */
struct pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t vendor;
    uint16_t device;
    uint32_t class_code;  /* class << 16 | subclass << 8 | prog-if */
    phys_t bar0;          /* assigned by this driver, not by firmware */
    uint64_t bar0_size;
};

/* Finds the host bridge in the device tree, maps its ECAM region and reads
 * the memory windows from `ranges`. KERR_NOENT when the machine has no PCIe,
 * which is ordinary and not a failure. */
int pci_init(const struct fdt *fdt);

/* Walks the bus for the first function whose class matches, assigns it a
 * memory window for BAR0, maps that window, and enables memory decoding and
 * bus mastering — a device that cannot master cannot read the queues a
 * driver builds for it, and the symptom is a controller that initialises
 * perfectly and never completes a command.
 *
 * `class_code` is compared on the top 24 bits: 0x010802 is NVMe, 0x080501 an
 * SD host controller. */
int pci_find_class(uint32_t class_code, struct pci_device *out);

/* Configuration-space access, for a driver that needs more than the walker
 * read. */
uint32_t pci_config_read32(const struct pci_device *d, uint32_t offset);
void pci_config_write32(const struct pci_device *d, uint32_t offset, uint32_t value);

/* Where the bridge is, how many functions exist, and how much window is
 * left — for the boot log, and because "it found nothing" and "it never
 * looked" must not read the same. */
phys_t pci_ecam_base(void);
uint32_t pci_functions_seen(void);
bool pci_ready(void);

#endif
