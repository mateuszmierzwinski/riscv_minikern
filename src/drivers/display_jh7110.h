/* The JH7110's display path brought up from nothing: `/dev/fb0` on HDMI.
 *
 * The firmware leaves the whole VOUT domain off and the tree describes
 * none of it, so this does every step itself, in the order the working
 * implementations use (docs/DISPLAY.md §6), and **each step proves itself
 * with something it can read back** — the board's screen is not in the
 * test loop, so a step that cannot show it worked is a step that did not:
 *
 *   1. the monitor's hot-plug line (GPIO 15) and the HDMI rails (PMIC
 *      register 0x11, read only);
 *   2. the DDC and hot-plug pins given to the transmitter;
 *   3. the VOUT power domain on — the PMU reports it;
 *   4. the system clocks feeding VOUT and the VOUT clock generator's own,
 *      their resets released — the display controller then reads its
 *      revision, 0x5720, instead of 0;
 *   5. the transmitter's PLLs locked, and the monitor's EDID read;
 *   6. the framebuffer flushed from the L2 and scanned out — the scan
 *      position moves and the plane does not underflow.
 *
 * What only a person can confirm — a picture on the monitor — is reported
 * as such.
 */
#ifndef MINIKERN_DRIVERS_DISPLAY_JH7110_H
#define MINIKERN_DRIVERS_DISPLAY_JH7110_H

#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* Brings HDMI up with the framebuffer at `fb` (`width` x `height`, a CEA
 * mode, `stride` bytes a line) as its picture. Logs every step and its
 * evidence; returns the first step's failure. */
int display_jh7110_start(const struct fdt *fdt, phys_t fb, uint32_t width, uint32_t height,
                         uint32_t stride);

/* Writes the framebuffer back from the L2 so the controller sees what was
 * drawn. Called from the housekeeping beat while HDMI is up. */
void display_jh7110_refresh(void);

/* How many refreshes ran, and the last one's cost in microseconds. */
void display_jh7110_refresh_stats(uint64_t *count, uint64_t *last_us);

#endif
