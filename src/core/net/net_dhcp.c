/* net_dhcp — a DHCP client (RFC 2131, options RFC 2132): DISCOVER, OFFER,
 * REQUEST, ACK. A reply counts only when it carries our transaction id and
 * our hardware address, and every option is read within its own length and
 * the packet's. Retransmission backs off from 1 s to 8 s. Renewal is not
 * done yet: leases from the networks we target outlive a session. */
#include "core/errno.h"
#include "core/net/net_internal.h"

#define BOOTP_FIXED     236u     /* op .. file */
#define DHCP_MAGIC      0x63825363u
#define DHCP_MIN_LEN    300u     /* BOOTP's minimum message size */
#define BOOTREQUEST     1u
#define BOOTREPLY       2u
#define BOOTP_BROADCAST 0x8000u
#define OPT_PAD         0u
#define OPT_MASK        1u
#define OPT_ROUTER      3u
#define OPT_DNS         6u
#define OPT_HOSTNAME    12u
#define OPT_REQUESTED   50u
#define OPT_LEASE       51u
#define OPT_MSG_TYPE    53u
#define OPT_SERVER_ID   54u
#define OPT_PARAMS      55u
#define OPT_END         255u
#define MSG_DISCOVER    1u
#define MSG_OFFER       2u
#define MSG_REQUEST     3u
#define MSG_ACK         5u
#define MSG_NAK         6u
#define RETRY_FIRST_MS  1000u
#define RETRY_MAX_MS    8000u

/* The options a reply may carry that we use. */
struct dhcp_reply {
    uint8_t type;
    ip4_t yiaddr;
    ip4_t server;
    ip4_t mask;
    ip4_t router;
    ip4_t dns;
    uint32_t lease;
};

/* Appends one option; returns the new offset. */
static uint32_t put_option(uint8_t *m, uint32_t at, uint8_t code, const uint8_t *val, uint8_t len) {
    m[at] = code;
    m[at + 1] = len;
    net_copy(m + at + 2, val, len);
    return at + 2u + len;
}

static uint32_t put_option_ip(uint8_t *m, uint32_t at, uint8_t code, ip4_t ip) {
    uint8_t v[4];
    put32(v, ip);
    return put_option(m, at, code, v, 4);
}

/* The fixed BOOTP part of a request, into m. */
static void write_bootp(const struct netif *nif, uint8_t *m) {
    net_zero(m, DHCP_MIN_LEN);
    m[0] = BOOTREQUEST;
    m[1] = 1;  /* htype: Ethernet */
    m[2] = ETH_ALEN;
    put32(m + 4, nif->dhcp.xid);
    put16(m + 10, BOOTP_BROADCAST);
    net_copy(m + 28, nif->mac, ETH_ALEN);
    put32(m + BOOTP_FIXED, DHCP_MAGIC);
}

/* True while the client is renewing an address it already holds, which is
 * what decides how the request is addressed and what it may say. */
static bool holds_address(const struct netif *nif) {
    return nif->dhcp.state == DHCP_RENEWING || nif->dhcp.state == DHCP_REBINDING;
}

static int send_message(struct netif *nif, uint8_t type) {
    static const uint8_t params[] = {OPT_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE};
    uint8_t m[DHCP_MIN_LEN];
    uint32_t at = BOOTP_FIXED + 4u;
    bool renewing = holds_address(nif);
    write_bootp(nif, m);
    at = put_option(m, at, OPT_MSG_TYPE, &type, 1);
    if (type == MSG_REQUEST && !renewing) {
        at = put_option_ip(m, at, OPT_REQUESTED, nif->dhcp.offered);
        at = put_option_ip(m, at, OPT_SERVER_ID, nif->dhcp.server);
    }
    at = put_option(m, at, OPT_PARAMS, params, sizeof params);
    at = put_option(m, at, OPT_HOSTNAME, (const uint8_t *)"minikern", 8);
    m[at] = OPT_END;
    if (renewing) {
        /* A renewal carries the address in ciaddr and **no** requested-address
         * or server-id option, and is sent from the address itself: that is
         * what tells a server this is an extension rather than a new
         * request (RFC 2131 §4.3.6). RENEWING goes to the server that
         * granted the lease; REBINDING has stopped expecting an answer from
         * it and asks the subnet. */
        put32(m + 12, nif->ip);
        return net_udp_send_from(nif, nif->ip,
                                 nif->dhcp.state == DHCP_RENEWING ? nif->dhcp.server : IP4_BROADCAST,
                                 DHCP_CLIENT_PORT, DHCP_SERVER_PORT, m, DHCP_MIN_LEN);
    }
    return net_udp_send(nif, IP4_BROADCAST, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, m, DHCP_MIN_LEN);
}

