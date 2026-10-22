#include "core/net/netboot.h"

#include "lib/ed25519.h"

#include "core/errno.h"
#include "core/net/net_internal.h"
#include "lib/sha256.h"

/* The hosts of the lease's subnet, capped: a /16 is not walked. */
static uint32_t subnet_hosts(const struct netif *nif) {
    uint32_t span = ~nif->mask;
    if (nif->mask == 0 || span < 2u) {
        return 0;
    }
    span -= 1u;  /* .0 is the network and the all-ones address is broadcast */
    return span < MKNB_SWEEP_MAX ? span : MKNB_SWEEP_MAX;
}

/* Writes the common header into buf. */
static void put_header(uint8_t *buf, const struct netboot *nb, enum mknb_type type, uint32_t arg) {
    put32(buf, MKNB_MAGIC);
    buf[4] = MKNB_VERSION;
    buf[5] = (uint8_t)type;
    put16(buf + 6, 0);
    put32(buf + 8, nb->xid);
    put32(buf + 12, arg);
}

/* A probe to one address. An address whose MAC we already hold (the
 * gateway, after the boot-time ping) is probed as an ordinary unicast;
 * the rest go in a broadcast frame, because resolving every address of a
 * subnet by ARP would cost one exchange each and only one packet can wait
 * for ARP at a time. */
static void send_discover(struct netboot *nb, ip4_t to) {
    uint8_t m[MKNB_HEADER + ETH_ALEN + MKNB_BOARD_ID];
    put_header(m, nb, MKNB_DISCOVER, 0);
    net_copy(m + MKNB_HEADER, nb->mac, ETH_ALEN);
    net_copy(m + MKNB_HEADER + ETH_ALEN, nb->board_id, MKNB_BOARD_ID);
    if (arp_lookup(nb->nif, to) != NULL) {
        (void)net_udp_send(nb->nif, to, nb->port, MKNB_PORT, m, sizeof m);
    } else {
        (void)net_udp_send_unresolved(nb->nif, to, nb->port, MKNB_PORT, m, sizeof m);
    }
    nb->stats.probes_sent++;
}

/* The address of host number n in our subnet, gateway first. */
static ip4_t sweep_address(const struct netboot *nb, uint32_t n) {
    ip4_t network = nb->nif->ip & nb->nif->mask;
    ip4_t gateway = nb->nif->gateway;
    ip4_t host = network + n;
    if (n == 0) {
        return gateway != 0 ? gateway : network + 1u;
    }
    if (host == gateway || host == nb->nif->ip) {
        return 0;  /* the gateway went first; never probe ourselves */
    }
    return host;
}

/* One burst of the sweep; returns false when this round is spent. */
static bool sweep_burst(struct netboot *nb) {
    for (uint32_t i = 0; i < MKNB_SWEEP_BURST; i++) {
        ip4_t to;
        if (nb->sweep_host > nb->sweep_hosts) {
            return false;
        }
        to = sweep_address(nb, nb->sweep_host++);
        if (to != 0) {
            send_discover(nb, to);
        }
    }
    return true;
}

int netboot_start(struct netboot *nb, struct netif *nif, uint8_t *image, uint32_t capacity, uint32_t *bitmap,
                  uint32_t xid, const uint8_t mac[ETH_ALEN], const uint8_t board_id[MKNB_BOARD_ID], ip4_t pinned) {
    struct udp_handler h;
    int rc;
    if (nif->ip == 0 || image == NULL || capacity < MKNB_BLOCK) {
        return KERR_INVAL;
    }
    net_zero(nb, sizeof *nb);
    nb->nif = nif;
    nb->xid = xid;
    nb->image = image;
    nb->capacity = capacity;
    nb->bitmap = bitmap;
    nb->pinned = pinned;
    net_copy(nb->mac, mac, ETH_ALEN);
    net_copy(nb->board_id, board_id, MKNB_BOARD_ID);
    nb->port = net_udp_ephemeral(nif);
    h = netboot_handler(nb);
    rc = nb->port == 0 ? KERR_NOMEM : net_udp_bind(nif, nb->port, &h);
    if (rc != KERR_OK) {
        return rc;
    }
    nb->sweep_hosts = pinned != 0 ? 0 : subnet_hosts(nif);
    nb->state = NETBOOT_SWEEPING;
    nb->now_ms = nif->now_ms;
    nb->deadline_ms = nif->now_ms + MKNB_DEADLINE_MS;
    nb->gateway_until_ms = pinned != 0 ? 0 : nif->now_ms + MKNB_GATEWAY_GRACE_MS;
    if (nif->gateway != 0 && arp_lookup(nif, nif->gateway) == NULL) {
        (void)arp_request(nif, nif->gateway);  /* so its probe goes out addressed */
    }
    return KERR_OK;
}

