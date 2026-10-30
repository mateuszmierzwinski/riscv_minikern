/* The MKNB client against a fake server on the wire, and against hostile
 * datagrams. The fake server reads what the client actually sent (the
 * frames the netdev captured) and answers by feeding frames back in, so
 * the whole protocol runs through the real stack. */
#include "core/net/netboot.h"

#include <stdlib.h>

#include "core/errno.h"
#include "ktest.h"

#include "offer_vector.h"
#include "lib/sha256.h"
#include "net_frames.h"

#define SERVER_IP   IP4(10, 0, 2, 2)      /* also the gateway: probed first */
#define OTHER_IP    IP4(10, 0, 2, 77)
#define CLIENT_IP   IP4(10, 0, 2, 15)
#define IMAGE_BYTES 5000u                 /* 4 full blocks plus a short one */
#define CAPACITY    (64u * MKNB_BLOCK)
#define QUEUE_MAX   512

static const uint8_t client_mac[6] = {0x02, 0x4d, 0x4b, 0, 0, 1};
static const uint8_t server_mac[6] = {0x52, 0x55, 0x0a, 0, 2, 2};
static const uint8_t board_id[MKNB_BOARD_ID] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* How the fake server should misbehave. */
struct server_config {
    ip4_t ip;
    bool silent;            /* never answer a probe */
    bool wrong_digest;      /* offer a digest that will not match */
    bool sign;              /* append a signature to the offer */
    bool spoil_signature;   /* ...and move a bit of it */
    bool bad_block_size;    /* offer a block size we do not support */
    uint32_t claim_len;     /* 0: the real length */
    uint32_t drop_first;    /* lose the first transmission of every n-th block */
    bool drop_all;          /* answer every READ with nothing */
    uint32_t corrupt_block; /* send this block's bytes wrong (0: none) */
    bool spoof_from_other;  /* send DATA from OTHER_IP */
};

static struct server_config cfg;
static struct capture cap;
static struct netif nif;
static struct netboot nb;
static uint8_t image_buf[CAPACITY];
static uint32_t bitmap[NETBOOT_BITMAP_WORDS(CAPACITY)];
static uint8_t source[IMAGE_BYTES];
static uint8_t source_digest[32];
static uint32_t data_sent;
static uint32_t attempts[64];
static uint32_t discovers_seen;
static uint32_t probed_hosts;
static uint32_t done_seen;
static uint32_t reads_seen;
static uint32_t token_seen;

/* Frames the server wants delivered, injected after the send returns. */
static struct {
    uint8_t frame[QUEUE_MAX][ETH_FRAME_MAX];
    uint32_t len[QUEUE_MAX];
    uint32_t n, next;
} queue;

static void enqueue(const uint8_t *f, uint32_t len) {
    if (queue.n < QUEUE_MAX) {
        memcpy(queue.frame[queue.n], f, len);
        queue.len[queue.n++] = len;
    }
}

/* Builds one MKNB message from the server to the client. */
static uint32_t server_message(uint8_t *f, uint8_t type, uint32_t arg, const void *body, uint32_t body_len,
                               uint16_t client_port, ip4_t from) {
    uint8_t m[MKNB_HEADER + MKNB_BLOCK];
    fp32(m, MKNB_MAGIC);
    m[4] = MKNB_VERSION;
    m[5] = type;
    fp16(m + 6, 0);
    fp32(m + 8, nb.xid);
    fp32(m + 12, arg);
    memcpy(m + MKNB_HEADER, body, body_len);
    return frame_udp(f, client_mac, server_mac, from, CLIENT_IP, MKNB_PORT, client_port, m,
                     MKNB_HEADER + body_len);
}

#define SERVER_TOKEN 0x5eed1234u

