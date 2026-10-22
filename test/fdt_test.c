#include "core/fdt.h"

#include "core/bootinfo.h"
#include "core/errno.h"
#include "fdt_build.h"
#include "ktest.h"

static struct fdtb b;

static void test_opens_and_finds(void) {
    uint32_t size = fdtb_neo(&b, "console=uart0");
    struct fdt f;
    struct fdt_prop p;
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_OK);
    KTEST_CHECK(fdt_get_prop(&f, "/chosen", "bootargs", &p) == KERR_OK);
    KTEST_CHECK_STR((const char *)p.value, "console=uart0");
    KTEST_CHECK(fdt_get_u32(&f, "/", "#address-cells", 9) == 1);
    KTEST_CHECK(fdt_get_u32(&f, "/cpus/cpu@0", "reg", 9) == 0);
    KTEST_CHECK(fdt_get_u32(&f, "/cpus/cpu", "reg", 9) == 0);
}

static void test_absent_is_noent(void) {
    uint32_t size = fdtb_neo(&b, "x");
    struct fdt f;
    struct fdt_prop p;
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_OK);
    KTEST_CHECK(fdt_get_prop(&f, "/chosen", "nothing", &p) == KERR_NOENT);
    KTEST_CHECK(fdt_get_prop(&f, "/nowhere", "reg", &p) == KERR_NOENT);
    KTEST_CHECK(fdt_get_prop(&f, "/cpus", "reg", &p) == KERR_NOENT);   /* reg is on cpu@0, not cpus */
    KTEST_CHECK(fdt_get_prop(&f, "/mem", "reg", &p) == KERR_NOENT);    /* no prefix matching */
    KTEST_CHECK(fdt_get_prop(&f, "chosen", "bootargs", &p) == KERR_INVAL);
    KTEST_CHECK(fdt_get_u32(&f, "/", "nothing", 7) == 7);
}

static void test_bootinfo_reads_neo(void) {
    uint32_t size = fdtb_neo(&b, "console=uart0 minikern.halt=poweroff");
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_fdt(b.blob, size, &info) == KERR_OK);
    KTEST_CHECK(info.mem_base == 0x40000000u && info.mem_size == 0x20000000u);
    KTEST_CHECK_STR(info.cmdline, "console=uart0 minikern.halt=poweroff");
    KTEST_CHECK(info.initrd_start == 0x48100000u && info.initrd_end == 0x48180000u);
    KTEST_CHECK(info.block_size == size);
}

static void test_bad_header_is_rejected(void) {
    uint32_t size = fdtb_neo(&b, "x");
    struct fdt f;
    KTEST_CHECK(fdt_open(&f, b.blob, size - 1) == KERR_INVAL);  /* totalsize beyond the bound */
    KTEST_CHECK(fdt_open(&f, b.blob, 39) == KERR_INVAL);
    KTEST_CHECK(fdt_open(&f, NULL, 4096) == KERR_INVAL);
    fdtb_put32(b.blob + 36, 0x7fffffffu);  /* structure block past the end */
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_INVAL);
    size = fdtb_neo(&b, "x");
    fdtb_put32(b.blob + 32, 0xfffffff0u);  /* strings block wraps */
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_INVAL);
    size = fdtb_neo(&b, "x");
    fdtb_put32(b.blob, 0xd00dfeeeu);
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_INVAL);
}

/* Truncating the structure block at every length must never read past it. */
static void test_truncated_struct_is_rejected(void) {
    uint32_t full = fdtb_neo(&b, "console=uart0");
    uint32_t slen = b.slen;
    struct fdt f;
    struct fdt_prop p;
    for (uint32_t cut = 0; cut < slen; cut += 4) {
        fdtb_neo(&b, "console=uart0");
        fdtb_put32(b.blob + 36, cut);
        KTEST_CHECK(fdt_open(&f, b.blob, full) == KERR_OK);
        KTEST_CHECK(fdt_get_prop(&f, "/memory", "missing", &p) == KERR_INVAL);
    }
}

static void test_hostile_lengths_are_rejected(void) {
    uint32_t size = fdtb_neo(&b, "x");
    struct fdt f;
    struct fdt_prop p;
    /* first prop of root: token at struct+8 (begin "" + pad), len at +12 */
    fdtb_put32(b.blob + 56 + 12, 0xfffffffcu);
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_OK);
    KTEST_CHECK(fdt_get_prop(&f, "/chosen", "bootargs", &p) == KERR_INVAL);
    size = fdtb_neo(&b, "x");
    fdtb_put32(b.blob + 56 + 16, 0x10000u);  /* name offset outside strings */
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_OK);
    KTEST_CHECK(fdt_get_prop(&f, "/chosen", "bootargs", &p) == KERR_INVAL);
}