void netboot_stop(struct netboot *nb) {
    if (nb->port != 0) {
        net_udp_unbind(nb->nif, nb->port);
        nb->port = 0;
    }
}

/* Sends one of our messages to the server, counting what went wrong. */
static void send_to_server(struct netboot *nb, const uint8_t *m, uint32_t len) {
    int rc = net_udp_send(nb->nif, nb->server, nb->port, MKNB_PORT, m, len);
    if (rc != KERR_OK) {
        nb->stats.send_errors++;
        nb->stats.last_send_rc = rc;
    }
}

static void fail(struct netboot *nb, enum netboot_error e) {
    nb->state = NETBOOT_FAILED;
    nb->error = e;
}

static bool block_present(const struct netboot *nb, uint32_t i) {
    return (nb->bitmap[i / 32u] >> (i % 32u)) & 1u;
}

/* The lowest block we still need, or nb->blocks when none. */
static uint32_t lowest_missing(const struct netboot *nb, uint32_t from) {
    for (uint32_t i = from; i < nb->blocks; i++) {
        if (!block_present(nb, i)) {
            return i;
        }
    }
    return nb->blocks;
}

/* Asks for a window starting at the lowest block still missing. The
 * server's token goes with it: a server that served blocks to anyone who
 * asked would be a 3000-fold UDP amplifier (NETBOOT.md §5). */
static void send_read(struct netboot *nb) {
    uint8_t m[MKNB_HEADER + MKNB_READ_BODY];
    uint32_t base = lowest_missing(nb, 0);
    uint32_t count = nb->blocks - base < MKNB_WINDOW ? nb->blocks - base : MKNB_WINDOW;
    if (base >= nb->blocks) {
        return;
    }
    put_header(m, nb, MKNB_READ, base);
    put16(m + MKNB_HEADER, (uint16_t)count);
    put16(m + MKNB_HEADER + 2, 0);
    put32(m + MKNB_HEADER + 4, nb->token);
    nb->window_base = base;
    nb->window_end = base + count;
    nb->stall_at_ms = nb->now_ms + MKNB_STALL_MS;
    nb->stats.reads_sent++;
    send_to_server(nb, m, sizeof m);
}

static void send_done(struct netboot *nb) {
    uint8_t m[MKNB_HEADER];
    put_header(m, nb, MKNB_DONE, 0);
    send_to_server(nb, m, sizeof m);
}

/* Everything arrived: the digest decides whether it may be used. */
static void finish(struct netboot *nb) {
    uint8_t got[32];
    sha256(nb->image, nb->image_len, got);
    if (!sha256_equal(got, nb->digest)) {
        fail(nb, NETBOOT_ERR_DIGEST);
        return;
    }
    nb->state = NETBOOT_READY;
    send_done(nb);
}

/* What the signature covers, built the same way on both ends: the session
 * id, the image length and the digest, big-endian. The xid is in it so that
 * an offer recorded from one boot cannot be replayed into another. */
static void signed_bytes(const struct netboot *nb, const uint8_t *body, uint32_t len, uint8_t out[MKNB_SIGNED_BYTES]) {
    put32(out, nb->xid);
    put32(out + 4, len);
    net_copy(out + 8, body + 4, 32);
}

