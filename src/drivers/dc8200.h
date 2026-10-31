/* The Verisilicon DC8200 display controller on the JH7110 (`0x2940_0000`).
 *
 * It reads a framebuffer from memory and turns it into a timed pixel
 * stream; on this board its display 0 feeds the HDMI transmitter. One
 * primary plane is all a framebuffer needs: an address, a stride, a format
 * and a size, plus the display's timing. Overlays and the hardware cursor
 * exist and stay off (docs/DISPLAY.md §4 and Part 2 — they are the chip's
 * real 2D acceleration, and a later step).
 *
 * The register values follow StarFive's U-Boot write sequence, the one
 * validated on this SoC, with the timing words computed by
 * `display_dc_timing` and checked against that sequence's 1080p constants.
 */
#ifndef MINIKERN_DRIVERS_DC8200_H
#define MINIKERN_DRIVERS_DC8200_H

#include <stdbool.h>
#include <stdint.h>

#include "core/display_mode.h"
#include "core/types.h"

/* What a powered, clocked controller reports as its revision. */
#define DC8200_REVISION 0x5720u

/* Maps the controller at `base` (no tree node on this board's firmware). */
int dc8200_init(phys_t base);

/* The revision register: DC8200_REVISION when the VOUT domain is on and the
 * controller's clocks and resets are released, 0 before. */
uint32_t dc8200_revision(void);

/* Scans out `stride`-byte lines of XRGB8888 at physical `fb` on display 0
 * with timing `t`. The controller takes a 32-bit address, so `fb` must lie
 * below 4 GiB: KERR_RANGE otherwise. */
int dc8200_scanout(phys_t fb, uint32_t stride, const struct display_timing *t);

/* The registers that decide whether display 0 runs, for the log when it
 * does not: plane config and its extension, the panel's config, start and
 * commit, the output format, and the top block's interrupt status. */
void dc8200_report(void);

/* Arms display 0's vsync interrupt flag, and whether it has been raised
 * since (read and cleared): a frame boundary passing, which proves the
 * controller runs independently of the scan-position register. */
void dc8200_arm_vsync(void);
bool dc8200_vsync_seen(void);

/* Whether the primary plane has run dry — the controller could not fetch
 * pixels in time. */
bool dc8200_underflow(void);

#endif