static void answer_discover(uint16_t client_port) {
    uint8_t f[ETH_FRAME_MAX], body[MKNB_OFFER_SIGNED];
    uint32_t len = cfg.claim_len != 0 ? cfg.claim_len : IMAGE_BYTES;
    uint32_t size = MKNB_OFFER_BODY;
    fp16(body, cfg.bad_block_size ? 4096u : MKNB_BLOCK);
    fp16(body + 2, 0);
    memcpy(body + 4, source_digest, 32);
    fp32(body + 36, SERVER_TOKEN);
    if (cfg.wrong_digest) {
        body[4] ^= 0xff;
    }
    if (cfg.sign) {
        memcpy(body + MKNB_OFFER_BODY, TEST_OFFER_SIGNATURE, 64);
        body[MKNB_OFFER_BODY] ^= cfg.spoil_signature ? 0x80u : 0u;
        size = MKNB_OFFER_SIGNED;
    }
    enqueue(f, server_message(f, MKNB_OFFER, len, body, size, client_port, cfg.ip));
}

/* True when this block's transmission is the one to lose. */
static bool lose_block(uint32_t index) {
    return cfg.drop_all || (cfg.drop_first != 0 && index % cfg.drop_first == 0 && attempts[index] == 1u);
}

/* Sends the requested window, honouring the configured misbehaviour. */
static void answer_read(uint16_t client_port, uint32_t base, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint8_t f[ETH_FRAME_MAX], block[MKNB_BLOCK];
        uint32_t index = base + i, offset = index * MKNB_BLOCK;
        uint32_t n = IMAGE_BYTES - offset < MKNB_BLOCK ? IMAGE_BYTES - offset : MKNB_BLOCK;
        data_sent++;
        if (index < 64u) {
            attempts[index]++;
        }
        if (offset >= IMAGE_BYTES || lose_block(index)) {
            continue;
        }
        memcpy(block, source + offset, n);
        if (cfg.corrupt_block == index + 1u) {
            block[0] ^= 0xff;
        }
        enqueue(f, server_message(f, MKNB_DATA, index, block, n,
                                  client_port, cfg.spoof_from_other ? OTHER_IP : cfg.ip));
    }
}

/* The netdev: records the frame, then plays the server's part — including
 * answering ARP, as the real host does before a unicast READ can go out. */
static int wire_send(void *self, const uint8_t *frame, uint32_t len) {
    uint16_t client_port = fg16(frame + F_UDP);
    const uint8_t *m = frame + F_DATA;
    uint8_t reply[64];
    capture_send(self, frame, len);
    if (frame_is_arp_request(frame, len, cfg.ip) && !cfg.silent) {
        enqueue(reply, frame_arp_reply(reply, frame, cfg.ip, server_mac));
        return KERR_OK;
    }
    if (!frame_is_udp(frame, len, MKNB_PORT) || len < F_DATA + MKNB_HEADER) {
        return KERR_OK;
    }
    if (m[5] == MKNB_DISCOVER) {
        discovers_seen++;
        probed_hosts = fg32(frame + F_IP + 16) == cfg.ip ? probed_hosts + 1u : probed_hosts;
        if (!cfg.silent && fg32(frame + F_IP + 16) == cfg.ip) {
            answer_discover(client_port);
        }
    } else if (m[5] == MKNB_READ && fg32(frame + F_IP + 16) == cfg.ip) {
        reads_seen++;
        token_seen = fg32(m + MKNB_HEADER + 4);
        if (token_seen == SERVER_TOKEN) {  /* a real server serves no one else */
            answer_read(client_port, fg32(m + 12), fg16(m + MKNB_HEADER));
        }
    } else if (m[5] == MKNB_DONE) {
        done_seen++;
    }
    return KERR_OK;
}

/* Delivers what the server queued, then lets the client act again. */
static void pump(uint64_t now_ms) {
    for (uint32_t guard = 0; guard < 64 && queue.next < queue.n; guard++) {
        uint32_t i = queue.next++;
        net_input(&nif, queue.frame[i], queue.len[i]);
    }
    queue.n = queue.next = 0;
    netboot_tick(&nb, now_ms);
}

/* Runs the session to completion (or the deadline), 10 ms at a time. */
static void run(uint64_t limit_ms) {
    for (uint64_t t = 0; t <= limit_ms && !netboot_finished(&nb); t += 10) {
        nif.now_ms = t;
        netboot_tick(&nb, t);
        pump(t);
    }
}