/* True when the offer carries a good signature, or when none is required. */
static bool offer_is_signed(const struct netboot *nb, const struct udp_datagram *d, const uint8_t *body,
                            uint32_t len) {
    uint8_t message[MKNB_SIGNED_BYTES];
    if (!nb->require_signature) {
        return true;
    }
    if (d->len < MKNB_HEADER + MKNB_OFFER_SIGNED) {
        return false; /* an unsigned offer, where one was required */
    }
    signed_bytes(nb, body, len, message);
    return ed25519_verify(nb->key, body + MKNB_OFFER_BODY, message, sizeof message);
}

/* An OFFER: the first valid one fixes the server and starts the transfer. */
static void take_offer(struct netboot *nb, const struct udp_datagram *d, uint32_t len) {
    const uint8_t *body = d->data + MKNB_HEADER;
    if (nb->state != NETBOOT_SWEEPING || d->len < MKNB_HEADER + MKNB_OFFER_BODY || get16(body) != MKNB_BLOCK) {
        nb->stats.offers_refused++;
        return;
    }
    if (len == 0 || len > nb->capacity) {
        nb->stats.offers_refused++;
        fail(nb, NETBOOT_ERR_TOO_BIG);
        return;
    }
    if (!offer_is_signed(nb, d, body, len)) {
        /* Not from the machine this board trusts. Refused at the offer, so
         * the sweep carries on and answering first gains nothing. */
        nb->stats.offers_refused++;
        return;
    }
    if (nb->pinned_digest && !sha256_equal(body + 4, nb->expected)) {
        /* Not the image this board accepts. Refusing the offer rather than
         * the transfer means the sweep carries on, so answering first buys
         * an impostor nothing at all. */
        nb->stats.offers_refused++;
        return;
    }
    nb->server = d->src;
    nb->image_len = len;
    nb->blocks = (len + MKNB_BLOCK - 1u) / MKNB_BLOCK;
    net_copy(nb->digest, body + 4, 32);
    nb->token = get32(body + 36);
    nb->state = NETBOOT_FETCHING;
    nb->stats.offers_seen++;
    send_read(nb);
}

/* A DATA block: its index and length must match the image exactly. */
static void take_data(struct netboot *nb, const struct udp_datagram *d, uint32_t index) {
    uint32_t payload = d->len - MKNB_HEADER;
    uint32_t offset = index * MKNB_BLOCK;
    uint32_t expect;
    if (nb->state != NETBOOT_FETCHING || d->src != nb->server || index >= nb->blocks) {
        nb->stats.blocks_refused++;
        return;
    }
    expect = nb->image_len - offset < MKNB_BLOCK ? nb->image_len - offset : MKNB_BLOCK;
    if (payload != expect) {
        nb->stats.blocks_refused++;
        return;
    }
    if (block_present(nb, index)) {
        nb->stats.blocks_duplicate++;
        return;
    }
    net_copy(nb->image + offset, d->data + MKNB_HEADER, payload);
    nb->bitmap[index / 32u] |= 1u << (index % 32u);
    nb->have++;
    nb->stats.blocks_stored++;
    nb->stalls = 0;
    nb->stall_at_ms = nb->now_ms + MKNB_STALL_MS;
    if (nb->have == nb->blocks) {
        finish(nb);
    } else if (lowest_missing(nb, nb->window_base) >= nb->window_end) {
        send_read(nb);  /* the window is complete, however it arrived */
    }
}

/* Header checks every datagram must pass: ours, this version, our session. */
static bool header_ok(const struct netboot *nb, const struct udp_datagram *d) {
    return d->len >= MKNB_HEADER && d->sport == MKNB_PORT && get32(d->data) == MKNB_MAGIC &&
           d->data[4] == MKNB_VERSION && get32(d->data + 8) == nb->xid;
}

