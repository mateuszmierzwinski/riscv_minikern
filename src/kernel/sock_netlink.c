/* Netlink, so that a program can ask what interfaces exist.
 *
 * **Why this and not a `/proc` file or an ioctl.** The question "what
 * interfaces does this machine have" has one answer every program already
 * knows how to ask: Go's `net.Interfaces` opens a netlink socket and sends
 * `RTM_GETLINK`, and so does every tool written against Linux. A private
 * interface here would mean a kernel that *has* the information and no
 * program able to read it, which is the state this replaces.
 *
 * It is a fourth `sock_ops` implementation beside UDP, TCP and unix, so it
 * costs the socket layer nothing. A request is **answered synchronously into
 * a reply buffer** at `send` time rather than by a kernel thread: netlink is
 * request and response over a datagram socket, the answers here are small
 * and bounded, and a reply built on the spot cannot be a queue that grows
 * without a reader.
 *
 * What it answers: `RTM_GETLINK` (the links), `RTM_GETADDR` (their
 * addresses) and `RTM_GETROUTE` (the routing table, including the two
 * implicit routes). Anything else gets `NLMSG_ERROR` with `EOPNOTSUPP`,
 * which is what a caller can act on — an empty dump would read as "this
 * machine has no interfaces", a far worse answer than "I do not implement
 * that".
 */
#include "kernel/netlink.h"

#include "core/errno.h"
#include "kernel/file.h"
#include "kernel/mem.h"
#include "kernel/sched.h"
#include "kernel/socket.h"

#include <stddef.h>

/* One reply, built whole at send time. Four interfaces' worth of links,
 * addresses and routes fits in well under this; the bound is checked at
 * every append rather than assumed, because the encoder is the thing most
 * likely to be wrong. */
#define REPLY_BYTES 2048u

struct netlink_sock {
    uint8_t reply[REPLY_BYTES];
    uint32_t len;
    uint32_t offset;
    /* The port this socket bound, which is echoed in replies. Zero means the
     * kernel, and a socket that never bound is answered as zero — which is
     * what Linux does and what a caller comparing `nlmsg_pid` expects. */
    uint32_t port;
};

static struct netlink_sock *nl(struct sock *s) {
    return s->proto;
}

/* ── building a reply ───────────────────────────────────────────────────── */

/* A reply under construction, so that the length of a message can be filled
 * in after its attributes are appended — the header carries the total and
 * the total is not known until the end. */
struct builder {
    uint8_t *at;
    uint32_t used;
    uint32_t room;
    uint32_t message_start;
    bool overflowed;
};

static void *reserve(struct builder *b, uint32_t n) {
    uint32_t aligned = NLMSG_ALIGN(n);
    if (b->used + aligned > b->room) {
        /* **Recorded, not truncated.** A dump cut short in the middle of a
         * message is a reply the caller cannot parse, and one cut between
         * messages is a lie about what exists. Either way the caller is told. */
        b->overflowed = true;
        return NULL;
    }
    {
        uint8_t *p = b->at + b->used;
        for (uint32_t i = 0; i < aligned; i++) {
            p[i] = 0;
        }
        b->used += aligned;
        return p;
    }
}

/* Starts a message and returns its payload, or NULL when there is no room. */
static void *begin_message(struct builder *b, uint16_t type, uint32_t seq, uint32_t port,
                           uint32_t payload, bool multi) {
    struct nlmsghdr *h;
    b->message_start = b->used;
    h = reserve(b, NLMSG_HDRLEN);
    if (h == NULL) {
        return NULL;
    }
    h->nlmsg_type = type;
    h->nlmsg_flags = (uint16_t)(multi ? NLM_F_MULTI : 0u);
    h->nlmsg_seq = seq;
    h->nlmsg_pid = port;
    return reserve(b, payload);
}

/* Fills in the length of the message begun last. */
static void end_message(struct builder *b) {
    struct nlmsghdr *h = (struct nlmsghdr *)(b->at + b->message_start);
    /* **The true byte count, not the aligned one.** A reader steps to the
     * next message by aligning this up, so storing the aligned value here
     * makes every message claim the padding of the one before it. */
    h->nlmsg_len = b->used - b->message_start;
}

