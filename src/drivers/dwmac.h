/* The board's own Ethernet: a DesignWare GMAC 5.20 on the JH7110.
 *
 * Everything the register map, the PHY and the clocks needed was measured
 * first, by `dwmac_probe` — see `CLAUDE.md` §3. The short version: the
 * dwmac4 register layout (MDIO at 0x200/0x204, version at 0x110 reading
 * 0x4152), a Motorcomm YT8531 PHY, `rgmii-id`, and the controller to bind
 * is `ethernet@16040000` because that is where the cable is.
 */
#ifndef DRIVERS_DWMAC_H
#define DRIVERS_DWMAC_H

#include <stdbool.h>
#include <stdint.h>

#include "core/ports.h"
#include "core/types.h"

struct fdt;

/* Brings up one controller and reports it as a network device.
 *
 * `sink` is where received frames are delivered; it may be installed later
 * with `dwmac_set_sink`. Returns `KERR_NODEV` when the tree has no
 * controller this driver recognises, or the reason it could not be started.
 */
int dwmac_init(const struct fdt *fdt, const struct netdev_port **port);

/* Where received frames go. */
void dwmac_set_sink(const struct frame_sink *sink);

/* Takes whatever the controller has received and delivers it. Polled,
 * because the kernel has no interrupt for this device yet; one call drains
 * what is ready and returns how many frames it passed on. */
uint32_t dwmac_poll(void);

/* Arms the controller's receive interrupt. Without it this device is only
 * looked at when something else happens, which cannot complete a blocking
 * read: the waiting thread has no deadline, so nothing is scheduled to poll
 * on its behalf. */
void dwmac_enable_rx_interrupt(void);

/* The PLIC source this controller raises, from its own node's `interrupts`.
 * Zero when there is no device or the tree named none. */
uint32_t dwmac_irq(void);

/* Takes every frame on the wire. A diagnostic: it separates "the frame is
 * not arriving" from "the frame arrives and the address filter drops it". */
void dwmac_set_promiscuous(bool on);

/* The receive filter and address registers as the controller holds them. */
void dwmac_report_filter(void);

/* Logs each accepted frame's destination address. Diagnostic. */
void dwmac_trace_destinations(bool on);

/* Keeps the strict receive filter — match this interface's address, accept
 * broadcast — which is what the filter is documented to do and which on this
 * board receives no unicast at all. Call it before `dwmac_init` to reproduce
 * that in one boot; see the comment on `set_the_receive_filter`. */
void dwmac_use_strict_filter(bool on);

/* Acknowledges at the device and drains the ring; the count is how many
 * frames were delivered. */
uint32_t dwmac_handle_interrupt(void);

/* The link, as the PHY reports it: whether it is up, and at what speed in
 * megabits. Both are what the MAC must be configured to match, so they are
 * read rather than assumed. */
bool dwmac_link_up(uint32_t *mbit, bool *full_duplex);

/* Frames sent, delivered and dropped. A driver that reports zero received
 * on a link that is up is a different problem from one that reports
 * hundreds dropped, and the counters are how the two are told apart. */
void dwmac_counters(uint64_t *sent, uint64_t *received, uint64_t *dropped);

/* Loops the MAC back on itself: every frame sent arrives on the receive
 * ring. The only way to test the rings without depending on what else is
 * on the segment — a quiet switch port and a broken receive ring look
 * identical from outside. */
void dwmac_set_loopback(bool on);

/* What the DMA channel reports, and the last transmit descriptor's
 * write-back. A frame counted as sent but whose descriptor the hardware
 * never claimed is the signature of a tail pointer that was not written. */
uint32_t dwmac_dma_status(void);
uint32_t dwmac_last_tx_status(void);

/* The MAC address the controller was given. */
void dwmac_address(uint8_t out[6]);

#endif