static void receive(void *self, struct netif *nif, const struct udp_datagram *d) {
    struct netboot *nb = self;
    (void)nif;
    if (!header_ok(nb, d) || netboot_finished(nb)) {
        return;
    }
    if (d->data[5] == MKNB_OFFER) {
        take_offer(nb, d, get32(d->data + 12));
    } else if (d->data[5] == MKNB_DATA) {
        take_data(nb, d, get32(d->data + 12));
    } else if (d->data[5] == MKNB_ERROR && (nb->server == 0 || d->src == nb->server)) {
        fail(nb, NETBOOT_ERR_SERVER);
    }
}

struct udp_handler netboot_handler(struct netboot *nb) {
    return (struct udp_handler){.receive = receive, .self = nb};
}

/* Re-probes one address every few stall periods. */
static void probe_again(struct netboot *nb, ip4_t to) {
    if (nb->now_ms >= nb->stall_at_ms) {
        send_discover(nb, to);
        nb->stall_at_ms = nb->now_ms + MKNB_STALL_MS;
    }
}

/* The gateway's head start: it is the likeliest server (and on a NAT the
 * only address that is really there), so it is probed on its own first,
 * which also gives its ARP resolution time to finish. Without this a host
 * that answers later in the sweep could win the race. */
static bool gateway_phase(struct netboot *nb) {
    if (nb->now_ms >= nb->gateway_until_ms || nb->nif->gateway == 0) {
        return false;
    }
    probe_again(nb, nb->nif->gateway);
    return true;
}

/* Keeps probing: the pinned address, the gateway, or the next burst. */
static void drive_sweep(struct netboot *nb) {
    if (nb->pinned != 0) {
        probe_again(nb, nb->pinned);
        return;
    }
    if (gateway_phase(nb)) {
        return;
    }
    if (sweep_burst(nb)) {
        return;
    }
    if (++nb->sweep_round >= MKNB_SWEEP_ROUNDS) {
        fail(nb, NETBOOT_ERR_NO_SERVER);
        return;
    }
    nb->sweep_host = 0;
}

/* Gives up on a server that offered an image and then sent nothing (or
 * nothing we could use), and resumes the sweep. A responder that cannot
 * deliver — a rogue, or an address a NAT invented — must not be able to
 * spend the whole boot. */
static void abandon_server(struct netboot *nb) {
    uint32_t words = (nb->blocks + 31u) / 32u;
    nb->stats.servers_abandoned++;
    nb->abandoned++;
    for (uint32_t i = 0; i < words; i++) {
        nb->bitmap[i] = 0;
    }
    nb->server = 0;
    nb->token = 0;
    nb->have = 0;
    nb->blocks = 0;
    nb->image_len = 0;
    nb->stalls = 0;
    nb->state = NETBOOT_SWEEPING;
    nb->gateway_until_ms = 0;  /* the gateway had its chance */
    nb->stall_at_ms = nb->now_ms;
}

void netboot_tick(struct netboot *nb, uint64_t now_ms) {
    nb->now_ms = now_ms;
    if (netboot_finished(nb) || nb->state == NETBOOT_IDLE) {
        return;
    }
    if (now_ms >= nb->deadline_ms) {
        fail(nb, nb->state == NETBOOT_SWEEPING ? NETBOOT_ERR_NO_SERVER : NETBOOT_ERR_TIMEOUT);
        return;
    }
    if (nb->state == NETBOOT_SWEEPING) {
        drive_sweep(nb);
    } else if (now_ms >= nb->stall_at_ms) {
        if (++nb->stalls > MKNB_STALL_LIMIT) {
            abandon_server(nb);
        } else {
            send_read(nb);
        }
    }
}

const char *netboot_error_text(enum netboot_error e) {
    static const char *const texts[] = {"none", "no server answered", "image too large for the buffer",
                                        "digest mismatch", "transfer timed out", "the server refused"};
    return e <= NETBOOT_ERR_SERVER ? texts[e] : "?";
}

void netboot_expect_digest(struct netboot *nb, const uint8_t digest[32]) {
    net_copy(nb->expected, digest, 32);
    nb->pinned_digest = true;
}

void netboot_require_signature(struct netboot *nb, const uint8_t key[32]) {
    net_copy(nb->key, key, 32);
    nb->require_signature = true;
}
