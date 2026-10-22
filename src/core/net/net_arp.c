/* net_arp — ARP for IPv4 over Ethernet (RFC 826): answers requests for our
 * address, asks for next hops, and keeps a small cache. */
#include "core/errno.h"
#include "core/net/net_internal.h"

#define ARP_LEN       28u
#define ARP_HTYPE_ETH 1u
#define ARP_OP_REQUEST 1u
#define ARP_OP_REPLY   2u

static struct arp_entry *find(struct netif *nif, ip4_t ip) {
    for (uint32_t i = 0; i < NET_ARP_ENTRIES; i++) {
        if ((nif->arp[i].valid || nif->arp[i].requested) && nif->arp[i].ip == ip) {
            return &nif->arp[i];
        }
    }
    return NULL;
}

const struct arp_entry *arp_lookup(const struct netif *nif, ip4_t ip) {
    for (uint32_t i = 0; i < NET_ARP_ENTRIES; i++) {
        if (nif->arp[i].valid && nif->arp[i].ip == ip) {
            return &nif->arp[i];
        }
    }
    return NULL;
}

/* A free slot, else the oldest entry. */
static struct arp_entry *victim(struct netif *nif) {
    struct arp_entry *old = &nif->arp[0];
    for (uint32_t i = 0; i < NET_ARP_ENTRIES; i++) {
        if (!nif->arp[i].valid && !nif->arp[i].requested) {
            return &nif->arp[i];
        }
        if (nif->arp[i].learned_ms < old->learned_ms) {
            old = &nif->arp[i];
        }
    }
    return old;
}

/* Writes an ARP packet into nif->tx after the Ethernet header and sends it. */
static int arp_send(struct netif *nif, uint16_t op, const uint8_t *tha, ip4_t tpa, const uint8_t *eth_dst) {
    uint8_t *p = nif->tx + ETH_HLEN;
    put16(p, ARP_HTYPE_ETH);
    put16(p + 2, ETHERTYPE_IPV4);
    p[4] = ETH_ALEN;
    p[5] = 4;
    put16(p + 6, op);
    net_copy(p + 8, nif->mac, ETH_ALEN);
    put32(p + 14, nif->ip);
    net_copy(p + 18, tha, ETH_ALEN);
    put32(p + 24, tpa);
    return eth_send(nif, eth_dst, ETHERTYPE_ARP, ARP_LEN);
}

int arp_request(struct netif *nif, ip4_t ip) {
    static const uint8_t zero[ETH_ALEN];
    struct arp_entry *e = find(nif, ip);
    if (e == NULL) {
        e = victim(nif);
        *e = (struct arp_entry){.ip = ip, .learned_ms = nif->now_ms};
    }
    e->requested = true;
    return arp_send(nif, ARP_OP_REQUEST, zero, ip, mac_broadcast);
}

/* Learns sha for spa. A reply is believed only when we asked for spa or
 * already hold it; a request addressed to us may add the asker (RFC 826's
 * merge), since we are about to talk to it. */
static void learn(struct netif *nif, ip4_t spa, const uint8_t *sha, bool may_add) {
    struct arp_entry *e = find(nif, spa);
    if (e == NULL && !may_add) {
        nif->stats.drop_arp_unsolicited++;
        return;
    }
    if (e == NULL) {
        e = victim(nif);
        e->ip = spa;
    }
    net_copy(e->mac, sha, ETH_ALEN);
    e->valid = true;
    e->requested = false;
    e->learned_ms = nif->now_ms;
    ipv4_release_held(nif, spa);
}

static bool well_formed(const uint8_t *p, uint32_t len) {
    return len >= ARP_LEN && get16(p) == ARP_HTYPE_ETH && get16(p + 2) == ETHERTYPE_IPV4 && p[4] == ETH_ALEN &&
           p[5] == 4;
}

void arp_input(struct netif *nif, const uint8_t *p, uint32_t len) {
    uint16_t op;
    ip4_t spa, tpa;
    uint8_t sha[ETH_ALEN];
    if (!well_formed(p, len)) {
        nif->stats.drop_malformed++;
        return;
    }
    op = get16(p + 6);
    net_copy(sha, p + 8, ETH_ALEN);
    spa = get32(p + 14);
    tpa = get32(p + 24);
    if (nif->ip == 0 || tpa != nif->ip || spa == 0) {
        nif->stats.drop_not_ours++;
        return;
    }
    if (op == ARP_OP_REQUEST) {
        (void)arp_send(nif, ARP_OP_REPLY, sha, spa, sha);
    }
    learn(nif, spa, sha, op == ARP_OP_REQUEST);
}