static void fresh(ip4_t mask) {
    struct netdev_port dev = {.send = wire_send, .self = &cap};
    struct net_events ev = {0};
    memset(&cap, 0, sizeof cap);
    memset(&queue, 0, sizeof queue);
    memset(bitmap, 0, sizeof bitmap);
    memset(image_buf, 0, sizeof image_buf);
    data_sent = discovers_seen = probed_hosts = done_seen = reads_seen = token_seen = 0;
    memset(attempts, 0, sizeof attempts);
    cfg = (struct server_config){.ip = SERVER_IP};
    net_init(&nif, client_mac, &dev, &ev);
    nif.ip = CLIENT_IP;
    nif.mask = mask;
    nif.gateway = SERVER_IP;
    for (uint32_t i = 0; i < IMAGE_BYTES; i++) {
        source[i] = (uint8_t)(i * 31u + 7u);
    }
    sha256(source, IMAGE_BYTES, source_digest);
}

static int start(ip4_t pinned) {
    return netboot_start(&nb, &nif, image_buf, CAPACITY, bitmap, 0xa1b2c3d4u, client_mac, board_id, pinned);
}

static void test_sweep_fetches_and_verifies(void) {
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    run(2000);
    KTEST_CHECK(nb.state == NETBOOT_READY && nb.error == NETBOOT_ERR_NONE);
    KTEST_CHECK(nb.image_len == IMAGE_BYTES && memcmp(image_buf, source, IMAGE_BYTES) == 0);
    KTEST_CHECK(nb.have == 5 && done_seen == 1);
    KTEST_CHECK(nb.token == SERVER_TOKEN && token_seen == SERVER_TOKEN);  /* echoed in every READ */
    KTEST_CHECK(reads_seen >= 1);
}

/* The index of the first captured MKNB datagram, or cap.n when there is
 * none (the gateway's ARP request comes first). */
static uint32_t first_mknb(void) {
    for (uint32_t i = 0; i < cap.n; i++) {
        if (frame_is_udp(cap.frame[i], cap.len[i], MKNB_PORT)) {
            return i;
        }
    }
    return cap.n;
}

/* The gateway is probed on its own first, so a server there is found
 * before the sweep reaches anyone else. */
static void test_gateway_is_probed_first(void) {
    uint32_t i;
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    nif.now_ms = 0;
    netboot_tick(&nb, 0);
    i = first_mknb();
    KTEST_CHECK(probed_hosts == 1 && i < cap.n);
    KTEST_CHECK(fg32(cap.frame[i] + F_IP + 16) == SERVER_IP);
    KTEST_CHECK(fg16(cap.frame[i] + F_UDP + 2) == MKNB_PORT && cap.len[i] == F_DATA + MKNB_HEADER + 22u);
    KTEST_CHECK(memcmp(cap.frame[i] + F_DATA + MKNB_HEADER, client_mac, 6) == 0);
    KTEST_CHECK(memcmp(cap.frame[i] + F_DATA + MKNB_HEADER + 6, board_id, MKNB_BOARD_ID) == 0);
    /* The gateway's MAC is unknown at this point, so the frame went to the
     * broadcast address rather than waiting for ARP. */
    KTEST_CHECK(memcmp(cap.frame[i], "\xff\xff\xff\xff\xff\xff", 6) == 0);
}

/* An ARP request for the gateway goes out at once, so that its probe (and
 * the READs that follow an offer from it) can be addressed properly. */
static void test_gateway_arp_is_requested(void) {
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    KTEST_CHECK(cap.n >= 1 && frame_is_arp_request(cap.frame[0], cap.len[0], SERVER_IP));
}

/* A host that answers from inside the sweep must not beat the gateway:
 * during the grace period nobody else is even probed. */
