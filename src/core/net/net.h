/* net — minikern's IPv4 stack: Ethernet II, ARP, IPv4, ICMP echo, UDP and
 * a DHCP client, for one interface. Pure: it sees frames through a
 * netdev_port and time through net_tick, so the whole stack is host-tested
 * with crafted and hostile frames.
 *
 * Policy, chosen for a network that is not trusted:
 *  - every length and offset in a received frame is checked against the
 *    frame before it is read; anything malformed is counted and dropped;
 *  - IPv4 fragments are dropped (nothing we speak needs them);
 *  - ARP learns only answers to requests it sent, or refreshes entries it
 *    already holds, so an unsolicited reply cannot redirect our traffic;
 *  - ICMP echo replies are rate-limited (a token bucket);
 *  - DHCP options are parsed with bounds and an option-length check. */
#ifndef MINIKERN_CORE_NET_NET_H
#define MINIKERN_CORE_NET_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/ports.h"

#define ETH_ALEN       6u
#define ETH_HLEN       14u
#define ETH_MTU        1500u
#define ETH_FRAME_MAX  (ETH_HLEN + ETH_MTU)
#define NET_ARP_ENTRIES 8u
#define NET_UDP_PORTS   32u
#define NET_EPHEMERAL_LO 49152u  /* IANA dynamic ports: where autobind looks */
#define NET_LOOP_QUEUE  8u   /* locally addressed packets waiting to be delivered */
#define NET_IP_PROTOS   2u   /* IP protocols registered beyond ICMP and UDP */
#define NET_ICMP_BURST  32u  /* echo replies allowed in a burst, refilled 32/s */

/* IPv4 addresses are host-order integers: 10.0.2.15 is 0x0a00020f. */
typedef uint32_t ip4_t;

#define IP4(a, b, c, d) (((ip4_t)(a) << 24) | ((ip4_t)(b) << 16) | ((ip4_t)(c) << 8) | (ip4_t)(d))
#define IP4_BROADCAST 0xffffffffu

struct netif;

/* A received UDP datagram, pointing into the frame. */
struct udp_datagram {
    ip4_t src;
    ip4_t dst;
    uint16_t sport;
    uint16_t dport;
    const uint8_t *data;
    uint32_t len;
};

/* What an IP protocol above the network layer does with a packet: the
 * payload after the IPv4 header, already checked for length and checksum.
 * TCP registers itself this way (ICMP and UDP are built in). */
struct ip_handler {
    void (*input)(void *self, struct netif *nif, ip4_t src, ip4_t dst, const uint8_t *p, uint32_t len);
    void *self;
};

/* What one bound UDP port does with a datagram. */
struct udp_handler {
    void (*receive)(void *self, struct netif *nif, const struct udp_datagram *d);
    void *self;
};

/* What the stack tells its owner. */
struct net_events {
    void (*dhcp_bound)(void *self, const struct netif *nif);
    void (*echo_reply)(void *self, ip4_t from, uint16_t seq);
    void *self;
};

struct arp_entry {
    ip4_t ip;
    uint8_t mac[ETH_ALEN];
    uint64_t learned_ms;
    bool valid;
    bool requested;  /* we asked: a reply for this ip will be accepted */
};

/* A lease of 0xffffffff seconds is "for ever" by convention, and is not
 * renewed: there is nothing to renew it before. */
#define DHCP_LEASE_FOREVER 0xffffffffu

enum dhcp_state {
    DHCP_OFF = 0,
    DHCP_SELECTING,  /* DISCOVER sent, waiting for an OFFER */
    DHCP_REQUESTING, /* REQUEST sent, waiting for an ACK */
    DHCP_BOUND,
    /* A lease is a loan with a term, and these are the two halves of
     * paying attention to it: at T1 — half the lease — the client asks the
     * server that granted it to extend it, and at T2 — seven eighths — it
     * stops being particular and asks anyone. Without either, the address
     * stops being ours while we go on using it, which is a conflict that
     * appears as someone else's traffic arriving (RFC 2131 §4.4.5). */
    DHCP_RENEWING,  /* T1 has passed: asking the server we have, directly */
    DHCP_REBINDING, /* T2 has passed: asking any server, by broadcast */
};

struct dhcp_client {
    enum dhcp_state state;
    uint32_t xid;
    ip4_t offered;
    ip4_t server;
    uint64_t next_send_ms;
    uint32_t tries;
    uint64_t renew_at_ms;  /* T1: half the lease */
    uint64_t rebind_at_ms; /* T2: seven eighths of it */
    uint64_t expiry_ms;    /* when the address stops being ours */
};

/* Why frames were dropped: each is a test assertion and a log line. */
struct net_stats {
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t drop_malformed;
    uint32_t drop_checksum;
    uint32_t drop_fragment;
    uint32_t drop_not_ours;
    uint32_t drop_arp_unsolicited;
    uint32_t drop_rate_limited;
    uint32_t drop_no_port;
    uint32_t drop_loop_full;
};