/* Sends the state's message and schedules the next try. */
static void transmit(struct netif *nif) {
    uint32_t backoff = RETRY_FIRST_MS << (nif->dhcp.tries < 3u ? nif->dhcp.tries : 3u);
    (void)send_message(nif, nif->dhcp.state == DHCP_SELECTING ? MSG_DISCOVER : MSG_REQUEST);
    nif->dhcp.tries++;
    nif->dhcp.next_send_ms = nif->now_ms + (backoff > RETRY_MAX_MS ? RETRY_MAX_MS : backoff);
}

void dhcp_start(struct netif *nif, uint32_t xid) {
    nif->dhcp = (struct dhcp_client){.state = DHCP_SELECTING, .xid = xid};
    transmit(nif);
}

/* Moves a bound client along its lease's clock: ask the granting server at
 * T1, ask anyone at T2, and give the address up when it expires.
 *
 * Giving it up matters as much as asking: an address whose lease has run
 * out is someone else's to hand out, and a board that keeps using one is a
 * conflict that shows up as traffic for another machine arriving here. */
static void follow_the_lease(struct netif *nif) {
    if (nif->lease_s == 0 || nif->lease_s == DHCP_LEASE_FOREVER) {
        return; /* a lease with no term is not renewed */
    }
    if (nif->now_ms >= nif->dhcp.expiry_ms) {
        nif->ip = 0; /* it is not ours any more */
        nif->dhcp.state = DHCP_SELECTING;
        nif->dhcp.tries = 0;
        nif->dhcp.next_send_ms = nif->now_ms;
        transmit(nif);
        return;
    }
    if (nif->dhcp.state == DHCP_BOUND && nif->now_ms >= nif->dhcp.renew_at_ms) {
        nif->dhcp.state = DHCP_RENEWING;
        nif->dhcp.tries = 0;
        transmit(nif);
        return;
    }
    if (nif->dhcp.state == DHCP_RENEWING && nif->now_ms >= nif->dhcp.rebind_at_ms) {
        nif->dhcp.state = DHCP_REBINDING;
        nif->dhcp.tries = 0;
        transmit(nif);
    }
}

void dhcp_tick(struct netif *nif) {
    bool waiting = nif->dhcp.state == DHCP_SELECTING || nif->dhcp.state == DHCP_REQUESTING ||
                   holds_address(nif);
    if (nif->dhcp.state == DHCP_BOUND || holds_address(nif)) {
        follow_the_lease(nif);
    }
    if (waiting && nif->now_ms >= nif->dhcp.next_send_ms) {
        if (nif->dhcp.state == DHCP_REQUESTING && nif->dhcp.tries >= 4u) {
            nif->dhcp.state = DHCP_SELECTING;  /* the server went quiet: start over */
            nif->dhcp.tries = 0;
        }
        transmit(nif);
    }
}

static ip4_t option_ip(const uint8_t *v, uint8_t len) {
    return len >= 4 ? get32(v) : 0;
}

/* Records one option of a reply. */
static void read_option(struct dhcp_reply *r, uint8_t code, const uint8_t *v, uint8_t len) {
    switch (code) {
    case OPT_MSG_TYPE: r->type = len >= 1 ? v[0] : 0; break;
    case OPT_SERVER_ID: r->server = option_ip(v, len); break;
    case OPT_MASK: r->mask = option_ip(v, len); break;
    case OPT_ROUTER: r->router = option_ip(v, len); break;
    case OPT_DNS: r->dns = option_ip(v, len); break;
    case OPT_LEASE: r->lease = len >= 4 ? get32(v) : 0; break;
    default: break;
    }
}

/* Walks the options after the magic cookie. KERR_INVAL when one runs past
 * the packet or the END option is missing. */
static int parse_options(const uint8_t *m, uint32_t len, struct dhcp_reply *r) {
    uint32_t at = BOOTP_FIXED + 4u;
    while (at < len) {
        uint8_t code = m[at], olen;
        if (code == OPT_END) {
            return KERR_OK;
        }
        if (code == OPT_PAD) {
            at++;
            continue;
        }
        if (at + 2u > len || (olen = m[at + 1]) > len - at - 2u) {
            return KERR_INVAL;
        }
        read_option(r, code, m + at + 2, olen);
        at += 2u + olen;
    }
    return KERR_INVAL;
}

