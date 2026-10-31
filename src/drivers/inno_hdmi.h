/* The Innosilicon HDMI transmitter on the JH7110 (`0x2959_0000`).
 *
 * Byte-wide registers on a 32-bit stride: register N is the word at
 * `base + 4 * N`. It owns three things the display needs: the TMDS PHY and
 * its two PLLs (which must lock for any picture), the timing it frames the
 * pixel stream with, and its own I²C master on the DDC wires, which reads
 * the monitor's EDID. Every value written here is from docs/DISPLAY.md §3,
 * where the vendor Linux and U-Boot drivers agree byte for byte.
 *
 * It lives in the VOUT power domain and needs its clocks and reset released
 * first (`display_jh7110.c`): before that it reads 0 and ignores writes.
 */
#ifndef MINIKERN_DRIVERS_INNO_HDMI_H
#define MINIKERN_DRIVERS_INNO_HDMI_H

#include <stdbool.h>
#include <stdint.h>

#include "core/display_mode.h"
#include "core/types.h"

/* Maps the transmitter at `base`. The device tree passed by this board's
 * firmware has no node for it, so the address is the TRM's. */
int inno_hdmi_init(phys_t base);

/* Whether a monitor pulls the hot-plug line, as the transmitter sees it. */
bool inno_hdmi_hotplug(void);

/* Programs the PHY's PLLs for `t` (1080p60 or 720p60), waits for both to
 * lock, frames the stream with `t`'s timing in DVI mode (no infoframes:
 * every monitor takes it, and neither vendor stack sends a valid AVI
 * infoframe), and turns the TMDS drivers on. KERR_NOTSUP for a mode with no
 * PLL table; KERR_TIMEDOUT naming which PLL did not lock. */
int inno_hdmi_set_mode(const struct display_timing *t, const char **why);

/* Reads EDID block `block` (0 = the base block) over the DDC wires. */
int inno_hdmi_read_edid(uint8_t block, uint8_t out[EDID_BLOCK_BYTES]);

/* Switches the transmitter's built-in colour bars on or off: a picture that
 * needs no display controller, for telling a dead transmitter from a dead
 * pixel stream with nothing but a monitor. */
void inno_hdmi_colour_bars(bool on);

#endif
