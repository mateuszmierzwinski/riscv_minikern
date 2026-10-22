#include "drivers/virtio_net.h"

#include "core/console.h"
#include "core/errno.h"
#include "drivers/virtio.h"
#include "kernel/mem.h"

/* The header every virtio-net buffer carries, in front of the frame.
 *
 * 12 bytes in the modern layout, and it is **present on both directions**:
 * the device writes one in front of every frame it gives us, and expects one
 * in front of every frame we give it. A driver that forgets it on transmit
 * sends a frame whose first 12 bytes are missing, which looks like a
 * corrupted destination address rather than a missing header. */
struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};

#define NET_HDR_BYTES 12u

/* The device's configuration space: the MAC address first. */
#define NET_CONFIG_MAC 0u

/* One receive buffer per descriptor, each big enough for a full frame plus
 * the header. 8 buffers at 1600 bytes is 12 KiB — small, and the stack
 * drains them on every tick, so a burst that overruns them costs a
 * retransmission rather than a wedged interface. The ARM project learned
 * that the other way round: a 64-block netboot window arrived as one burst
 * and overran a 32-entry ring. */
#define RX_BUFFER_BYTES 1600u

static struct {
    struct virtio_dev dev;
    phys_t rx[VIRTIO_QUEUE_SIZE];
    phys_t tx;
    uint8_t mac[ETH_ALEN];
    bool up;
    uint64_t received;
    uint64_t sent;
    uint64_t dropped;
} net;

/* Reads the hardware address out of configuration space.
 *
 * The device supplies one and this driver uses it rather than inventing a
 * locally administered address: a frame sent from an address the model does
 * not expect is silently dropped by the host's side, which looks exactly
 * like a driver that cannot transmit. */
static void read_mac(void) {
    uint32_t low = virtio_config_u32(&net.dev, NET_CONFIG_MAC);
    uint32_t high = virtio_config_u32(&net.dev, NET_CONFIG_MAC + 4u);
    net.mac[0] = (uint8_t)low;
    net.mac[1] = (uint8_t)(low >> 8);
    net.mac[2] = (uint8_t)(low >> 16);
    net.mac[3] = (uint8_t)(low >> 24);
    net.mac[4] = (uint8_t)high;
    net.mac[5] = (uint8_t)(high >> 8);
}

/* Hands one buffer back to the device, ready for the next frame. */
static void post_receive_buffer(uint16_t index) {
    virtio_post_buffer(&net.dev, VIRTIO_QUEUE_RX, index, net.rx[index],
                       RX_BUFFER_BYTES + NET_HDR_BYTES);
}

static int allocate_buffers(void) {
    for (uint32_t i = 0; i < VIRTIO_QUEUE_SIZE; i++) {
        int rc = mem_alloc_pages(1, 1, &net.rx[i]);
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return mem_alloc_pages(1, 1, &net.tx);
}

/* Transmit: the header, then the frame, as a two-descriptor chain.
 *
 * Synchronous, like the block device's requests, because the stack has
 * nothing to overlap it with — it builds one frame, sends it, and returns.
 * The header is zeroed: no checksum offload and no segmentation are
 * claimed, so the device must take the frame exactly as given. */
static int transmit(void *self, const uint8_t *frame, uint32_t len) {
    struct virtio_net_hdr *hdr = (struct virtio_net_hdr *)(uintptr_t)net.tx;
    uint8_t *payload = (uint8_t *)(uintptr_t)net.tx + NET_HDR_BYTES;
    const void *bufs[2];
    uint32_t lens[2];
    const bool writable[2] = {false, false};
    int rc;
    (void)self;
    if (!net.up || len == 0 || len > RX_BUFFER_BYTES) {
        return KERR_INVAL;
    }
    *hdr = (struct virtio_net_hdr){0};
    for (uint32_t i = 0; i < len; i++) {
        payload[i] = frame[i];
    }
    bufs[0] = hdr;
    lens[0] = NET_HDR_BYTES;
    bufs[1] = payload;
    lens[1] = len;
    rc = virtio_request_on(&net.dev, VIRTIO_QUEUE_TX, bufs, lens, writable, 2u);
    net.sent += rc == KERR_OK ? 1u : 0u;
    net.dropped += rc != KERR_OK ? 1u : 0u;
    return rc;
}

static const struct netdev_port virtio_net_port = {.send = transmit, .self = NULL};

int virtio_net_init(const struct fdt *fdt, const struct netdev_port **port,
                    uint8_t mac[ETH_ALEN]) {
    int rc = virtio_find(fdt, VIRTIO_ID_NET, 2u, &net.dev);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = allocate_buffers();
    if (rc != KERR_OK) {
        return rc;
    }
    read_mac();
    net.up = true;
    /* Every receive buffer posted before the device is told to look, so a
     * frame that arrives in the first microsecond has somewhere to go. */
    for (uint16_t i = 0; i < VIRTIO_QUEUE_SIZE; i++) {
        post_receive_buffer(i);
    }
    virtio_kick(&net.dev, VIRTIO_QUEUE_RX);
    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        mac[i] = net.mac[i];
    }
    *port = &virtio_net_port;
    kprintf("net: %s, mac %02x:%02x:%02x:%02x:%02x:%02x, %u receive buffers\n", net.dev.path,
            net.mac[0], net.mac[1], net.mac[2], net.mac[3], net.mac[4], net.mac[5],
            VIRTIO_QUEUE_SIZE);
    return KERR_OK;
}

void virtio_net_poll(struct netif *nif) {
    uint16_t index = 0;
    uint32_t len = 0;
    if (!net.up) {
        return;
    }
    (void)virtio_ack_interrupt(&net.dev);
    while (virtio_take_completion(&net.dev, VIRTIO_QUEUE_RX, &index, &len)) {
        const uint8_t *frame = (const uint8_t *)(uintptr_t)net.rx[index] + NET_HDR_BYTES;
        /* The length the device reports **includes the header**, so the
         * frame is that much shorter. Handing the whole length up would
         * feed the stack 12 bytes of trailing rubbish on every packet. */
        if (len > NET_HDR_BYTES) {
            net.received++;
            net_input(nif, frame, len - NET_HDR_BYTES);
        }
        post_receive_buffer(index);
    }
    virtio_kick(&net.dev, VIRTIO_QUEUE_RX);
}

uint32_t virtio_net_irq(void) {
    return net.up ? net.dev.irq : 0u;
}

void virtio_net_counters(uint64_t *received, uint64_t *sent, uint64_t *dropped) {
    *received = net.received;
    *sent = net.sent;
    *dropped = net.dropped;
}
