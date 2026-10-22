/* net_udp — UDP (RFC 768): checksummed in and out, one handler per port. */
#include "core/errno.h"
#include "core/net/net_internal.h"

/* The pseudo-header sum: source, destination, protocol and UDP length. */
static uint32_t pseudo_sum(ip4_t src, ip4_t dst, uint32_t len) {
    uint8_t ph[12];
    put32(ph, src);
    put32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = IPPROTO_UDP;
    put16(ph + 10, (uint16_t)len);
    return net_sum(ph, sizeof ph, 0);
}

static int find_port(const struct netif *nif, uint16_t port) {
    for (uint32_t i = 0; i < NET_UDP_PORTS; i++) {
        if (nif->udp[i].receive != NULL && nif->udp_port[i] == port) {
            return (int)i;
        }
    }
    return -1;
}

int net_udp_bind(struct netif *nif, uint16_t port, const struct udp_handler *h) {
    if (find_port(nif, port) >= 0) {
        return KERR_EXIST;
    }
    for (uint32_t i = 0; i < NET_UDP_PORTS; i++) {
        if (nif->udp[i].receive == NULL) {
            nif->udp_port[i] = port;
            nif->udp[i] = *h;
            return KERR_OK;
        }
    }
    return KERR_NOMEM;
}

void net_udp_unbind(struct netif *nif, uint16_t port) {
    int slot = find_port(nif, port);
    if (slot >= 0) {
        nif->udp[slot] = (struct udp_handler){0};
        nif->udp_port[slot] = 0;
    }
}

uint16_t net_udp_ephemeral(struct netif *nif) {
    uint32_t span = 65536u - NET_EPHEMERAL_LO;
    for (uint32_t i = 0; i < span; i++) {
        uint16_t port = (uint16_t)(NET_EPHEMERAL_LO + (nif->next_ephemeral + i) % span);
        if (find_port(nif, port) < 0) {
            nif->next_ephemeral = (uint16_t)((nif->next_ephemeral + i + 1u) % span);
            return port;
        }
    }
    return 0;
}

void udp_input(struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len) {
    struct udp_datagram d;
    uint32_t ulen;
    int slot;
    if (len < UDP_HLEN || (ulen = get16(p + 4)) < UDP_HLEN || ulen > len) {
        nif->stats.drop_malformed++;
        return;
    }
    if (get16(p + 6) != 0 && net_checksum(p, ulen, pseudo_sum(src, dst, ulen)) != 0) {
        nif->stats.drop_checksum++;
        return;
    }
    d = (struct udp_datagram){.src = src, .dst = dst, .sport = get16(p), .dport = get16(p + 2),
                              .data = p + UDP_HLEN, .len = ulen - UDP_HLEN};
    if (d.dport == DHCP_CLIENT_PORT) {
        dhcp_input(nif, &d);
        return;
    }
    slot = find_port(nif, d.dport);
    if (slot < 0) {
        nif->stats.drop_no_port++;
        return;
    }
    nif->udp[slot].receive(nif->udp[slot].self, nif, &d);
}

/* Writes the datagram into nif->tx and returns its total length, or a
 * negative error. */
static int32_t udp_build(struct netif *nif, ip4_t src, ip4_t dst, uint16_t sport, uint16_t dport,
                         const void *data, uint32_t len) {
    uint8_t *u = TX_L4(nif);
    uint16_t sum;
    if (len > ETH_MTU - IP_HLEN - UDP_HLEN) {
        return KERR_INVAL;
    }
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint16_t)(UDP_HLEN + len));
    put16(u + 6, 0);
    net_copy(u + UDP_HLEN, data, len);
    sum = net_checksum(u, UDP_HLEN + len, pseudo_sum(src, dst, UDP_HLEN + len));
    put16(u + 6, sum == 0 ? 0xffffu : sum);
    return (int32_t)(UDP_HLEN + len);
}

int net_udp_send_from(struct netif *nif, ip4_t src, ip4_t dst, uint16_t sport, uint16_t dport, const void *data,
                      uint32_t len) {
    ip4_t from = src != 0 ? src : ipv4_source_for(nif, dst);
    int32_t total = udp_build(nif, from, dst, sport, dport, data, len);
    return total < 0 ? (int)total : ipv4_send_from(nif, from, dst, IPPROTO_UDP, (uint32_t)total);
}

int net_udp_send(struct netif *nif, ip4_t dst, uint16_t sport, uint16_t dport, const void *data, uint32_t len) {
    return net_udp_send_from(nif, 0, dst, sport, dport, data, len);
}

int net_udp_send_unresolved(struct netif *nif, ip4_t dst, uint16_t sport, uint16_t dport, const void *data,
                            uint32_t len) {
    ip4_t from = ipv4_source_for(nif, dst);
    int32_t total = udp_build(nif, from, dst, sport, dport, data, len);
    return total < 0 ? (int)total : ipv4_send_unresolved(nif, dst, IPPROTO_UDP, (uint32_t)total);
}
