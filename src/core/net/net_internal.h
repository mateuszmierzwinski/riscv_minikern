/* net_internal — what the stack's parts share; not for its users. */
#ifndef MINIKERN_CORE_NET_NET_INTERNAL_H
#define MINIKERN_CORE_NET_NET_INTERNAL_H

#include "core/net/net.h"

#define ETHERTYPE_IPV4 0x0800u
#define ETHERTYPE_ARP  0x0806u
#define IP_HLEN        20u
#define UDP_HLEN       8u
#define IPPROTO_ICMP   1u
#define IPPROTO_UDP    17u
#define DHCP_CLIENT_PORT 68u
#define DHCP_SERVER_PORT 67u

static inline uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void net_copy(void *dst, const void *src, uint32_t n);
void net_zero(void *dst, uint32_t n);
bool mac_equal(const uint8_t *a, const uint8_t *b);
extern const uint8_t mac_broadcast[ETH_ALEN];

/* Ethernet: writes the header into nif->tx and sends len payload bytes
 * that follow it there. */
int eth_send(struct netif *nif, const uint8_t dst[ETH_ALEN], uint16_t type, uint32_t payload_len);

/* ARP. */
void arp_input(struct netif *nif, const uint8_t *p, uint32_t len);
const struct arp_entry *arp_lookup(const struct netif *nif, ip4_t ip);
int arp_request(struct netif *nif, ip4_t ip);

/* IPv4: input, and output of a payload already placed after the IP header
 * in nif->tx. */
void ipv4_input(struct netif *nif, const uint8_t *p, uint32_t len);
int ipv4_send(struct netif *nif, ip4_t dst, uint8_t proto, uint32_t payload_len);

/* ipv4_send with an explicit source address (0: let the stack choose).
 * An upper layer that computed its checksum over a particular source must
 * pass that same one, or the receiver's check fails. */
int ipv4_send_from(struct netif *nif, ip4_t src, ip4_t dst, uint8_t proto, uint32_t payload_len);

/* The source address the stack would use for dst: the loopback address
 * for a loopback destination, otherwise the interface's. */
ip4_t ipv4_source_for(const struct netif *nif, ip4_t dst);

/* ipv4_send with the frame forced to the broadcast MAC (no ARP). */
int ipv4_send_unresolved(struct netif *nif, ip4_t dst, uint8_t proto, uint32_t payload_len);

/* Delivers queued locally addressed packets. */
void ipv4_drain_loop(struct netif *nif);

/* After ARP learns hop: sends a held packet for it, if any. */
void ipv4_release_held(struct netif *nif, ip4_t hop);

void icmp_input(struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len);
void udp_input(struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len);

/* DHCP: datagrams for port 68, and retransmission. */
void dhcp_input(struct netif *nif, const struct udp_datagram *d);
void dhcp_tick(struct netif *nif);

/* Where payloads start in nif->tx. */
#define TX_IP(nif)  ((nif)->tx + ETH_HLEN)
#define TX_L4(nif)  ((nif)->tx + ETH_HLEN + IP_HLEN)

#endif