static void test_gateway_wins_the_race(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.ip = OTHER_IP;  /* the only server is elsewhere */
    KTEST_CHECK(start(0) == KERR_OK);
    for (uint64_t t = 0; t < MKNB_GATEWAY_GRACE_MS; t += 10) {
        nif.now_ms = t;
        netboot_tick(&nb, t);
        pump(t);
    }
    KTEST_CHECK(nb.state == NETBOOT_SWEEPING && probed_hosts == 0);  /* OTHER_IP not probed yet */
    KTEST_CHECK(discovers_seen > 0);  /* the gateway was, repeatedly */
    run(3000);
    KTEST_CHECK(nb.state == NETBOOT_READY && nb.server == OTHER_IP);  /* found after the grace period */
}

static void test_pinned_server_skips_the_sweep(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.ip = OTHER_IP;
    KTEST_CHECK(start(OTHER_IP) == KERR_OK);
    run(2000);
    KTEST_CHECK(nb.state == NETBOOT_READY && discovers_seen == 1);
    KTEST_CHECK(memcmp(image_buf, source, IMAGE_BYTES) == 0);
}

/* A server that is not the gateway is found later in the sweep. */
static void test_sweep_reaches_other_hosts(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.ip = OTHER_IP;
    KTEST_CHECK(start(0) == KERR_OK);
    run(2000);
    KTEST_CHECK(nb.state == NETBOOT_READY && nb.server == OTHER_IP);
    KTEST_CHECK(discovers_seen > 1 && memcmp(image_buf, source, IMAGE_BYTES) == 0);
}

/* A /16 is not walked: the sweep stops at MKNB_SWEEP_MAX hosts per round. */
static void test_large_subnet_is_bounded(void) {
    fresh(IP4(255, 255, 0, 0));
    cfg.silent = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(MKNB_DEADLINE_MS + 100u);
    KTEST_CHECK(nb.state == NETBOOT_FAILED && nb.error == NETBOOT_ERR_NO_SERVER);
    /* Bounded by the rounds, plus the gateway re-probes during its grace
     * period: nothing like the 65534 hosts a /16 holds. */
    KTEST_CHECK(nb.stats.probes_sent <= MKNB_SWEEP_MAX * MKNB_SWEEP_ROUNDS + 64u);
}

static void test_no_server_fails_cleanly(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.silent = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(MKNB_DEADLINE_MS + 100u);
    KTEST_CHECK(nb.state == NETBOOT_FAILED && nb.error == NETBOOT_ERR_NO_SERVER);
    KTEST_CHECK(nb.have == 0);
}

/* Every third block is lost once: the window is re-requested until whole. */
static void test_lost_blocks_are_refetched(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.drop_first = 3;
    KTEST_CHECK(start(0) == KERR_OK);
    run(5000);
    KTEST_CHECK(nb.state == NETBOOT_READY && memcmp(image_buf, source, IMAGE_BYTES) == 0);
    KTEST_CHECK(nb.stats.reads_sent > 1);
}

static void test_duplicates_are_ignored(void) {
    uint8_t f[ETH_FRAME_MAX];
    uint32_t len;
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    run(2000);
    KTEST_CHECK(nb.state == NETBOOT_READY);
    len = server_message(f, MKNB_DATA, 0, source, MKNB_BLOCK, nb.port, SERVER_IP);
    net_input(&nif, f, len);
    KTEST_CHECK(nb.have == 5);  /* finished sessions ignore everything */
}

static void test_digest_mismatch_is_refused(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.wrong_digest = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(5000);
    KTEST_CHECK(nb.state == NETBOOT_FAILED && nb.error == NETBOOT_ERR_DIGEST);
}

/* A block arriving with the wrong bytes also fails the digest: a corrupt
 * transfer is never mounted. */
static void test_corrupt_block_fails_the_digest(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.corrupt_block = 3;
    KTEST_CHECK(start(0) == KERR_OK);
    run(5000);
    KTEST_CHECK(nb.state == NETBOOT_FAILED && nb.error == NETBOOT_ERR_DIGEST);
}

static void test_oversized_offer_is_refused(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.claim_len = CAPACITY + 1u;
    KTEST_CHECK(start(0) == KERR_OK);
    run(2000);
    KTEST_CHECK(nb.state == NETBOOT_FAILED && nb.error == NETBOOT_ERR_TOO_BIG);
    KTEST_CHECK(nb.stats.offers_refused == 1);
}

