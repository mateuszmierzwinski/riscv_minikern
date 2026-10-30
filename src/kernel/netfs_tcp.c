#include "kernel/netfs_tcp.h"
#include "kernel/klock.h"

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/net/net.h"
#include "core/net/tcp.h"
#include "core/netfs.h"
#include "kernel/net.h"
#include "kernel/netfsmount.h"
#include "kernel/sched.h"

/* How long one send, receive or connect may take before the call fails:
 * a server that stops answering must cost a request, not the kernel. */
#define NETFS_TIMEOUT_NS 2000000000ull

/* How long a thread sleeps before looking again. The connection's notify
 * does not fire when an ACK frees send space, so a sender waiting only on
 * it would sleep out the whole timeout. */
#define NETFS_SLICE_NS 1000000ull

/* One connection, re-made on the next call after it breaks. */
struct link {
    struct tcp_conn *c;
    ip4_t ip;
    uint16_t port;
    bool busy;
    bool took_klock; /* the boot path's: lock() took the kernel lock for it */
};

static struct link link;

static void report_trail(const struct tcp_conn *c);
/* Whether `client` is set up, and for which server. */
static bool attached;
/* The board's receive poll: at boot nothing else drains the ring. */
static void (*poll_receive)(void);
static struct netfs client;
static uint8_t frame[NETFS_FRAME];

static void on_change(void *self, struct tcp_conn *c) {
    (void)c;
    (void)sched_wake_on(self, ~0u);
}

/* Waits for anything to happen on the link, or the deadline. A thread
 * sleeps. The boot path, which has no thread, **polls**: it opens
 * interrupts for the receive path and drives the stack's timers itself —
 * a `wfi` there sleeps for ever once the peer goes quiet, because at boot
 * nothing else raises an interrupt and the queued request is never sent. */
static void wait_a_moment(uint64_t deadline_ns) {
    uint64_t slice = timer_ns() + NETFS_SLICE_NS;
    if (sched_wait_on(&link, slice < deadline_ns ? slice : deadline_ns) != KERR_INVAL) {
        /* **The receive ring is drained and the stack's timers run here
         * too.** Both otherwise happen on the housekeeping beat, which
         * dynamic ticks space a second or more apart while this thread
         * sleeps, and the GMAC's receive interrupt is sometimes missed —
         * so a reply sat in the ring, or a 200 ms retransmit waited, for a
         * beat: every stall `bulkgo` measured was a multiple of one. */
        if (poll_receive != NULL) {
            poll_receive();
        }
        knet_tick();
        return;
    }
    __asm__ volatile("csrsi sstatus, 2\n\tnop\n\tcsrci sstatus, 2" : : : "memory");
    if (poll_receive != NULL) {
        poll_receive();
    }
    knet_tick();
}

static void drop(void) {
    if (link.c != NULL) {
        tcp_free(link.c);
        link.c = NULL;
    }
}

static int connect_once(void) {
    uint64_t deadline = timer_ns() + NETFS_TIMEOUT_NS;
    int rc;
    link.c = tcp_alloc(knet_tcp());
    if (link.c == NULL) {
        return KERR_NOMEM;
    }
    link.c->notify = on_change;
    link.c->notify_self = &link;
    rc = tcp_connect(link.c, link.ip, link.port);
    while (rc == KERR_OK && !tcp_connect_done(link.c) && timer_ns() < deadline) {
        wait_a_moment(deadline);
    }
    if (rc == KERR_OK && link.c->state != TCP_ESTABLISHED) {
        rc = link.c->error != 0 ? link.c->error : KERR_TIMEDOUT;
    }
    if (rc != KERR_OK) {
        drop();
    }
    return rc;
}

/* A few attempts: a reset can answer the first SYN when the server still
 * holds a connection from before this boot. */
static int connect_link(void) {
    int rc = KERR_IO;
    for (uint32_t attempt = 0; attempt < 3u && rc != KERR_OK; attempt++) {
        rc = connect_once();
    }
    return rc;
}

