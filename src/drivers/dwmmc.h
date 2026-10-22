/* dwmmc — the SD controller a VisionFive 2 actually has.
 *
 * **Not SDHCI.** This project was told, from documentation, that the JH7110's
 * SD host controller was Cadence SDHCI-compatible, and an SDHCI driver was
 * written and proved against an emulator's model of one. The board's own
 * device tree says otherwise:
 *
 *     compatible = "starfive,jh7110-mmc";
 *     clock-names = "biu", "ciu";
 *     fifo-depth = <0x20>;
 *     data-addr = <0>;
 *
 * `biu`/`ciu` and a FIFO depth are Synopsys **DesignWare mobile storage**
 * properties. It is a different controller with a different register set, so
 * the SDHCI driver was not a driver with a bug in it — it was the wrong
 * driver, and nothing in an emulator could have said so. This is the rule
 * this project already had, arriving: where documentation and a measurement
 * disagree, the measurement decides.
 *
 * The card's side of the conversation is shared with the SDHCI path
 * (`sdcard.h`); only the transport is here.
 *
 * Programmed I/O through the FIFO, not the controller's internal DMA. The
 * DMA would be faster and needs a descriptor list in memory the controller
 * can reach; the FIFO is correct, needs nothing, and is fast enough for a
 * kernel that is reading a filesystem rather than streaming.
 */
#ifndef MINIKERN_DRIVERS_DWMMC_H
#define MINIKERN_DRIVERS_DWMMC_H

#include "core/blockdev.h"
#include "core/fdt.h"

/* Brings up the first DesignWare MMC controller in the tree that has a card
 * in it, and reports it as a block device.
 *
 * KERR_NOENT when the tree has no such controller — the ordinary case on an
 * emulator — and KERR_NODEV when there is one but no card. */
int dwmmc_init(const struct fdt *fdt, struct blockdev *out);

/* Logs the controller's own reason when a command fails: a response
 * timeout, a response CRC failure and a data timeout are three different
 * faults that an `EIO` makes indistinguishable. */
void dwmmc_set_trace(bool on);

/* Where it was found, and how big the card is, for a report. */
const char *dwmmc_path(void);
uint64_t dwmmc_blocks(void);

#endif