static void test_bad_block_size_is_refused(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.bad_block_size = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(1000);
    KTEST_CHECK(nb.state != NETBOOT_READY && nb.stats.offers_refused > 0);
}

/* Blocks from an address that is not the chosen server are dropped. */
static void test_data_from_another_host_is_refused(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.spoof_from_other = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(1000);
    KTEST_CHECK(nb.state == NETBOOT_FETCHING && nb.have == 0);
    KTEST_CHECK(nb.stats.blocks_refused > 0);
}

/* A signature is the thing a pinned digest is not: it says *who* is
 * offering, so the server can change what it serves and nothing else on
 * the subnet can make an offer this board will look at. */
static void test_a_signed_offer_is_accepted(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.sign = true;
    KTEST_CHECK(start(0) == KERR_OK);
    netboot_require_signature(&nb, TEST_PUBLIC_KEY);
    run(5000);
    KTEST_CHECK(nb.state == NETBOOT_READY);
    KTEST_CHECK(nb.stats.offers_refused == 0);
}

/* The attack: a machine that answers the sweep without the private key.
 * Refused at the offer, so the sweep carries on and it gains nothing. */
static void test_an_unsigned_offer_is_refused(void) {
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    netboot_require_signature(&nb, TEST_PUBLIC_KEY);
    run(5000);
    KTEST_CHECK(nb.state != NETBOOT_READY);
    KTEST_CHECK(nb.have == 0 && nb.stats.offers_seen == 0);
    KTEST_CHECK(nb.stats.offers_refused > 0);
}

/* A signature that is there but wrong, which is the same answer. */
static void test_a_spoiled_signature_is_refused(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.sign = true;
    cfg.spoil_signature = true;
    KTEST_CHECK(start(0) == KERR_OK);
    netboot_require_signature(&nb, TEST_PUBLIC_KEY);
    run(5000);
    KTEST_CHECK(nb.state != NETBOOT_READY);
    KTEST_CHECK(nb.stats.offers_refused > 0);
}

/* And a signed offer is still accepted by a board that was not told to
 * expect one: adding the signature does not break an older board. */
static void test_a_signature_is_ignored_when_not_required(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.sign = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(5000);
    KTEST_CHECK(nb.state == NETBOOT_READY);
}

/* With a digest pinned, only the image that matches it is accepted — and the
 * refusal happens at the **offer**, before a byte is transferred. */
static void test_a_pinned_digest_accepts_the_right_image(void) {
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    netboot_expect_digest(&nb, source_digest); /* after the start: it clears the session */
    run(5000);
    KTEST_CHECK(nb.state == NETBOOT_READY);
    KTEST_CHECK(nb.stats.offers_refused == 0);
}

/* The attack the pin exists for: a machine that answers the sweep first and
 * offers something else. Its offer is refused, nothing is fetched from it,
 * and the client stays in the sweep rather than failing — so answering
 * first buys an impostor nothing, not even a denial of service. */
static void test_a_pinned_digest_refuses_another_image(void) {
    uint8_t other[32];
    fresh(IP4(255, 255, 255, 0));
    memcpy(other, source_digest, sizeof other);
    other[0] ^= 0xff;
    KTEST_CHECK(start(0) == KERR_OK);
    netboot_expect_digest(&nb, other);
    run(5000);
    KTEST_CHECK(nb.state != NETBOOT_READY);
    KTEST_CHECK(nb.have == 0);
    KTEST_CHECK(nb.stats.offers_refused > 0);
    KTEST_CHECK(nb.stats.offers_seen == 0); /* it never became the server */
}

/* Takes the real offer, then checks what the client refuses afterwards. */
static void offered(void) {
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(start(0) == KERR_OK);
    cfg.drop_all = true;  /* the server answers READ with nothing */
    nif.now_ms = 0;
    netboot_tick(&nb, 0);
    pump(0);
    KTEST_CHECK(nb.state == NETBOOT_FETCHING && nb.have == 0);
}

