/* net (kernel) — the one network interface, wired up: frames from the
 * device's interrupt into the stack (core/net), the stack's 10 ms tick from
 * the timer, address configuration by DHCP at boot, a ping of the gateway
 * to prove the path, and the RFC 862 UDP echo service on port 7. */
#ifndef MINIKERN_KERNEL_NET_H
#define MINIKERN_KERNEL_NET_H

#include <stdint.h>

#include "core/net/net.h"
#include "core/ports.h"

#define NET_ECHO_PORT 7u

/* Creates the interface over dev with mac. Returns the sink the device
 * driver must deliver received frames to. */
struct frame_sink knet_init(const uint8_t mac[ETH_ALEN], const struct netdev_port *dev);

/* Runs the stack's timers. Called from the timer tick, and from any loop
 * that is waiting for the network to do something. */
void knet_tick(void);

/* Starts DHCP and the tick (once the device is up). */
void knet_start(uint32_t xid);

/* Brings the interface up with an address given rather than asked for, and
 * starts no DHCP client at all.
 *
 * A board on a segment with no DHCP server is not a strange case: two
 * machines on a cable with nothing else between them have no server, and
 * that is exactly how the netboot test proves discovery between two guests
 * rather than through an emulator's NAT. prefix is the mask's length;
 * gateway may be 0, which means nothing is routed off the segment. */
void knet_start_static(ip4_t addr, uint32_t prefix, ip4_t gateway, ip4_t dns);

/* Overrides the resolver the lease advertised, which is what
 * minikern.dns=<ip> is for: a board on a network whose DHCP points at a
 * resolver it cannot reach, and the DNS test, which runs a resolver of its
 * own. Takes effect for every /etc/resolv.conf opened afterwards. */
void knet_set_dns(ip4_t dns);

/* Waits, with interrupts open, until DHCP has given the interface an
 * address. KERR_OK, or KERR_TIMEDOUT after timeout_ms.
 *
 * Anything that dials out at boot must call this first: the network comes
 * up in the background, so without it the first SYN leaves with no source
 * address and is never answered (CLAUDE.md §10). */
int knet_wait_address(uint32_t timeout_ms);

/* The interface, for later layers (sockets). */
struct netif *knet_interface(void);

/* The TCP stack running on it. */
struct tcp_stack *knet_tcp(void);

/* Prints the interface and TCP counters (minikern.netstat=1). */
void knet_report(void);

/* Polls the device (`poll`, or virtio's when NULL) and runs the stack's
 * timers **holding the kernel lock** — taken here when this hart does not
 * already hold it. The stack belongs to the lock's holder: a boot-time loop
 * that polled without it raced another hart's housekeeping beat polling the
 * same ring (`dwmac: … overlapping polls`). */
void knet_poll_locked(void (*poll)(void));

/* What `knet_report` adds about the device underneath — the composition
 * root's to install, since this layer does not know which device it is. */
void knet_set_device_report(void (*report)(void));

#endif
