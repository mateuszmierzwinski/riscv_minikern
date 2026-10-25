/* USB on the VisionFive 2: an XHCI controller, the VL805 behind PCIe.
 *
 * The board's VL805 has been enumerated on PCIe since the bus work and
 * ignored ever since — a device found, named in a log line and never driven.
 * This is the driver.
 *
 * **Staged deliberately, because XHCI is large and each stage is provable on
 * its own.** Bringing the controller to its running state proves the
 * register map, the rings and the DMA; reading port status proves it can see
 * what is plugged in; enumerating proves the control transfers; mass storage
 * proves bulk transfers. A driver written all at once and then debugged as
 * one thing is how a week disappears.
 */
#ifndef DRIVERS_XHCI_H
#define DRIVERS_XHCI_H

#include "core/fdt.h"
#include "core/types.h"

/* Finds the controller on PCIe, maps it and brings it to its running state:
 * reset, the device-context base array, the command ring, the event ring,
 * then `USBCMD.RS`. KERR_NOENT when there is no XHCI on the bus. */
int xhci_init(void);

/* The controller's own description of itself, for reporting: the interface
 * version, and how many ports and device slots it has. These come from
 * registers the specification fixes, so a wrong window shows up here as
 * zeroes or nonsense rather than later as a transfer that never completes. */
void xhci_describe(uint32_t *version, uint32_t *ports, uint32_t *slots);

/* Whether the controller reached its running state. */
bool xhci_running(void);

/* Resets each port and reports what is attached. Returns how many ports have
 * a device connected. */
uint32_t xhci_scan_ports(void);

/* Port `n`'s raw status-and-control word, one-based as the specification
 * numbers them. */
uint32_t xhci_port_status(uint32_t n);

/* Whether this controller's contexts are 32 or 64 bytes, which it decides
 * and a driver must not assume. */
uint32_t xhci_context_bytes(void);

/* Which USB major version port `n` speaks, from the controller's Supported
 * Protocol capability. A USB2 and a USB3 port must be reset differently, and
 * nothing about a port's own register says which it is. */
uint8_t xhci_port_major(uint32_t n);

/* Prints each port register around the reset write. Diagnostic. */
void xhci_trace_ports(bool on);

/* What enumerating a device found. */
struct xhci_device {
    uint32_t port;
    uint32_t slot;
    uint32_t speed;
    uint16_t vendor;
    uint16_t product;
    uint8_t device_class;
    uint8_t max_packet0;
};

/* Addresses the first enabled device and reads its device descriptor: a
 * slot, its contexts, an Address Device command and one control transfer.
 * KERR_NOENT when no port has an enabled device. */
int xhci_enumerate_first_port(struct xhci_device *out);

#endif
