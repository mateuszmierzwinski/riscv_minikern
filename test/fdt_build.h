/* fdt_build — a tiny device-tree blob writer for tests. It produces the
 * same layout dtc does (header, empty reservation map, structure block,
 * strings block), so tests can build a tree and then corrupt it on purpose. */
#ifndef MINIKERN_TEST_FDT_BUILD_H
#define MINIKERN_TEST_FDT_BUILD_H

#include <stdint.h>
#include <string.h>

#define FDTB_CAP 4096

struct fdtb {
    uint8_t blob[FDTB_CAP];
    uint8_t strct[FDTB_CAP];
    char strings[FDTB_CAP];
    uint32_t slen, strlen_;
};

static inline void fdtb_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void fdtb_token(struct fdtb *b, uint32_t v) {
    fdtb_put32(b->strct + b->slen, v);
    b->slen += 4;
}

static inline void fdtb_bytes(struct fdtb *b, const void *data, uint32_t len) {
    memcpy(b->strct + b->slen, data, len);
    b->slen += len;
    while (b->slen % 4 != 0) {
        b->strct[b->slen++] = 0;
    }
}

static inline void fdtb_begin(struct fdtb *b, const char *name) {
    fdtb_token(b, 1);
    fdtb_bytes(b, name, (uint32_t)strlen(name) + 1);
}

static inline void fdtb_end(struct fdtb *b) {
    fdtb_token(b, 2);
}

static inline uint32_t fdtb_string(struct fdtb *b, const char *name) {
    uint32_t off = b->strlen_;
    memcpy(b->strings + off, name, strlen(name) + 1);
    b->strlen_ += (uint32_t)strlen(name) + 1;
    return off;
}

static inline void fdtb_prop(struct fdtb *b, const char *name, const void *value, uint32_t len) {
    fdtb_token(b, 3);
    fdtb_token(b, len);
    fdtb_token(b, fdtb_string(b, name));
    fdtb_bytes(b, value, len);
}

static inline void fdtb_prop_u32(struct fdtb *b, const char *name, uint32_t v) {
    uint8_t be[4];
    fdtb_put32(be, v);
    fdtb_prop(b, name, be, 4);
}

static inline void fdtb_prop_cells2(struct fdtb *b, const char *name, uint32_t x, uint32_t y) {
    uint8_t be[8];
    fdtb_put32(be, x);
    fdtb_put32(be + 4, y);
    fdtb_prop(b, name, be, 8);
}

static inline void fdtb_prop_str(struct fdtb *b, const char *name, const char *s) {
    fdtb_prop(b, name, s, (uint32_t)strlen(s) + 1);
}

/* Closes the structure block and lays the blob out; returns its size. */
static inline uint32_t fdtb_finish(struct fdtb *b) {
    uint32_t off_rsv = 40, off_struct = 56, off_strings, total;
    fdtb_token(b, 9);
    off_strings = off_struct + b->slen;
    total = off_strings + b->strlen_;
    memset(b->blob, 0, sizeof b->blob);
    fdtb_put32(b->blob, 0xd00dfeedu);
    fdtb_put32(b->blob + 4, total);
    fdtb_put32(b->blob + 8, off_struct);
    fdtb_put32(b->blob + 12, off_strings);
    fdtb_put32(b->blob + 16, off_rsv);
    fdtb_put32(b->blob + 20, 17);
    fdtb_put32(b->blob + 24, 16);
    fdtb_put32(b->blob + 32, b->strlen_);
    fdtb_put32(b->blob + 36, b->slen);
    memcpy(b->blob + off_struct, b->strct, b->slen);
    memcpy(b->blob + off_strings, b->strings, b->strlen_);
    return total;
}

/* A four-cell (base, size) pair: #address-cells = 2 and #size-cells = 2,
 * which is what every RISC-V device tree uses and what lets a memory node
 * describe more than 4 GiB. */
static inline void fdtb_prop_cells4(struct fdtb *b, const char *name, uint64_t base, uint64_t size) {
    uint8_t v[16];
    fdtb_put32(v, (uint32_t)(base >> 32));
    fdtb_put32(v + 4, (uint32_t)base);
    fdtb_put32(v + 8, (uint32_t)(size >> 32));
    fdtb_put32(v + 12, (uint32_t)size);
    fdtb_prop(b, name, v, sizeof v);
}

/* A `compatible` property: a list of NUL-separated strings, which is how a
 * device names what it is, most specific first. */
static inline void fdtb_prop_compatible(struct fdtb *b, const char *first, const char *second) {
    uint8_t v[128];
    uint32_t at = 0;
    for (const char *s = first; s != NULL; s = (s == first) ? second : NULL) {
        for (uint32_t i = 0; s[i] != '\0' && at + 1u < sizeof v; i++) {
            v[at++] = (uint8_t)s[i];
        }
        v[at++] = 0;
        if (second == NULL) {
            break;
        }
    }
    fdtb_prop(b, "compatible", v, at);
}

