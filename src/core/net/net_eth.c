/* net_eth — Ethernet II framing and the stack's entry points. */
#include "core/errno.h"
#include "core/net/net_internal.h"

void net_init(struct netif *nif, const uint8_t mac[ETH_ALEN], const struct netdev_port *dev,
              const struct net_events *events) {
    net_zero(nif, sizeof *nif);
    net_copy(nif->mac, mac, ETH_ALEN);
    nif->dev = *dev;
    nif->events = *events;
    nif->icmp_tokens = NET_ICMP_BURST;
}

int eth_send(struct netif *nif, const uint8_t dst[ETH_ALEN], uint16_t type, uint32_t payload_len) {
    if (payload_len > ETH_MTU) {
        return KERR_INVAL;
    }
    net_copy(nif->tx, dst, ETH_ALEN);
    net_copy(nif->tx + ETH_ALEN, nif->mac, ETH_ALEN);
    put16(nif->tx + 12, type);
    nif->stats.tx_frames++;
    return nif->dev.send(nif->dev.self, nif->tx, ETH_HLEN + payload_len);
}

/* Frames for us: our MAC or broadcast (DHCP and ARP need the latter). */
static bool addressed_to_us(const struct netif *nif, const uint8_t *dst) {
    return mac_equal(dst, nif->mac) || mac_equal(dst, mac_broadcast);
}

void net_input(struct netif *nif, const uint8_t *frame, uint32_t len) {
    uint16_t type;
    nif->stats.rx_frames++;
    if (frame == NULL || len < ETH_HLEN || len > ETH_FRAME_MAX) {
        nif->stats.drop_malformed++;
        return;
    }
    if (!addressed_to_us(nif, frame)) {
        nif->stats.drop_not_ours++;
        return;
    }
    type = get16(frame + 12);
    nif->input_depth++;
    if (type == ETHERTYPE_ARP) {
        arp_input(nif, frame + ETH_HLEN, len - ETH_HLEN);
    } else if (type == ETHERTYPE_IPV4) {
        ipv4_input(nif, frame + ETH_HLEN, len - ETH_HLEN);
    } else {
        nif->stats.drop_not_ours++;
    }
    nif->input_depth--;
    if (nif->input_depth == 0) {
        ipv4_drain_loop(nif);
    }
}

/* The ICMP bucket refills NET_ICMP_BURST tokens per second. */
static void refill_icmp(struct netif *nif) {
    if (nif->now_ms - nif->icmp_refill_ms >= 1000u) {
        nif->icmp_tokens = NET_ICMP_BURST;
        nif->icmp_refill_ms = nif->now_ms;
    }
}

void net_tick(struct netif *nif, uint64_t now_ms) {
    nif->now_ms = now_ms;
    refill_icmp(nif);
    dhcp_tick(nif);
}
