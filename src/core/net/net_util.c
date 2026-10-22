#include "core/net/net_internal.h"

#include "core/errno.h"
#include "lib/kfmt.h"

const uint8_t mac_broadcast[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void net_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

void net_zero(void *dst, uint32_t n) {
    uint8_t *d = dst;
    for (uint32_t i = 0; i < n; i++) {
        d[i] = 0;
    }
}

bool mac_equal(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < ETH_ALEN; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

uint32_t net_sum(const void *data, uint32_t len, uint32_t sum) {
    const uint8_t *p = data;
    uint32_t i = 0;
    for (; i + 1 < len; i += 2) {
        sum += (uint32_t)get16(p + i);
    }
    if (i < len) {
        sum += (uint32_t)p[i] << 8;
    }
    return sum;
}

uint16_t net_checksum(const void *data, uint32_t len, uint32_t sum) {
    sum = net_sum(data, len, sum);
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

int ip4_parse(const char *text, uint32_t len, ip4_t *out) {
    uint32_t at = 0, ip = 0;
    for (uint32_t octet = 0; octet < 4u; octet++) {
        uint32_t v = 0, digits = 0;
        for (; at < len && text[at] >= '0' && text[at] <= '9'; at++, digits++) {
            v = v * 10u + (uint32_t)(text[at] - '0');
        }
        if (digits == 0 || digits > 3u || v > 255u) {
            return KERR_INVAL;
        }
        ip = (ip << 8) | v;
        if (octet < 3u && (at >= len || text[at++] != '.')) {
            return KERR_INVAL;
        }
    }
    if (at != len) {
        return KERR_INVAL;
    }
    *out = ip;
    return KERR_OK;
}

const char *ip4_format(ip4_t ip, char buf[16]) {
    kfmt_snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xffu, (ip >> 8) & 0xffu, ip & 0xffu);
    return buf;
}