static void test_hostile_datagrams(void) {
    uint8_t f[ETH_FRAME_MAX], block[MKNB_BLOCK];
    uint32_t len;
    offered();
    memcpy(block, source, MKNB_BLOCK);
    /* a block past the end of the image */
    net_input(&nif, f, server_message(f, MKNB_DATA, 99999u, block, MKNB_BLOCK, nb.port, SERVER_IP));
    /* the last block's index with a full-length payload */
    net_input(&nif, f, server_message(f, MKNB_DATA, 4, block, MKNB_BLOCK, nb.port, SERVER_IP));
    /* a full block's index with a short payload */
    net_input(&nif, f, server_message(f, MKNB_DATA, 1, block, 8, nb.port, SERVER_IP));
    KTEST_CHECK(nb.have == 0 && nb.stats.blocks_refused == 3);
    /* a header that is not ours */
    len = server_message(f, MKNB_DATA, 0, block, MKNB_BLOCK, nb.port, SERVER_IP);
    f[F_DATA] ^= 0xff;  /* magic */
    net_input(&nif, f, len);
    len = server_message(f, MKNB_DATA, 0, block, MKNB_BLOCK, nb.port, SERVER_IP);
    f[F_DATA + 4] = 9;  /* version */
    net_input(&nif, f, len);
    len = server_message(f, MKNB_DATA, 0, block, MKNB_BLOCK, nb.port, SERVER_IP);
    f[F_DATA + 8] ^= 0xff;  /* xid */
    net_input(&nif, f, len);
    KTEST_CHECK(nb.have == 0 && nb.stats.blocks_refused == 3);  /* dropped before the type */
    /* a second OFFER cannot move the server */
    answer_discover(nb.port);
    pump(10);
    KTEST_CHECK(nb.server == SERVER_IP && nb.stats.offers_refused == 1);
    /* a truncated datagram */
    net_input(&nif, f, frame_udp(f, client_mac, server_mac, SERVER_IP, CLIENT_IP, MKNB_PORT, nb.port, block, 4));
    KTEST_CHECK(nb.have == 0 && nb.state == NETBOOT_FETCHING);
}

/* A server that offers an image and then sends nothing is abandoned, and
 * the sweep resumes: a rogue first responder cannot spend the whole boot.
 * (This is what a NAT that invents an unreachable address looks like.) */
static void test_dead_server_is_abandoned(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.drop_all = true;
    KTEST_CHECK(start(0) == KERR_OK);
    run(6000);
    /* It keeps cycling — offer, stall, abandon, sweep again — instead of
     * spending the whole deadline on one server that cannot deliver. */
    KTEST_CHECK(nb.stats.servers_abandoned >= 2);
    KTEST_CHECK(nb.have == 0);
}

/* After abandoning a silent server, a working one is still used. */
static void test_second_server_is_used(void) {
    fresh(IP4(255, 255, 255, 0));
    cfg.drop_all = true;
    KTEST_CHECK(start(0) == KERR_OK);
    while (nb.stats.servers_abandoned == 0 && !netboot_finished(&nb)) {
        nif.now_ms += 10;
        netboot_tick(&nb, nif.now_ms);
        pump(nif.now_ms);
    }
    KTEST_CHECK(nb.stats.servers_abandoned == 1);
    cfg.drop_all = false;  /* the next server answers properly */
    run(nif.now_ms + 6000);
    KTEST_CHECK(nb.state == NETBOOT_READY && memcmp(image_buf, source, IMAGE_BYTES) == 0);
}

/* An ERROR from the server ends the session; one from elsewhere does not. */
static void test_server_error(void) {
    uint8_t f[ETH_FRAME_MAX];
    offered();
    net_input(&nif, f, server_message(f, MKNB_ERROR, 1, "no image", 8, nb.port, OTHER_IP));
    KTEST_CHECK(nb.state == NETBOOT_FETCHING);
    net_input(&nif, f, server_message(f, MKNB_ERROR, 1, "no image", 8, nb.port, SERVER_IP));
    KTEST_CHECK(nb.state == NETBOOT_FAILED && nb.error == NETBOOT_ERR_SERVER);
}

