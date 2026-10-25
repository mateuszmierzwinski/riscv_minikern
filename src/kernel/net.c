#include "kernel/net.h"

#include "core/console.h"
#include "core/net/tcp.h"
#include "core/errno.h"
#include "drivers/virtio_net.h"
#include "kernel/random.h"
#include "arch/riscv/timer.h"

#define NET_TICK_NS   10000000ull  /* the stack's tick: 10 ms */
#define PING_COUNT    3u
#define PING_ID       0x6d6bu      /* "mk" */
#define PING_EVERY_MS 200u

/* The interface plus what the boot-time ping is doing. */
struct knet {
    struct netif nif;
    struct tcp_stack tcp;
    uint64_t last_tick_ns;
    uint32_t pings_sent;
    uint32_t net_ticks;
    uint64_t next_ping_ms;
};

static struct knet knet;

static void on_frame(void *self, const uint8_t *frame, uint32_t len) {
    struct knet *k = self;
    net_input(&k->nif, frame, len);
}

/* A resolver the command line named, which outlives any lease: a lease
 * that arrives later would otherwise overwrite it, and the override is
 * there precisely because the lease's resolver is not wanted. */
static ip4_t dns_override;

static void on_bound(void *self, const struct netif *nif) {
    char ip[16], gw[16], dns[16];
    uint32_t prefix = 0;
    (void)self;
    for (uint32_t m = nif->mask; m & 0x80000000u; m <<= 1) {
        prefix++;
    }
    /* Where the address came from, because "dhcp, lease 0 s" for an address
     * nobody leased is a line that makes a reader doubt the rest. */
    if (nif->dhcp.state == DHCP_OFF) {
        kprintf("net: eth0 %s/%u gw %s dns %s (configured, no dhcp)\n", ip4_format(nif->ip, ip), prefix,
                ip4_format(nif->gateway, gw), ip4_format(nif->dns, dns));
    } else {
        kprintf("net: eth0 %s/%u gw %s dns %s (dhcp, lease %u s)\n", ip4_format(nif->ip, ip), prefix,
                ip4_format(nif->gateway, gw), ip4_format(nif->dns, dns), nif->lease_s);
    }
    if (dns_override != 0 && knet.nif.dns != dns_override) {
        knet.nif.dns = dns_override;
        kprintf("net: keeping the resolver the command line named (%s)\n",
                ip4_format(dns_override, dns));
    }
}

static void on_echo_reply(void *self, ip4_t from, uint16_t seq) {
    char ip[16];
    (void)self;
    kprintf("net: %s answered ping seq %u\n", ip4_format(from, ip), seq);
}

/* After DHCP: a few pings of the gateway, PING_EVERY_MS apart. */
static void maybe_ping(struct knet *k) {
    if (k->nif.ip == 0 || k->nif.gateway == 0 || k->pings_sent >= PING_COUNT || k->nif.now_ms < k->next_ping_ms) {
        return;
    }
    k->pings_sent++;
    (void)net_ping(&k->nif, k->nif.gateway, PING_ID, (uint16_t)k->pings_sent);
    k->next_ping_ms = k->nif.now_ms + PING_EVERY_MS;
}

static void on_tick(void *self, uint64_t now_ns) {
    struct knet *k = self;
    if (now_ns - k->last_tick_ns < NET_TICK_NS) {
        return;
    }
    k->last_tick_ns = now_ns;
    k->net_ticks++;
    net_tick(&k->nif, now_ns / 1000000u);
    tcp_tick(&k->tcp, now_ns / 1000000u);
    maybe_ping(k);
}

struct frame_sink knet_init(const uint8_t mac[ETH_ALEN], const struct netdev_port *dev) {
    static const struct net_events events = {.dhcp_bound = on_bound, .echo_reply = on_echo_reply};
    struct udp_handler echo = net_echo_service();
    uint32_t seed;
    net_init(&knet.nif, mac, dev, &events);
    random_fill(&seed, sizeof seed);
    (void)tcp_init(&knet.tcp, &knet.nif, seed);
    (void)net_udp_bind(&knet.nif, NET_ECHO_PORT, &echo);
    return (struct frame_sink){.frame = on_frame, .self = &knet};
}

