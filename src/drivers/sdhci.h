/* sdhci — an SD card, through a standard host controller.
 *
 * The JH7110's SD controller is a Cadence part that implements the **SD Host
 * Controller Interface**, which is a published standard rather than a vendor
 * interface — so this driver is written against the standard and binds to
 * anything that implements it, including the `sdhci-pci` controller an
 * emulator offers. That is what makes an SD driver testable at all without
 * the board: the card protocol is identical, and only where the registers
 * live differs.
 *
 * Two protocols stacked, and keeping them apart is what keeps this
 * readable:
 *
 *   **the host controller** — registers for issuing a command, clocking the
 *   bus, and moving a block;
 *   **the card** — a sequence of numbered commands (CMD0 to reset, CMD8 to
 *   ask what voltage it wants, ACMD41 to power it up, CMD2 and CMD3 to give
 *   it an address, CMD9 to read its capacity) that every SD card since 2006
 *   answers the same way.
 *
 * The one piece of real variation is **capacity**: a card up to 2 GiB
 * reports its size as a geometry to multiply out, and anything larger (an
 * "SDHC" or "SDXC" card) reports it as a block count in a different layout
 * of the same register — and addresses blocks rather than bytes. Getting
 * that wrong gives a working driver that sees a 32 MiB card where a 32 GiB
 * one is fitted.
 */
#ifndef MINIKERN_DRIVERS_SDHCI_H
#define MINIKERN_DRIVERS_SDHCI_H

#include "core/blockdev.h"
#include "core/fdt.h"

/* The PCI class of an SD host controller: base system peripheral, SD host.
 * The third byte is the programming interface — 01 for "SDHCI 1.0
 * compliant", 02 for a DMA-only variant — so it is masked out: this driver
 * uses the standard registers either way. */
#define SDHCI_PCI_CLASS 0x080500u
#define SDHCI_PCI_CLASS_MASK 0xffff00u

/* Finds a host controller — in the device tree by `compatible` first, which
 * is how the board presents one, then on the PCI bus, which is how an
 * emulator does — initialises the card in it and presents it as a block
 * device.
 *
 * KERR_NOENT when there is no controller or no card in it. A controller with
 * an empty slot is not an error: that is a board with nothing in its SD
 * socket, which is most of them. */
int sdhci_init(const struct fdt *fdt, struct blockdev *out);

/* What was found, for the boot log: whether it is a high-capacity card, its
 * geometry, and where the controller is. */
const char *sdhci_kind(void);
uint64_t sdhci_blocks(void);

#endif