/* A reply to us: BOOTREPLY, our xid and MAC, the cookie, sane options. */
static int parse_reply(const struct netif *nif, const struct udp_datagram *d, struct dhcp_reply *r) {
    const uint8_t *m = d->data;
    if (d->len < BOOTP_FIXED + 4u || m[0] != BOOTREPLY || get32(m + 4) != nif->dhcp.xid ||
        !mac_equal(m + 28, nif->mac) || get32(m + BOOTP_FIXED) != DHCP_MAGIC) {
        return KERR_INVAL;
    }
    net_zero(r, sizeof *r);
    r->yiaddr = get32(m + 16);
    return parse_options(m, d->len, r);
}

static bool usable_address(ip4_t ip) {
    return ip != 0 && ip != IP4_BROADCAST && (ip >> 24) != 127u && (ip >> 28) < 0xeu;
}

static void take_offer(struct netif *nif, const struct dhcp_reply *r) {
    nif->dhcp.offered = r->yiaddr;
    nif->dhcp.server = r->server;
    nif->dhcp.state = DHCP_REQUESTING;
    nif->dhcp.tries = 0;
    transmit(nif);
}

static void bind_lease(struct netif *nif, const struct dhcp_reply *r) {
    nif->ip = r->yiaddr;
    nif->mask = r->mask != 0 ? r->mask : IP4(255, 255, 255, 0);
    nif->gateway = r->router;
    nif->dns = r->dns;
    nif->lease_s = r->lease;
    nif->dhcp.state = DHCP_BOUND;
    nif->dhcp.tries = 0;
    /* T1 and T2 as RFC 2131 gives them, from the moment the lease was
     * granted rather than from when it was asked for. */
    nif->dhcp.renew_at_ms = nif->now_ms + (uint64_t)r->lease * 500ull;
    nif->dhcp.rebind_at_ms = nif->now_ms + (uint64_t)r->lease * 875ull;
    nif->dhcp.expiry_ms = nif->now_ms + (uint64_t)r->lease * 1000ull;
    if (nif->events.dhcp_bound != NULL) {
        nif->events.dhcp_bound(nif->events.self, nif);
    }
}

/* A NAK from the server we asked sends us back to DISCOVER — including a
 * NAK of a renewal, which is a server saying the address is no longer ours
 * and must not be used while we look for another. */
static void take_nak(struct netif *nif, const struct dhcp_reply *r) {
    if (holds_address(nif)) {
        nif->ip = 0;
        nif->dhcp.state = DHCP_SELECTING;
        nif->dhcp.tries = 0;
        nif->dhcp.next_send_ms = nif->now_ms;
        return;
    }
    if (nif->dhcp.state == DHCP_REQUESTING && r->server == nif->dhcp.server) {
        nif->dhcp.state = DHCP_SELECTING;
        nif->dhcp.tries = 0;
        nif->dhcp.next_send_ms = nif->now_ms;
    }
}

void dhcp_input(struct netif *nif, const struct udp_datagram *d) {
    struct dhcp_reply r;
    if (d->sport != DHCP_SERVER_PORT || parse_reply(nif, d, &r) != KERR_OK) {
        nif->stats.drop_malformed++;
        return;
    }
    if (r.type == MSG_NAK) {
        take_nak(nif, &r);
        return;
    }
    if (!usable_address(r.yiaddr)) {
        nif->stats.drop_malformed++;
        return;
    }
    if (nif->dhcp.state == DHCP_SELECTING && r.type == MSG_OFFER && r.server != 0) {
        take_offer(nif, &r);
    } else if (nif->dhcp.state == DHCP_REQUESTING && r.type == MSG_ACK && r.yiaddr == nif->dhcp.offered &&
               (r.server == 0 || r.server == nif->dhcp.server)) {
        bind_lease(nif, &r);
    } else if (holds_address(nif) && r.type == MSG_ACK && r.yiaddr == nif->ip) {
        /* A renewal was answered: the same address with a new term. It must
         * be accepted in these states too, or the clock never moves and the
         * client renews until the lease runs out underneath it. The address
         * has to match the one we hold — an ACK naming a different one is
         * not an answer to what was asked. */
        bind_lease(nif, &r);
    }
}
