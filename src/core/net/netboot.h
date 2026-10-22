/* netboot — the MKNB client: finds a boot server by sweeping the subnet
 * with unicast UDP probes, fetches the root filesystem in windowed blocks,
 * and verifies it against the SHA-256 the server offered. The wire format
 * and the reasoning are in NETBOOT.md.
 *
 * Pure: it is driven by netboot_tick and by datagrams, holds no allocation
 * of its own (the image buffer and the block bitmap are the caller's), and
 * is host-tested against a fake server and against hostile datagrams. */
#ifndef MINIKERN_CORE_NET_NETBOOT_H
#define MINIKERN_CORE_NET_NETBOOT_H

#include <stdbool.h>
#include <stdint.h>

#include "core/net/net.h"

#define MKNB_PORT        7634u
#define MKNB_MAGIC       0x4d4b4e42u  /* "MKNB" */
#define MKNB_VERSION     1u
#define MKNB_HEADER      16u
#define MKNB_BLOCK       1024u
/* Blocks in flight. The server sends a whole window as fast as it can, so
 * this must stay well inside the receive ring of the slowest board that
 * boots this way (the H3's EMAC ring holds 64 frames): a larger window
 * overruns the ring and most of the burst is lost. */
#define MKNB_WINDOW      16u
#define MKNB_SWEEP_BURST 16u    /* probes per tick, so a /24 takes ~160 ms */
#define MKNB_SWEEP_MAX   1024u  /* the largest subnet we will walk */
#define MKNB_BOARD_ID    16u
#define MKNB_OFFER_BODY  40u    /* block size, reserved, digest, token */
/* A signed offer carries 64 more bytes: an Ed25519 signature over the
 * session's id, the image's length and its digest — the three things that
 * decide what this board will run. A server that does not sign is still
 * understood, and still refused when a key has been configured. */
#define MKNB_OFFER_SIGNED (MKNB_OFFER_BODY + 64u)
#define MKNB_SIGNED_BYTES 40u   /* xid, length, digest: what the signature covers */
#define MKNB_READ_BODY   8u     /* block count, reserved, token */
#define MKNB_SWEEP_ROUNDS 3u    /* sweeps before giving up */
#define MKNB_GATEWAY_GRACE_MS 400u /* the gateway's head start before the sweep */
#define MKNB_STALL_LIMIT 8u    /* fruitless windows before a server is abandoned */
#define MKNB_STALL_MS    120u   /* silence before a window is re-requested */
#define MKNB_DEADLINE_MS 30000u /* the whole session */

enum mknb_type {
    MKNB_DISCOVER = 1,
    MKNB_OFFER = 2,
    MKNB_READ = 3,
    MKNB_DATA = 4,
    MKNB_DONE = 5,
    MKNB_ERROR = 6,
};

enum netboot_state {
    NETBOOT_IDLE = 0,
    NETBOOT_SWEEPING,   /* probing addresses, waiting for an OFFER */
    NETBOOT_FETCHING,
    NETBOOT_READY,      /* the image is complete and its digest matched */
    NETBOOT_FAILED,
};

/* Why a session failed, for the log. */
enum netboot_error {
    NETBOOT_ERR_NONE = 0,
    NETBOOT_ERR_NO_SERVER,   /* nobody answered the sweep */
    NETBOOT_ERR_TOO_BIG,     /* the image does not fit the buffer we reserved */
    NETBOOT_ERR_DIGEST,      /* it arrived whole and hashed differently */
    NETBOOT_ERR_TIMEOUT,     /* the transfer stalled past the deadline */
    NETBOOT_ERR_SERVER,      /* the server said ERROR */
};

struct netboot_stats {
    uint32_t probes_sent;
    uint32_t offers_seen;
    uint32_t offers_refused;
    uint32_t reads_sent;
    uint32_t blocks_stored;
    uint32_t blocks_duplicate;
    uint32_t blocks_refused;
    uint32_t servers_abandoned;
    uint32_t send_errors;   /* sends the device or the stack refused */
    int last_send_rc;       /* the most recent non-zero send result */
};