void knet_tick(void) {
    on_tick(&knet, timer_ns());
}

void knet_start(uint32_t xid) {
    knet.nif.now_ms = timer_ns() / 1000000u;
    dhcp_start(&knet.nif, xid);
    (void)timer_add_housekeeping_hook(knet_tick);
}

void knet_start_static(ip4_t addr, uint32_t prefix, ip4_t gateway, ip4_t dns) {
    knet.nif.now_ms = timer_ns() / 1000000u;
    knet.nif.ip = addr;
    knet.nif.mask = prefix >= 32u ? 0xffffffffu : ~((1u << (32u - prefix)) - 1u);
    knet.nif.gateway = gateway;
    knet.nif.dns = dns;
    knet.nif.lease_s = 0; /* nothing granted it, so nothing expires */
    knet.nif.dhcp.state = DHCP_OFF;
    if (knet.nif.events.dhcp_bound != NULL) {
        /* The same notification as a lease: everything above only wants to
         * know that there is an address now, not where it came from. */
        knet.nif.events.dhcp_bound(knet.nif.events.self, &knet.nif);
    }
    (void)timer_add_housekeeping_hook(knet_tick);
}

void knet_report(void) {
    const struct net_stats *n = &knet.nif.stats;
    kprintf("net: %u housekeeping beats, %u net ticks, now_ms %llu, next ping %llu\n",
            timer_housekeeping_beats(), knet.net_ticks,
            (unsigned long long)knet.nif.now_ms, (unsigned long long)knet.next_ping_ms);
    const struct tcp_stats *t = &knet.tcp.stats;
    kprintf("net: rx %u tx %u; dropped: malformed %u checksum %u fragment %u not-ours %u arp %u "
            "rate %u no-port %u loop %u\n", n->rx_frames, n->tx_frames, n->drop_malformed, n->drop_checksum,
            n->drop_fragment, n->drop_not_ours, n->drop_arp_unsolicited, n->drop_rate_limited,
            n->drop_no_port, n->drop_loop_full);
    kprintf("tcp: in %u out %u conns %u retransmits %u resets %u; dropped: checksum %u malformed %u "
            "no-socket %u window %u order %u backlog %u reset %u\n", t->segments_in, t->segments_out,
            t->connections, t->retransmits, t->resets_sent, t->drop_checksum, t->drop_malformed,
            t->drop_no_socket, t->drop_out_of_window, t->drop_out_of_order, t->drop_backlog_full,
            t->drop_bad_reset);
    /* **Reported, because an option nobody counts is an option nobody
     * checks.** SACK is invisible when it works and invisible when it does
     * not, so the two directions are printed: blocks sent means this stack
     * told a peer what it was holding, blocks received means it understood a
     * peer that told it. */
    kprintf("tcp: sack blocks %u sent, %u received\n", t->sack_blocks_sent,
            t->sack_blocks_received);
}

void knet_set_dns(ip4_t dns) {
    dns_override = dns;
    knet.nif.dns = dns;
}

int knet_wait_address(uint32_t timeout_ms) {
    uint64_t deadline = timer_ns() / 1000000u + timeout_ms;
    while (knet.nif.ip == 0) {
        if (timer_ns() / 1000000u >= deadline) {
            return KERR_TIMEDOUT;
        }
        /* Waiting with interrupts on, so the frame that carries the lease
         * can actually arrive. The ARM project had an `irq_wait` for this;
         * here it is the one instruction, because nothing else is needed:
         * the device's interrupt and the tick both wake it. */
        __asm__ volatile("csrsi sstatus, 2\n\twfi\n\tcsrci sstatus, 2" : : : "memory");
        virtio_net_poll(&knet.nif);
        knet_tick();
    }
    return KERR_OK;
}

struct netif *knet_interface(void) {
    return &knet.nif;
}

struct tcp_stack *knet_tcp(void) {
    return &knet.tcp;
}
