/* net_echo — the RFC 862 UDP echo service, hardened against reflection. */
#include "core/net/net_internal.h"

#define PORT_ECHO    7u
#define PORT_DAYTIME 13u
#define PORT_CHARGEN 19u
#define PORT_TIME    37u

static bool reflector_port(uint16_t port) {
    return port == PORT_ECHO || port == PORT_DAYTIME || port == PORT_CHARGEN || port == PORT_TIME;
}

static void echo_receive(void *self, struct netif *nif, const struct udp_datagram *d) {
    (void)self;
    if (d->dst != nif->ip || reflector_port(d->sport) || nif->icmp_tokens == 0) {
        nif->stats.drop_rate_limited++;
        return;
    }
    nif->icmp_tokens--;
    (void)net_udp_send(nif, d->src, d->dport, d->sport, d->data, d->len);
}

struct udp_handler net_echo_service(void) {
    return (struct udp_handler){.receive = echo_receive, .self = 0};
}