static void add_attr(struct builder *b, uint16_t type, const void *value, uint32_t n) {
    struct rtattr *a = reserve(b, RTA_LENGTH(n));
    if (a == NULL) {
        return;
    }
    a->rta_type = type;
    a->rta_len = (uint16_t)RTA_LENGTH(n);
    {
        uint8_t *dst = (uint8_t *)a + RTA_ALIGN(sizeof *a);
        const uint8_t *src = value;
        for (uint32_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
    }
}

static void add_u32(struct builder *b, uint16_t type, uint32_t v) {
    add_attr(b, type, &v, sizeof v);
}

/* An IPv4 address, **on the wire in network order**. The stack keeps them in
 * host order, and netlink does not: an address handed over unswapped reads as
 * a different one entirely, which is the sort of defect that looks like a
 * routing bug for an afternoon. */
static void add_ip(struct builder *b, uint16_t type, ip4_t ip) {
    uint8_t be[4] = {(uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip};
    add_attr(b, type, be, sizeof be);
}

static void add_string(struct builder *b, uint16_t type, const char *s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    add_attr(b, type, s, n + 1u); /* the NUL is part of the value, as on Linux */
}

/* ── the interfaces this machine has ───────────────────────────────────── */

/* The loopback is index 1 and the real interface index 2, which is the
 * convention every Linux machine follows and which a program that hard-codes
 * "interface 1 is lo" relies on. */
#define LO_INDEX 1u
#define ETH_INDEX 2u
#define LO_MTU 65536u
#define ETH_MTU 1500u

static void dump_one_link(struct builder *b, uint32_t seq, uint32_t port, uint32_t index) {
    struct ifinfomsg *m = begin_message(b, RTM_NEWLINK, seq, port, sizeof *m, true);
    if (m == NULL) {
        return;
    }
    m->ifi_index = index;
    if (index == LO_INDEX) {
        m->ifi_type = (uint16_t)ARPHRD_LOOPBACK;
        m->ifi_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
        add_string(b, IFLA_IFNAME, "lo");
        add_u32(b, IFLA_MTU, LO_MTU);
    } else {
        uint8_t mac[6] = {0};
        struct netif *nif = socket_netif();
        m->ifi_type = (uint16_t)ARPHRD_ETHER;
        /* **Running only when it has an address.** A caller choosing an
         * interface to bind to reads these flags, and reporting a link as
         * running before DHCP has finished sends it at an interface that
         * cannot yet carry anything. */
        m->ifi_flags = IFF_UP | (nif != NULL && nif->ip != 0 ? IFF_RUNNING : 0u);
        add_string(b, IFLA_IFNAME, "eth0");
        if (nif != NULL) {
            for (uint32_t i = 0; i < 6u; i++) {
                mac[i] = nif->mac[i];
            }
        }
        add_attr(b, IFLA_ADDRESS, mac, sizeof mac);
        add_u32(b, IFLA_MTU, ETH_MTU);
    }
    end_message(b);
}

/* How many leading ones a mask has, which is what netlink reports instead of
 * the mask itself. */
static uint8_t prefix_of(ip4_t mask) {
    uint8_t n = 0;
    for (uint32_t i = 0; i < 32u; i++) {
        n += (uint8_t)((mask >> i) & 1u);
    }
    return n;
}

static void dump_one_addr(struct builder *b, uint32_t seq, uint32_t port, uint32_t index) {
    struct netif *nif = socket_netif();
    struct ifaddrmsg *m;
    if (index == ETH_INDEX && (nif == NULL || nif->ip == 0)) {
        return; /* no address yet: reporting one would be inventing it */
    }
    m = begin_message(b, RTM_NEWADDR, seq, port, sizeof *m, true);
    if (m == NULL) {
        return;
    }
    m->ifa_family = AF_INET;
    m->ifa_index = index;
    if (index == LO_INDEX) {
        m->ifa_prefixlen = 8;
        m->ifa_scope = (uint8_t)RT_SCOPE_HOST;
        add_ip(b, IFA_ADDRESS, IP4(127, 0, 0, 1));
        add_ip(b, IFA_LOCAL, IP4(127, 0, 0, 1));
        add_string(b, IFA_LABEL, "lo");
    } else {
        m->ifa_prefixlen = prefix_of(nif->mask);
        m->ifa_scope = (uint8_t)RT_SCOPE_UNIVERSE;
        add_ip(b, IFA_ADDRESS, nif->ip);
        add_ip(b, IFA_LOCAL, nif->ip);
        add_string(b, IFA_LABEL, "eth0");
    }
    end_message(b);
}

/* One route. `gateway` of zero means on-link, which is reported by *omitting*
 * `RTA_GATEWAY` and setting a link scope — not by sending a zero gateway,
 * which a reader would take as a route via 0.0.0.0. */
static void dump_one_route(struct builder *b, uint32_t seq, uint32_t port, ip4_t dst,
                           uint8_t dst_len, ip4_t gateway) {
    struct rtmsg *m = begin_message(b, RTM_NEWROUTE, seq, port, sizeof *m, true);
    if (m == NULL) {
        return;
    }
    m->rtm_family = AF_INET;
    m->rtm_dst_len = dst_len;
    m->rtm_table = (uint8_t)RT_TABLE_MAIN;
    m->rtm_protocol = (uint8_t)RTPROT_BOOT;
    m->rtm_type = (uint8_t)RTN_UNICAST;
    m->rtm_scope = (uint8_t)(gateway != 0 ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK);
    if (dst_len != 0) {
        add_ip(b, RTA_DST, dst);
    }
    if (gateway != 0) {
        add_ip(b, RTA_GATEWAY, gateway);
    }
    add_u32(b, RTA_OIF, ETH_INDEX);
    end_message(b);
}

/* The whole routing table: the interface's own subnet and the default route,
 * which are rules rather than entries, then everything `net_route_add` has
 * been given. Reported in that order because it is the order they are
 * *ranked* in, and a reader printing them in receipt order then sees the
 * same picture the kernel uses. */
static void dump_routes(struct builder *b, uint32_t seq, uint32_t port) {
    struct netif *nif = socket_netif();
    if (nif == NULL || nif->ip == 0) {
        return;
    }
    dump_one_route(b, seq, port, nif->ip & nif->mask, prefix_of(nif->mask), 0);
    if (nif->gateway != 0) {
        dump_one_route(b, seq, port, 0, 0, nif->gateway);
    }
    for (uint32_t i = 0; i < NET_ROUTES; i++) {
        const struct route *r = net_route_at(nif, i);
        if (r != NULL) {
            dump_one_route(b, seq, port, r->dst, prefix_of(r->mask), r->gateway);
        }
    }
}

/* ── answering a request ───────────────────────────────────────────────── */

/* `NLMSG_DONE`, which is how a reader knows a multi-part dump has ended.
 * Without it a caller blocks waiting for more, so this is not optional
 * politeness — it is the terminator of the protocol. */
static void finish_dump(struct builder *b, uint32_t seq, uint32_t port) {
    int32_t *end = begin_message(b, (uint16_t)NLMSG_DONE, seq, port, sizeof(int32_t), true);
    if (end != NULL) {
        *end = 0;
        end_message(b);
    }
}

/* `NLMSG_ERROR`, which carries a **negative** errno and a copy of the header
 * it is refusing — so a caller with several requests in flight can tell
 * which one failed. An error with error 0 is an acknowledgement, which is
 * what `NLM_F_ACK` asks for, and the same message serves both. */
static void put_error(struct builder *b, const struct nlmsghdr *req, int32_t err) {
    struct { int32_t error; struct nlmsghdr original; } *payload;
    payload = begin_message(b, (uint16_t)NLMSG_ERROR, req->nlmsg_seq, req->nlmsg_pid,
                            sizeof *payload, false);
    if (payload != NULL) {
        payload->error = err;
        payload->original = *req;
        end_message(b);
    }
}

/* Answers one request into the builder. */
static void answer(struct builder *b, struct sock *s, const struct nlmsghdr *req) {
    uint32_t seq = req->nlmsg_seq;
    uint32_t port = nl(s)->port;
    switch (req->nlmsg_type) {
    case RTM_GETLINK:
        dump_one_link(b, seq, port, LO_INDEX);
        dump_one_link(b, seq, port, ETH_INDEX);
        finish_dump(b, seq, port);
        return;
    case RTM_GETADDR:
        dump_one_addr(b, seq, port, LO_INDEX);
        dump_one_addr(b, seq, port, ETH_INDEX);
        finish_dump(b, seq, port);
        return;
    case RTM_GETROUTE:
        dump_routes(b, seq, port);
        finish_dump(b, seq, port);
        return;
    default:
        /* **Refused, never answered with an empty dump.** An empty dump
         * reads as "this machine has none of those", which is a wrong answer
         * where this is merely an unimplemented one. */
        put_error(b, req, EOPNOTSUPP);
        return;
    }
}

static int32_t netlink_send(struct sock *s, const void *buf, uint32_t n, const struct sock_addr *to) {
    struct netlink_sock *k = nl(s);
    struct builder b = {.at = k->reply, .room = REPLY_BYTES};
    uint32_t at = 0;
    (void)to;
    if (n < NLMSG_HDRLEN) {
        return KERR_INVAL;
    }
    /* **Every request in the datagram**, because netlink permits several and
     * a caller that sent two and got one answer waits for ever. Each is
     * bounded by its own length, and a length that does not advance is a
     * malformed message rather than a reason to loop. */
    while (at + NLMSG_HDRLEN <= n) {
        const struct nlmsghdr *req = (const struct nlmsghdr *)((const uint8_t *)buf + at);
        uint32_t len = req->nlmsg_len;
        if (len < NLMSG_HDRLEN || at + len > n) {
            return KERR_INVAL;
        }
        answer(&b, s, req);
        at += NLMSG_ALIGN(len);
    }
    if (b.overflowed) {
        /* The reply did not fit. Reported rather than sent short, because a
         * truncated dump is indistinguishable from a complete one. */
        k->len = 0;
        k->offset = 0;
        return EMSGSIZE;
    }
    k->len = b.used;
    k->offset = 0;
    socket_wake(s);
    return (int32_t)n;
}

static int32_t netlink_recv(struct sock *s, void *buf, uint32_t n, struct sock_addr *from,
                            uint32_t flags, bool nonblock) {
    struct netlink_sock *k = nl(s);
    uint32_t left, take;
    (void)flags;
    while (k->offset >= k->len) {
        int rc;
        if (nonblock) {
            return KERR_AGAIN;
        }
        rc = sched_wait_on(s, sock_deadline(s, false));
        if (rc == KERR_INTR) {
            return rc;
        }
        if (rc == KERR_TIMEDOUT) {
            return KERR_AGAIN;
        }
    }
    if (from != NULL) {
        *from = (struct sock_addr){0}; /* from the kernel, which is port 0 */
    }
    left = k->len - k->offset;
    take = n < left ? n : left;
    for (uint32_t i = 0; i < take; i++) {
        ((uint8_t *)buf)[i] = k->reply[k->offset + i];
    }
    k->offset += take;
    return (int32_t)take;
}

/* `sockaddr_nl` is a family, two bytes of padding, a port and a group mask.
 * The port is the caller's choice of identity; zero means "give me one", and
 * this gives the only one it has. */
static int netlink_bind(struct sock *s, struct sock_addr a) {
    nl(s)->port = a.port;
    s->bound = true;
    return KERR_OK;
}

static int netlink_connect(struct sock *s, struct sock_addr a) {
    (void)a;
    /* Connecting a netlink socket means "talk to the kernel", which is the
     * only peer there is, so it is accepted and records nothing. */
    s->connected = true;
    return KERR_OK;
}

static uint32_t netlink_poll(struct sock *s) {
    return EPOLLOUT | (nl(s)->offset < nl(s)->len ? EPOLLIN : 0u);
}

static int netlink_nothing(struct sock *s, int how) {
    (void)s;
    (void)how;
    return EOPNOTSUPP;
}

static int netlink_no_accept(struct sock *s, struct sock **out, bool nonblock) {
    (void)s;
    (void)out;
    (void)nonblock;
    return EOPNOTSUPP;
}

static void netlink_release(struct sock *s) {
    kfree(nl(s));
    s->proto = NULL;
}

static const struct sock_ops netlink_ops = {
    .bind = netlink_bind,
    .connect = netlink_connect,
    .send = netlink_send,
    .recv = netlink_recv,
    .poll = netlink_poll,
    .listen = NULL,
    .accept = netlink_no_accept,
    .shutdown = netlink_nothing,
    .release = netlink_release,
};

int netlink_sock_init(struct sock *s, int type, int protocol) {
    struct netlink_sock *k;
    /* `SOCK_RAW` is what every netlink caller asks for and `SOCK_DGRAM` is
     * equally valid; the two behave identically here, as they do on Linux. */
    if (type != SOCK_RAW && type != SOCK_DGRAM) {
        return EPROTONOSUPPORT;
    }
    if (protocol != NETLINK_ROUTE) {
        /* The other families — `NETLINK_AUDIT`, `NETLINK_NETFILTER` — have
         * nothing behind them here, and saying so is better than accepting a
         * socket that answers everything with an error. */
        return EPROTONOSUPPORT;
    }
    k = kzalloc(sizeof *k);
    if (k == NULL) {
        return KERR_NOMEM;
    }
    s->proto = k;
    s->ops = &netlink_ops;
    return KERR_OK;
}