/* A board with a device on it: a 16550 at `base`, named the way the JH7110's
 * tree names one, inside a /soc node — so the test exercises a device that is
 * not at the root, which is where every real one is. */
static inline uint32_t fdtb_board_with_uart(struct fdtb *b, uint64_t base, uint64_t size) {
    memset(b, 0, sizeof *b);
    fdtb_begin(b, "");
    fdtb_prop_u32(b, "#address-cells", 2);
    fdtb_prop_u32(b, "#size-cells", 2);
    fdtb_begin(b, "soc");
    fdtb_begin(b, "serial@10000000");
    fdtb_prop_compatible(b, "snps,dw-apb-uart", "ns16550a");
    fdtb_prop_cells4(b, "reg", base, size);
    fdtb_end(b);
    fdtb_begin(b, "gpio@13040000");
    fdtb_prop_compatible(b, "starfive,jh7110-sys-pinctrl", NULL);
    fdtb_prop_cells4(b, "reg", 0x13040000ull, 0x10000ull);
    fdtb_end(b);
    fdtb_end(b);
    fdtb_end(b);
    return fdtb_finish(b);
}

/* The shape a RISC-V board's firmware passes: two cells for both, DRAM at
 * 0x4000_0000 as the JH7110 has it, and a size the caller chooses so that
 * the 8 GiB case can be tested. */
static inline uint32_t fdtb_riscv_board(struct fdtb *b, const char *bootargs, uint64_t mem_size) {
    memset(b, 0, sizeof *b);
    fdtb_begin(b, "");
    fdtb_prop_u32(b, "#address-cells", 2);
    fdtb_prop_u32(b, "#size-cells", 2);
    fdtb_begin(b, "cpus");
    /* Hart 0 is the monitor core on this SoC; the application cores are
     * 1..4, which is why nothing may assume hart 0 is its own. */
    fdtb_begin(b, "cpu@0");
    fdtb_prop_u32(b, "reg", 0);
    fdtb_end(b);
    fdtb_begin(b, "cpu@1");
    fdtb_prop_u32(b, "reg", 1);
    fdtb_end(b);
    fdtb_end(b);
    fdtb_begin(b, "chosen");
    fdtb_prop_str(b, "bootargs", bootargs);
    fdtb_end(b);
    fdtb_begin(b, "memory@40000000");
    fdtb_prop_str(b, "device_type", "memory");
    fdtb_prop_cells4(b, "reg", 0x40000000ull, mem_size);
    fdtb_end(b);
    fdtb_end(b);
    return fdtb_finish(b);
}

/* A row of transport slots, the way a board advertises virtio: several nodes
 * with the same compatible string, each at its own address. */
static inline uint32_t fdtb_board_with_slots(struct fdtb *b, uint32_t count) {
    memset(b, 0, sizeof *b);
    fdtb_begin(b, "");
    fdtb_prop_u32(b, "#address-cells", 2);
    fdtb_prop_u32(b, "#size-cells", 2);
    fdtb_begin(b, "soc");
    fdtb_prop_u32(b, "#address-cells", 2);
    fdtb_prop_u32(b, "#size-cells", 2);
    for (uint32_t i = 0; i < count; i++) {
        char name[32];
        uint32_t at = 0;
        const char *prefix = "virtio_mmio@";
        uint64_t base = 0x10001000ull + (uint64_t)i * 0x1000ull;
        for (const char *c = prefix; *c != '\0'; c++) {
            name[at++] = *c;
        }
        for (int shift = 28; shift >= 0; shift -= 4) {
            name[at++] = "0123456789abcdef"[(base >> shift) & 0xf];
        }
        name[at] = '\0';
        fdtb_begin(b, name);
        fdtb_prop_compatible(b, "virtio,mmio", NULL);
        fdtb_prop_cells4(b, "reg", base, 0x200ull);
        fdtb_end(b);
    }
    fdtb_end(b);
    fdtb_end(b);
    return fdtb_finish(b);
}

/* The NanoPi NEO's shape as U-Boot passes it, trimmed to what we read. */
static inline uint32_t fdtb_neo(struct fdtb *b, const char *bootargs) {
    memset(b, 0, sizeof *b);
    fdtb_begin(b, "");
    fdtb_prop_u32(b, "#address-cells", 1);
    fdtb_prop_u32(b, "#size-cells", 1);
    fdtb_begin(b, "cpus");
    fdtb_begin(b, "cpu@0");
    fdtb_prop_u32(b, "reg", 0);
    fdtb_end(b);
    fdtb_end(b);
    fdtb_begin(b, "chosen");
    fdtb_prop_str(b, "bootargs", bootargs);
    fdtb_prop_u32(b, "linux,initrd-start", 0x48100000u);
    fdtb_prop_u32(b, "linux,initrd-end", 0x48180000u);
    fdtb_end(b);
    fdtb_begin(b, "memory@40000000");
    fdtb_prop_str(b, "device_type", "memory");
    fdtb_prop_cells2(b, "reg", 0x40000000u, 0x20000000u);
    fdtb_end(b);
    fdtb_end(b);
    return fdtb_finish(b);
}

#endif