static void test_unbalanced_and_deep_trees_are_rejected(void) {
    struct fdt f;
    struct fdt_prop p;
    uint32_t size;
    memset(&b, 0, sizeof b);
    fdtb_end(&b);  /* END_NODE with nothing open */
    size = fdtb_finish(&b);
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_OK);
    KTEST_CHECK(fdt_get_prop(&f, "/", "x", &p) == KERR_INVAL);
    memset(&b, 0, sizeof b);
    for (int i = 0; i < 40; i++) {
        fdtb_begin(&b, "n");
    }
    size = fdtb_finish(&b);
    KTEST_CHECK(fdt_open(&f, b.blob, size) == KERR_OK);
    KTEST_CHECK(fdt_get_prop(&f, "/", "x", &p) == KERR_INVAL);
}

static void test_cells(void) {
    uint8_t v[8] = {0, 0, 0, 1, 0x40, 0, 0, 0};
    struct fdt_prop p = {.value = v, .len = 8};
    uint64_t out = 0;
    /* A two-cell value is **64 bits**, high word included. The reader this
     * grew from refused a non-zero high word, which was safe on a 512 MiB
     * ARM board and would reject the memory node of an 8 GiB VisionFive 2. */
    KTEST_CHECK(fdt_prop_cells(&p, 0, 2, &out) == KERR_OK && out == 0x140000000ull);
    KTEST_CHECK(fdt_prop_cells(&p, 1, 1, &out) == KERR_OK && out == 0x40000000u);
    KTEST_CHECK(fdt_prop_cells(&p, 1, 2, &out) == KERR_INVAL);  /* runs past the value */
    KTEST_CHECK(fdt_prop_cells(&p, 0, 3, &out) == KERR_INVAL);
    KTEST_CHECK(fdt_prop_cells(&p, 0x40000001u, 1, &out) == KERR_INVAL);  /* at*4 overflows */
    v[0] = v[1] = v[2] = v[3] = 0;
    KTEST_CHECK(fdt_prop_cells(&p, 0, 2, &out) == KERR_OK && out == 0x40000000u);
}

static void test_unterminated_bootargs_ignored(void) {
    uint32_t size;
    struct bootinfo info;
    memset(&b, 0, sizeof b);
    fdtb_begin(&b, "");
    fdtb_begin(&b, "chosen");
    fdtb_prop(&b, "bootargs", "abcd", 4);
    fdtb_end(&b);
    fdtb_end(&b);
    size = fdtb_finish(&b);
    KTEST_CHECK(bootinfo_from_fdt(b.blob, size, &info) == KERR_OK);
    KTEST_CHECK_STR(info.cmdline, "");
    KTEST_CHECK(info.mem_size == 0 && info.initrd_start == 0);
}

/* ── finding a device, which is how every driver after the console binds ── */

/* A device is found by what it says it is, not by where it is. The same
 * kernel then runs on an emulator and on silicon with different addresses,
 * which is the whole reason for doing it this way. */
static void test_finds_a_device_by_compatible(void) {
    static struct fdtb b;
    struct fdt f;
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    uint32_t len = fdtb_board_with_uart(&b, 0x10000000ull, 0x100ull);
    KTEST_CHECK(fdt_open(&f, b.blob, len) == KERR_OK);
    /* Either of the node's compatible strings finds it: a list is "most
     * specific first", and a generic driver binds on the generic name. */
    KTEST_CHECK(fdt_find_compatible(&f, "ns16550a", path, sizeof path) == KERR_OK);
    KTEST_CHECK_STR(path, "/soc/serial@10000000");
    KTEST_CHECK(fdt_find_compatible(&f, "snps,dw-apb-uart", path, sizeof path) == KERR_OK);
    KTEST_CHECK_STR(path, "/soc/serial@10000000");
    /* And the pin controller beside it, so the scan is not merely finding
     * the first node with a compatible property. */
    KTEST_CHECK(fdt_find_compatible(&f, "starfive,jh7110-sys-pinctrl", path, sizeof path) == KERR_OK);
    KTEST_CHECK_STR(path, "/soc/gpio@13040000");
    KTEST_CHECK(fdt_device_reg(&f, path, 0, &base, &size) == KERR_OK);
    KTEST_CHECK(base == 0x13040000ull && size == 0x10000ull);
}

static void test_a_device_that_is_not_there(void) {
    static struct fdtb b;
    struct fdt f;
    char path[64];
    uint32_t len = fdtb_board_with_uart(&b, 0x10000000ull, 0x100ull);
    KTEST_CHECK(fdt_open(&f, b.blob, len) == KERR_OK);
    KTEST_CHECK(fdt_find_compatible(&f, "nothing,at-all", path, sizeof path) == KERR_NOENT);
    /* A prefix of a real string is not a match: "ns16550" must not find
     * "ns16550a", or a driver binds to a part it does not understand. */
    KTEST_CHECK(fdt_find_compatible(&f, "ns16550", path, sizeof path) == KERR_NOENT);
    KTEST_CHECK(fdt_find_compatible(&f, "", path, sizeof path) == KERR_NOENT);
}