static int tx(void *self, const void *buf, uint32_t n) {
    uint64_t deadline = timer_ns() + NETFS_TIMEOUT_NS;
    uint32_t done = 0;
    (void)self;
    if (link.c == NULL && connect_link() != KERR_OK) {
        return KERR_IO;
    }
    while (done < n) {
        int32_t sent = tcp_send(link.c, (const uint8_t *)buf + done, n - done);
        if (sent < 0 || timer_ns() >= deadline) {
            drop();
            return sent < 0 ? sent : KERR_TIMEDOUT;
        }
        done += (uint32_t)sent;
        if (done < n) {
            wait_a_moment(deadline);
        }
    }
    return KERR_OK;
}

/* **A frame header that is not "MKFS"**: the byte stream is out of step,
 * which happened once in ~20 bulk loads and could not be reproduced. Said
 * with the stack's counters, so the next one names its cause. */
static void report_bad_header(const uint8_t *b) {
    kprintf("netfs: a reply broke the framing: %02x %02x %02x %02x %02x %02x %02x %02x\n", b[0],
            b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    knet_report();
    report_trail(link.c);
}

static int rx(void *self, void *buf, uint32_t n) {
    uint64_t deadline = timer_ns() + NETFS_TIMEOUT_NS;
    uint32_t done = 0;
    (void)self;
    while (done < n && link.c != NULL) {
        int32_t got = tcp_recv(link.c, (uint8_t *)buf + done, n - done, false);
        if (got > 0) {
            done += (uint32_t)got;
            continue;
        }
        if (got != KERR_AGAIN || timer_ns() >= deadline) {
            if (got == KERR_AGAIN) {
                kprintf("netfs: no reply after %u of %u bytes; the link's counters:\n", done, n);
                knet_report();
                report_trail(link.c);
            }
            drop(); /* end of stream, an error, or silence */
            return got == 0 || got == KERR_AGAIN ? KERR_IO : got;
        }
        wait_a_moment(deadline);
    }
    return done == n ? KERR_OK : KERR_IO;
}

/* The connection's last received segments, oldest first: what a transfer
 * that stopped was last handed, and what the stack did with each. */
static void report_trail(const struct tcp_conn *c) {
    uint32_t n;
    if (c == NULL) {
        return;
    }
    n = c->trail_at < TCP_TRAIL ? c->trail_at : TCP_TRAIL;
    kprintf("netfs: rcv_nxt %u rx_len %u; last %u segments (offset from rcv_nxt, length, "
            "flags, verdict, window):\n", c->rcv_nxt, c->rx_len, n);
    for (uint32_t i = c->trail_at - n; i != c->trail_at; i++) {
        const struct tcp_trail *t = &c->trail[i % TCP_TRAIL];
        kprintf("netfs:   %d %u %02x %c %u took %u\n", t->off, t->len, t->flags, t->verdict,
                t->wnd, t->took);
    }
}

/* ── the two framings ──────────────────────────────────────────────────── */

#define FRAME_HEADER 8u

static void put_le32(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* "MKFS" u32 | length u32 | message, both ways, little-endian. */
static int framed_exchange(void *self, uint8_t *buf, uint32_t req_len, uint32_t cap,
                           uint32_t *resp_len) {
    uint8_t h[FRAME_HEADER];
    int rc;
    put_le32(h, NETFS_MAGIC);
    put_le32(h + 4, req_len);
    rc = tx(self, h, sizeof h);
    rc = rc != KERR_OK ? rc : tx(self, buf, req_len);
    rc = rc != KERR_OK ? rc : rx(self, h, sizeof h);
    if (rc != KERR_OK) {
        return rc;
    }
    *resp_len = get_le32(h + 4);
    if (get_le32(h) != NETFS_MAGIC || *resp_len > cap) {
        report_bad_header(h);
        return KERR_PROTO;
    }
    return *resp_len == 0 ? KERR_OK : rx(self, buf, *resp_len);
}

/* One caller at a time on the link: a thread asleep in rx must not have
 * another's request written into the middle of its reply.
 *
 * **And the kernel lock, on the boot path.** The network stack belongs to
 * whoever holds it: a thread in the kernel does, and so does the
 * housekeeping beat that polls the ring on any hart. The boot hart's loads
 * over netfs (the first program, before the scheduler) ran without it, so
 * another hart's beat delivered frames into the same connection at the same
 * moment — measured: "1 overlapping polls", a segment 2920 bytes early
 * recorded as taken, a reply that broke the framing, and a receive DMA left
 * suspended. Taken here when this hart does not hold it; its own interrupts
 * re-enter it as a holder. */
static void lock(void *self) {
    (void)self;
    if (!klock_held()) {
        link.took_klock = klock_enter();
    }
    while (link.busy) {
        wait_a_moment(timer_ns() + NETFS_TIMEOUT_NS);
    }
    link.busy = true;
}

static void unlock(void *self) {
    bool took = link.took_klock;
    (void)self;
    link.busy = false;
    link.took_klock = false;
    (void)sched_wake_on(&link, ~0u);
    if (took) {
        klock_exit(true);
    }
}

/* "a.b.c.d:port" into link.ip and link.port. */
static int parse_spec(const char *spec, size_t len) {
    size_t colon = 0;
    uint32_t port = 0;
    while (colon < len && spec[colon] != ':') {
        colon++;
    }
    if (colon == len || colon == 0 || ip4_parse(spec, (uint32_t)colon, &link.ip) != KERR_OK) {
        return KERR_INVAL;
    }
    for (size_t i = colon + 1; i < len; i++) {
        if (spec[i] < '0' || spec[i] > '9' || port > 6553u) {
            return KERR_INVAL;
        }
        port = port * 10u + (uint32_t)(spec[i] - '0');
    }
    if (port == 0 || port > 65535u) {
        return KERR_INVAL;
    }
    link.port = (uint16_t)port;
    return KERR_OK;
}

/* The spec, NUL-terminated for printing (kprintf has no precision). */
static void copy_spec(const char *spec, size_t len, char *out, size_t room) {
    size_t n = 0;
    while (n < len && n + 1u < room) {
        out[n] = spec[n];
        n++;
    }
    out[n] = '\0';
}

static void reset(void *self) {
    (void)self;
    drop();
}

int netfs_tcp_mount(const char *spec, size_t len, const char *at, void (*poll)(void)) {
    static struct netfs_transport t = {.exchange = framed_exchange, .lock = lock,
                                       .unlock = unlock, .reset = reset, .self = &link};
    struct netfs_stat root;
    char shown[40];
    int rc = parse_spec(spec, len);
    poll_receive = poll;
    copy_spec(spec, len, shown, sizeof shown);
    if (rc != KERR_OK) {
        kprintf("netfs: cannot read the server address \"%s\"\n", shown);
        return rc;
    }
    netfs_init(&client, &t, frame);
    rc = netfs_stat(&client, "/", &root);
    if (rc != KERR_OK) {
        kprintf("netfs: the server did not answer (rc %d); not mounted\n", rc);
        return rc;
    }
    attached = true;
    rc = netfsmount_attach(&client, at);
    kprintf("netfs: %s mounted at %s (rc %d)\n", shown, at, rc);
    return rc;
}

static uint32_t length(const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static bool same_text(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

int netfs_tcp_mount_hook(const char *fstype, const char *source, const char *target) {
    ip4_t ip = link.ip;
    uint16_t port = link.port;
    if (!same_text(fstype, "netfs")) {
        return KERR_NODEV;
    }
    if (!attached) {
        return netfs_tcp_mount(source, length(source), target, poll_receive);
    }
    if (parse_spec(source, length(source)) != KERR_OK || link.ip != ip ||
        link.port != port) {
        link.ip = ip;
        link.port = port;
        return KERR_BUSY; /* one server per boot: its connection is this one */
    }
    return netfsmount_attach(&client, target);
}