/* With no server left to find, a stalled transfer ends at the deadline. */
static void test_stalled_transfer_times_out(void) {
    offered();
    cfg.silent = true;  /* nothing answers the sweep that follows either */
    run(MKNB_DEADLINE_MS + 100u);
    KTEST_CHECK(nb.state == NETBOOT_FAILED);
    KTEST_CHECK(nb.error == NETBOOT_ERR_TIMEOUT || nb.error == NETBOOT_ERR_NO_SERVER);
    KTEST_CHECK(nb.stats.reads_sent >= MKNB_STALL_LIMIT);  /* it kept asking before giving up */
}

static void test_start_rejects_bad_setup(void) {
    fresh(IP4(255, 255, 255, 0));
    nif.ip = 0;
    KTEST_CHECK(start(0) == KERR_INVAL);  /* no lease yet */
    fresh(IP4(255, 255, 255, 0));
    KTEST_CHECK(netboot_start(&nb, &nif, NULL, CAPACITY, bitmap, 1, client_mac, board_id, 0) == KERR_INVAL);
    KTEST_CHECK(netboot_start(&nb, &nif, image_buf, 10, bitmap, 1, client_mac, board_id, 0) == KERR_INVAL);
}

/* Mutated server traffic: nothing may escape the image buffer (ASan). */
static void test_fuzz_server_traffic(void) {
    uint8_t f[ETH_FRAME_MAX], block[MKNB_BLOCK];
    uint64_t x = 0x243f6a8885a308d3ull;
    memcpy(block, source, MKNB_BLOCK);
    for (int round = 0; round < 4000; round++) {
        uint32_t len;
        if (round % 50 == 0) {
            offered();
        }
        x ^= x << 13, x ^= x >> 7, x ^= x << 17;
        len = server_message(f, (uint8_t)(x % 8u), (uint32_t)(x >> 8), block, (uint32_t)(x >> 40) % MKNB_BLOCK,
                             nb.port, (x & 1) ? SERVER_IP : OTHER_IP);
        f[F_DATA + 12u + (x >> 3) % 4u] ^= (uint8_t)(x >> 24);
        net_input(&nif, f, len);
    }
    KTEST_CHECK(nb.state == NETBOOT_FETCHING || netboot_finished(&nb));
}

int main(void) {
    KTEST_RUN(test_sweep_fetches_and_verifies);
    KTEST_RUN(test_gateway_is_probed_first);
    KTEST_RUN(test_gateway_arp_is_requested);
    KTEST_RUN(test_gateway_wins_the_race);
    KTEST_RUN(test_pinned_server_skips_the_sweep);
    KTEST_RUN(test_sweep_reaches_other_hosts);
    KTEST_RUN(test_large_subnet_is_bounded);
    KTEST_RUN(test_no_server_fails_cleanly);
    KTEST_RUN(test_lost_blocks_are_refetched);
    KTEST_RUN(test_duplicates_are_ignored);
    KTEST_RUN(test_digest_mismatch_is_refused);
    KTEST_RUN(test_a_signed_offer_is_accepted);
    KTEST_RUN(test_an_unsigned_offer_is_refused);
    KTEST_RUN(test_a_spoiled_signature_is_refused);
    KTEST_RUN(test_a_signature_is_ignored_when_not_required);
    KTEST_RUN(test_a_pinned_digest_accepts_the_right_image);
    KTEST_RUN(test_a_pinned_digest_refuses_another_image);
    KTEST_RUN(test_corrupt_block_fails_the_digest);
    KTEST_RUN(test_oversized_offer_is_refused);
    KTEST_RUN(test_bad_block_size_is_refused);
    KTEST_RUN(test_data_from_another_host_is_refused);
    KTEST_RUN(test_hostile_datagrams);
    KTEST_RUN(test_dead_server_is_abandoned);
    KTEST_RUN(test_second_server_is_used);
    KTEST_RUN(test_server_error);
    KTEST_RUN(test_stalled_transfer_times_out);
    KTEST_RUN(test_start_rejects_bad_setup);
    KTEST_RUN(test_fuzz_server_traffic);
    return ktest_failures();
}