/* A path that does not fit is **refused**, not truncated: half a path names a
 * different node, and a driver that bound to it would drive the wrong
 * registers. */
static void test_a_path_too_long_is_refused(void) {
    static struct fdtb b;
    struct fdt f;
    char path[8];
    uint32_t len = fdtb_board_with_uart(&b, 0x10000000ull, 0x100ull);
    KTEST_CHECK(fdt_open(&f, b.blob, len) == KERR_OK);
    KTEST_CHECK(fdt_find_compatible(&f, "ns16550a", path, sizeof path) == KERR_NOMEM);
    KTEST_CHECK(fdt_find_compatible(&f, "ns16550a", path, 0) == KERR_INVAL);
}

/* A device above 4 GiB, which a two-cell address can describe and a 32-bit
 * reader could not. The JH7110's own peripherals are low, but a PCIe window
 * — which is where the NVMe slot lives — is not. */
static void test_a_device_above_four_gibibytes(void) {
    static struct fdtb b;
    struct fdt f;
    char path[64];
    phys_t base = 0;
    uint64_t size = 0;
    uint32_t len = fdtb_board_with_uart(&b, 0x900000000ull, 0x10000000ull);
    KTEST_CHECK(fdt_open(&f, b.blob, len) == KERR_OK);
    KTEST_CHECK(fdt_find_compatible(&f, "ns16550a", path, sizeof path) == KERR_OK);
    KTEST_CHECK(fdt_device_reg(&f, path, 0, &base, &size) == KERR_OK);
    KTEST_CHECK(base == 0x900000000ull && size == 0x10000000ull);
}

/* A board advertises a **row** of identical transport slots, most of them
 * empty, so a driver has to walk them and probe each. Getting the n-th match
 * right is therefore load-bearing: a scan that returns the first one every
 * time finds only the first device, and one that skips wrongly reads a
 * neighbour's registers. */
static void test_finds_the_nth_match(void) {
    static struct fdtb b;
    struct fdt f;
    char path[64];
    uint32_t len = fdtb_board_with_slots(&b, 4);
    KTEST_CHECK(fdt_open(&f, b.blob, len) == KERR_OK);
    KTEST_CHECK(fdt_find_compatible_nth(&f, "virtio,mmio", 0, path, sizeof path) == KERR_OK);
    KTEST_CHECK_STR(path, "/soc/virtio_mmio@10001000");
    KTEST_CHECK(fdt_find_compatible_nth(&f, "virtio,mmio", 1, path, sizeof path) == KERR_OK);
    KTEST_CHECK_STR(path, "/soc/virtio_mmio@10002000");
    KTEST_CHECK(fdt_find_compatible_nth(&f, "virtio,mmio", 3, path, sizeof path) == KERR_OK);
    KTEST_CHECK_STR(path, "/soc/virtio_mmio@10004000");
    /* Past the end is "not there", not the last one again. */
    KTEST_CHECK(fdt_find_compatible_nth(&f, "virtio,mmio", 4, path, sizeof path) == KERR_NOENT);
    /* And every one of them must yield its own registers — this is what a
     * driver does next, and the step that was failing when this was written. */
    for (uint32_t i = 0; i < 4u; i++) {
        phys_t base = 0;
        uint64_t size = 0;
        KTEST_CHECK(fdt_find_compatible_nth(&f, "virtio,mmio", i, path, sizeof path) == KERR_OK);
        KTEST_CHECK(fdt_device_reg(&f, path, 0, &base, &size) == KERR_OK);
        KTEST_CHECK(base == 0x10001000ull + (uint64_t)i * 0x1000ull);
        KTEST_CHECK(size == 0x200ull);
    }
}

int main(void) {
    KTEST_RUN(test_opens_and_finds);
    KTEST_RUN(test_absent_is_noent);
    KTEST_RUN(test_bootinfo_reads_neo);
    KTEST_RUN(test_bad_header_is_rejected);
    KTEST_RUN(test_truncated_struct_is_rejected);
    KTEST_RUN(test_hostile_lengths_are_rejected);
    KTEST_RUN(test_unbalanced_and_deep_trees_are_rejected);
    KTEST_RUN(test_cells);
    KTEST_RUN(test_unterminated_bootargs_ignored);
    KTEST_RUN(test_finds_a_device_by_compatible);
    KTEST_RUN(test_a_device_that_is_not_there);
    KTEST_RUN(test_a_path_too_long_is_refused);
    KTEST_RUN(test_a_device_above_four_gibibytes);
    KTEST_RUN(test_finds_the_nth_match);
    return ktest_failures();
}
