/* net_frames — builders for the frames a network test feeds to the stack,
 * and readers for the frames it sends back. Shared by net_test.c and
 * netboot_test.c so there is one copy of the wire layout in the tests. */
#ifndef MINIKERN_TEST_NET_FRAMES_H
#define MINIKERN_TEST_NET_FRAMES_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/net/net.h"

#define FRAME_MAX_SENT 256

/* What the fake netdev captured. */
struct capture {
    uint8_t frame[FRAME_MAX_SENT][ETH_FRAME_MAX];
    uint32_t len[FRAME_MAX_SENT];
    uint32_t n;
};

static inline int capture_send(void *self, const uint8_t *frame, uint32_t len) {
    struct capture *c = self;
    if (c->n < FRAME_MAX_SENT) {
        memcpy(c->frame[c->n], frame, len);
        c->len[c->n++] = len;
    }
    return 0;
}

static inline void fp16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void fp32(uint8_t *p, uint32_t v) {
    fp16(p, (uint16_t)(v >> 16));
    fp16(p + 2, (uint16_t)v);
}

static inline uint16_t fg16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t fg32(const uint8_t *p) {
    return ((uint32_t)fg16(p) << 16) | fg16(p + 2);
}

/* Offsets inside an Ethernet + IPv4 frame. F_L4 is where the transport
 * header starts (UDP's or TCP's); F_DATA is a UDP payload. */
#define F_IP   14u
#define F_L4   34u
#define F_UDP  F_L4
#define F_DATA 42u

/* An Ethernet + IPv4 header into f; returns where the payload goes. */
static inline uint8_t *frame_ip(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src_mac, ip4_t src, ip4_t dst,
                         uint8_t proto, uint32_t plen) {
    uint8_t *ip = f + F_IP;
    memcpy(f, dst_mac, 6);
    memcpy(f + 6, src_mac, 6);
    fp16(f + 12, 0x0800);
    memset(ip, 0, 20);
    ip[0] = 0x45;
    fp16(ip + 2, (uint16_t)(20 + plen));
    ip[8] = 64;
    ip[9] = proto;
    fp32(ip + 12, src);
    fp32(ip + 16, dst);
    fp16(ip + 10, net_checksum(ip, 20, 0));
    return ip + 20;
}

/* A UDP datagram with a correct checksum; returns the frame length. */
static inline uint32_t frame_udp(uint8_t *f, const uint8_t *dst_mac, const uint8_t *src_mac, ip4_t src, ip4_t dst,
                          uint16_t sport, uint16_t dport, const void *data, uint32_t len) {
    uint8_t *u = frame_ip(f, dst_mac, src_mac, src, dst, 17, 8 + len);
    uint8_t ph[12];
    fp16(u, sport);
    fp16(u + 2, dport);
    fp16(u + 4, (uint16_t)(8 + len));
    fp16(u + 6, 0);
    memcpy(u + 8, data, len);
    fp32(ph, src);
    fp32(ph + 4, dst);
    ph[8] = 0;
    ph[9] = 17;
    fp16(ph + 10, (uint16_t)(8 + len));
    fp16(u + 6, net_checksum(u, 8 + len, net_sum(ph, 12, 0)));
    return F_DATA + len;
}

/* True when the frame is an ARP request for ip. */
static inline bool frame_is_arp_request(const uint8_t *f, uint32_t len, ip4_t ip) {
    return len >= 42u && fg16(f + 12) == 0x0806 && fg16(f + 20) == 1u && fg32(f + 38) == ip;
}

/* An ARP reply saying ip is at mac, answering the request in req. */
static inline uint32_t frame_arp_reply(uint8_t *f, const uint8_t *req, ip4_t ip, const uint8_t *mac) {
    memcpy(f, req + 22, 6);        /* back to the asker */
    memcpy(f + 6, mac, 6);
    fp16(f + 12, 0x0806);
    fp16(f + 14, 1);
    fp16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    fp16(f + 20, 2);               /* reply */
    memcpy(f + 22, mac, 6);
    fp32(f + 28, ip);
    memcpy(f + 32, req + 22, 6);
    fp32(f + 38, fg32(req + 28));
    return 42;
}

/* True when the captured frame is a UDP datagram for dport. */
static inline bool frame_is_udp(const uint8_t *f, uint32_t len, uint16_t dport) {
    return len >= F_DATA && fg16(f + 12) == 0x0800 && f[F_IP + 9] == 17 && fg16(f + F_UDP + 2) == dport;
}

#endif
