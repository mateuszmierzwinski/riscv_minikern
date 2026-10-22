/* virtio-net — a network interface the emulator models.
 *
 * It exists so the IPv4 stack can be proved on this architecture before the
 * board's own Ethernet is written. The stack itself carried over from the
 * ARM project unchanged, because it is pure; what is new is the adapter
 * underneath it, and that is exactly the division a hexagon is supposed to
 * give.
 *
 * Two queues, which is the difference from the block device: a **receive**
 * buffer is handed to the device and waits there until a frame arrives, so
 * it cannot share a queue with transmit — a send would then wait behind a
 * packet that has not been sent to us.
 *
 * The receive side is a fixed set of buffers, one per descriptor index, each
 * re-posted as soon as its frame has been handed up. No free list, and
 * therefore no way to reuse a descriptor the device still owns.
 */
#ifndef MINIKERN_DRIVERS_VIRTIO_NET_H
#define MINIKERN_DRIVERS_VIRTIO_NET_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/net/net.h"
#include "core/ports.h"

/* Brings up the first virtio network device in the tree and reports the port
 * the stack drives it through, plus the hardware address the device claims.
 *
 * KERR_NOENT when there is no such device, which is the ordinary case on a
 * machine without one. */
int virtio_net_init(const struct fdt *fdt, const struct netdev_port **port,
                    uint8_t mac[ETH_ALEN]);

/* Hands every frame that has arrived to `nif`, re-posting each buffer as it
 * goes. Called from the device's interrupt and from the tick, because a
 * driver that only polls on an interrupt loses whatever arrived while it was
 * not looking. */
void virtio_net_poll(struct netif *nif);

/* The device's interrupt number, for the controller to route. 0 when there
 * is no device. */
uint32_t virtio_net_irq(void);

/* How many frames have been received and sent, and how many were dropped
 * because nothing could be posted back. */
void virtio_net_counters(uint64_t *received, uint64_t *sent, uint64_t *dropped);

#endif