struct netif {
    struct netdev_port dev;
    struct net_events events;
    uint8_t mac[ETH_ALEN];
    ip4_t ip;      /* 0 until DHCP binds */
    ip4_t mask;
    ip4_t gateway;
    ip4_t dns;
    uint32_t lease_s;
    uint64_t now_ms;
    struct arp_entry arp[NET_ARP_ENTRIES];
    struct dhcp_client dhcp;
    uint16_t udp_port[NET_UDP_PORTS];
    struct udp_handler udp[NET_UDP_PORTS];
    uint8_t ip_proto[NET_IP_PROTOS];
    struct ip_handler ip_handler[NET_IP_PROTOS];
    uint16_t next_ephemeral;
    uint32_t icmp_tokens;
    uint64_t icmp_refill_ms;
    uint8_t held[ETH_FRAME_MAX];  /* one IPv4 packet waiting for ARP */
    uint32_t held_len;
    ip4_t held_hop;
    uint8_t tx[ETH_FRAME_MAX];
    uint8_t loop[NET_LOOP_QUEUE][ETH_MTU];  /* IP packets to ourselves */
    uint32_t loop_len[NET_LOOP_QUEUE];
    uint32_t loop_head, loop_count;
    uint32_t input_depth;  /* >0 while a frame is being processed */
    struct net_stats stats;
};

void net_init(struct netif *nif, const uint8_t mac[ETH_ALEN], const struct netdev_port *dev,
              const struct net_events *events);

/* One received frame (without FCS). */
void net_input(struct netif *nif, const uint8_t *frame, uint32_t len);

/* True for 127.0.0.0/8 and, once configured, our own address. */
bool net_is_local(const struct netif *nif, ip4_t ip);

/* Time passes: DHCP retransmits, the ICMP bucket refills. */
void net_tick(struct netif *nif, uint64_t now_ms);

/* Starts address configuration. */
void dhcp_start(struct netif *nif, uint32_t xid);

/* Binds port to handler. KERR_EXIST, KERR_NOMEM (table full) or KERR_OK. */
int net_udp_bind(struct netif *nif, uint16_t port, const struct udp_handler *h);

/* Registers a handler for one IP protocol number (6 for TCP). KERR_OK,
 * KERR_EXIST or KERR_NOMEM. */
int net_ip_register(struct netif *nif, uint8_t proto, const struct ip_handler *h);

/* Sends a packet of an upper protocol: the payload must already be in the
 * frame's transmit buffer, which net_l4_buffer points at. */
int net_ip_send(struct netif *nif, ip4_t dst, uint8_t proto, uint32_t payload_len);

/* The same with an explicit source address, which must be the one the
 * protocol's checksum was computed over. */
int net_ip_send_from(struct netif *nif, ip4_t src, ip4_t dst, uint8_t proto, uint32_t payload_len);

/* The source address the stack would use to reach dst. */
ip4_t net_source_for(const struct netif *nif, ip4_t dst);

/* Where an upper protocol writes its header and data before sending. */
uint8_t *net_l4_buffer(struct netif *nif);

/* The largest upper-protocol payload one frame carries. */
#define NET_L4_MAX (ETH_MTU - 20u)

/* Releases port (no-op when unbound). */
void net_udp_unbind(struct netif *nif, uint16_t port);

/* A free port in the dynamic range, or 0 when all bindable slots or ports
 * are taken. Picks sequentially from a rotating start. */
uint16_t net_udp_ephemeral(struct netif *nif);

/* The largest UDP payload one frame carries (no fragmentation). */
#define NET_UDP_MAX (ETH_MTU - 20u - 8u)

/* Sends a UDP datagram. KERR_OK, KERR_AGAIN (waiting for ARP: the packet is
 * held and sent when the answer comes), KERR_INVAL (too big), or the
 * device's error. */
int net_udp_send(struct netif *nif, ip4_t dst, uint16_t sport, uint16_t dport, const void *data, uint32_t len);

/* The same from a chosen source address (0: let the stack choose), for a
 * socket bound to one. */
int net_udp_send_from(struct netif *nif, ip4_t src, ip4_t dst, uint16_t sport, uint16_t dport, const void *data,
                      uint32_t len);

/* Sends a UDP datagram whose Ethernet frame is addressed to the broadcast
 * MAC although its IP destination is one host. The netboot sweep uses this:
 * probing every address in a subnet would otherwise need an ARP resolution
 * each, and only one packet can wait for ARP at a time. Every host on the
 * segment sees the frame and every one but the addressee drops it, exactly
 * as it would drop a misdelivered unicast. */
int net_udp_send_unresolved(struct netif *nif, ip4_t dst, uint16_t sport, uint16_t dport, const void *data,
                            uint32_t len);

/* Sends an ICMP echo request with 32 bytes of payload. */
int net_ping(struct netif *nif, ip4_t dst, uint16_t id, uint16_t seq);

/* The RFC 862 echo service as a UDP handler. It will not answer a
 * broadcast (fraggle) or a datagram from another small service's port (7,
 * 13, 19, 37: the echo/chargen ping-pong), and shares the ICMP rate limit. */
struct udp_handler net_echo_service(void);

/* RFC 1071 Internet checksum of len bytes, folded and complemented, with
 * sum as the running value of earlier parts (0 to start). */
uint16_t net_checksum(const void *data, uint32_t len, uint32_t sum);

/* The unfolded one's-complement sum, for checksums over several parts. */
uint32_t net_sum(const void *data, uint32_t len, uint32_t sum);

/* Parses a dotted quad of exactly len characters. KERR_OK or KERR_INVAL;
 * nothing but four decimal octets separated by dots is accepted. */
int ip4_parse(const char *text, uint32_t len, ip4_t *out);

/* Formats ip as dotted quad into buf (16 bytes). */
const char *ip4_format(ip4_t ip, char buf[16]);

#endif