struct netboot {
    struct netif *nif;
    enum netboot_state state;
    enum netboot_error error;
    uint32_t xid;
    uint16_t port;              /* our ephemeral port */
    ip4_t server;               /* 0 until an OFFER is taken */
    ip4_t pinned;               /* minikern.netboot=<ip>: no sweep at all */
    uint32_t token;             /* the server's session token, echoed in every READ */
    uint8_t mac[ETH_ALEN];
    uint8_t board_id[MKNB_BOARD_ID];
    uint32_t sweep_host;        /* next host number in the subnet */
    uint32_t sweep_hosts;       /* how many there are to try */
    uint32_t sweep_round;
    uint64_t gateway_until_ms;  /* while now is below this, only the gateway is probed */
    uint32_t stalls;            /* consecutive windows that brought nothing */
    uint32_t abandoned;         /* servers given up on */
    uint8_t *image;             /* the caller's buffer */
    uint32_t capacity;
    uint32_t image_len;
    uint32_t blocks;
    uint32_t have;
    uint8_t digest[32];
    uint8_t expected[32];       /* the only image this board will accept */
    bool pinned_digest;         /* ...when one was given */
    uint8_t key[32];            /* the public key an offer must be signed with */
    bool require_signature;     /* ...when one was given */
    uint32_t *bitmap;           /* one bit per block, the caller's */
    uint32_t window_base;
    uint32_t window_end;
    uint64_t now_ms;
    uint64_t stall_at_ms;
    uint64_t deadline_ms;
    struct netboot_stats stats;
};

/* Bitmap words needed for an image buffer of capacity bytes. */
#define NETBOOT_BITMAP_WORDS(capacity) ((((capacity) / MKNB_BLOCK + 1u) + 31u) / 32u)

/* Starts a session on nif, which must already have a lease. image and
 * bitmap are the caller's, sized for capacity bytes. pinned, when not 0,
 * is the only address probed. Returns KERR_OK, or KERR_INVAL / the bind
 * error. */
int netboot_start(struct netboot *nb, struct netif *nif, uint8_t *image, uint32_t capacity, uint32_t *bitmap,
                  uint32_t xid, const uint8_t mac[ETH_ALEN], const uint8_t board_id[MKNB_BOARD_ID], ip4_t pinned);

/* Accepts only an image with this digest.
 *
 * The digest in an offer proves the *transfer*, not the server: it is the
 * server's own claim about what it is about to send, so a machine that
 * answers the sweep first decides what the board runs. Pinning the digest
 * the board expects closes that, and closes it **at the offer** rather than
 * after the transfer — so a rogue server cannot deny service either; the
 * board refuses its offer and goes on sweeping for one it will accept.
 *
 * What this is not is a signature: it pins one image rather than
 * authenticating a server, so a new root filesystem means a new pin. An
 * Ed25519 signature over the offer is the next step and would let the
 * server change what it serves (TASKS.md P5.5).
 *
 * **Called after `netboot_start`**, which begins a session by clearing it. */
void netboot_expect_digest(struct netboot *nb, const uint8_t digest[32]);

/* Accepts only offers signed by the holder of this public key.
 *
 * This is the thing a pinned digest is not: it authenticates the **server**
 * rather than one image, so the server can change what it serves without
 * every board being reconfigured, and nothing else on the subnet can make
 * an offer this board will look at. The signature covers the session's own
 * id, so a recorded offer cannot be replayed into a later boot.
 *
 * Called after `netboot_start`, which begins a session by clearing it. */
void netboot_require_signature(struct netboot *nb, const uint8_t key[32]);

/* Drives the sweep, the window re-requests and the deadline. */
void netboot_tick(struct netboot *nb, uint64_t now_ms);

/* The handler to bind on our port (netboot_start does it). */
struct udp_handler netboot_handler(struct netboot *nb);

/* True once the session has ended, either way. */
static inline bool netboot_finished(const struct netboot *nb) {
    return nb->state == NETBOOT_READY || nb->state == NETBOOT_FAILED;
}

/* Releases the port. */
void netboot_stop(struct netboot *nb);

/* A one-line reason for the log. */
const char *netboot_error_text(enum netboot_error e);

#endif
