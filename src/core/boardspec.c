#include "core/boardspec.h"

#include <stddef.h>

#include "core/cmdline.h"

/* A number in decimal or 0x hex, `len` bytes; false for anything else. The
 * suffix, when allowed, multiplies (K, M, G). */
static bool parse_number(const char *s, size_t len, bool suffix, uint64_t *out) {
    uint64_t v = 0, base = 10;
    size_t i = 0;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i >= len) {
        return false;
    }
    for (; i < len; i++) {
        char c = s[i];
        uint64_t d = c >= '0' && c <= '9' ? (uint64_t)(c - '0')
                     : base == 16 && c >= 'a' && c <= 'f' ? (uint64_t)(c - 'a' + 10)
                     : base == 16 && c >= 'A' && c <= 'F' ? (uint64_t)(c - 'A' + 10)
                                                          : 99u;
        if (d == 99u && suffix && i == len - 1u && (c == 'K' || c == 'M' || c == 'G')) {
            *out = v << (c == 'K' ? 10 : c == 'M' ? 20 : 30);
            return true;
        }
        if (d >= base || v > (UINT64_MAX - d) / base) {
            return false;
        }
        v = v * base + d;
    }
    *out = v;
    return true;
}

/* Where `c` first occurs in the `len` bytes at s, or len. */
static size_t find(const char *s, size_t len, char c) {
    size_t i = 0;
    while (i < len && s[i] != c) {
        i++;
    }
    return i;
}

static void parse_mem(const char *v, size_t len, struct board_spec *out) {
    size_t colon = find(v, len, ':');
    uint64_t base = 0, size = 0;
    if (colon < len && parse_number(v, colon, false, &base) &&
        parse_number(v + colon + 1u, len - colon - 1u, true, &size) && size != 0) {
        out->has_mem = true;
        out->mem_base = base;
        out->mem_size = size;
    }
}

static bool same(const char *a, size_t len, const char *b) {
    size_t i = 0;
    while (i < len && b[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return i == len && b[i] == '\0';
}

static void set_uart(struct board_spec *out, uint64_t base, uint32_t shift, uint32_t width) {
    out->has_uart = true;
    out->uart_base = base;
    out->uart_shift = shift;
    out->uart_width = width;
}

/* "uart8250,mmio32,0x10000000" (extra fields after the address, such as a
 * baud rate, are allowed and ignored: the firmware set the rate). */
static void parse_earlycon(const char *v, size_t len, struct board_spec *out) {
    size_t a = find(v, len, ','), b, end;
    uint64_t addr = 0;
    if (a == len || !same(v, a, "uart8250")) {
        return;
    }
    b = a + 1u + find(v + a + 1u, len - a - 1u, ',');
    if (b >= len) {
        return;
    }
    end = b + 1u + find(v + b + 1u, len - b - 1u, ',');
    if (!parse_number(v + b + 1u, end - b - 1u, false, &addr)) {
        return;
    }
    if (same(v + a + 1u, b - a - 1u, "mmio32")) {
        set_uart(out, addr, 2u, 4u);
    } else if (same(v + a + 1u, b - a - 1u, "mmio")) {
        set_uart(out, addr, 0u, 1u);
    }
}

void board_spec_parse(const char *cmdline, struct board_spec *out) {
    size_t len = 0;
    const char *v;
    *out = (struct board_spec){0};
    if (cmdline == NULL) {
        return;
    }
    if ((v = cmdline_value(cmdline, "minikern.mem", &len)) != NULL) {
        parse_mem(v, len, out);
    }
    if ((v = cmdline_value(cmdline, "earlycon", &len)) != NULL) {
        parse_earlycon(v, len, out);
    }
    if ((v = cmdline_value(cmdline, "minikern.timebase", &len)) != NULL &&
        !parse_number(v, len, false, &out->timebase)) {
        out->timebase = 0;
    }
}
