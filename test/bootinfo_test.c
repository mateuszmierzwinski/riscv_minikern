#include "core/bootinfo.h"

#include "fdt_build.h"

#include <stdint.h>

#include "core/errno.h"
#include "ktest.h"

static void test_clamps_to_board(void) {
    struct bootinfo info = {.mem_size = 0x40000000u};
    bootinfo_clamp_memory(&info, 0x20000000u);
    KTEST_CHECK(info.mem_size == 0x20000000u);
    info.mem_size = 0;
    bootinfo_clamp_memory(&info, 0x20000000u);
    KTEST_CHECK(info.mem_size == 0x20000000u);
    info.mem_size = 0x10000000u;
    bootinfo_clamp_memory(&info, 0x20000000u);
    KTEST_CHECK(info.mem_size == 0x10000000u);
}

/* The only boot protocol there is here: a device tree, with two cells for
 * an address and two for a size. The ATAG list the ARM kernel also had to
 * understand — and the code that told the two apart — is gone. */
static void test_reads_a_riscv_device_tree(void) {
    static struct fdtb b;
    struct bootinfo info;
    uint32_t len = fdtb_riscv_board(&b, "console=sbi minikern.halt=poweroff", 0x40000000ull);
    KTEST_CHECK(bootinfo_from_fdt(b.blob, len, &info) == KERR_OK);
    KTEST_CHECK(info.source == BOOTINFO_FDT);
    KTEST_CHECK(info.mem_base == 0x40000000ull);
    KTEST_CHECK(info.mem_size == 0x40000000ull);
    KTEST_CHECK_STR(info.cmdline, "console=sbi minikern.halt=poweroff");
}

/* **Eight gibibytes**, which is a VisionFive 2 configuration and the case
 * that broke the reader this grew from: a two-cell value whose high word is
 * non-zero was rejected outright, so the memory node of a board like this
 * would have been refused and the kernel would have booted believing it had
 * no memory at all. 0x4000_0000 + 0x2_0000_0000 also proves the sum does
 * not wrap, which it would in 32 bits. */
static void test_reads_eight_gibibytes(void) {
    static struct fdtb b;
    struct bootinfo info;
    uint32_t len = fdtb_riscv_board(&b, "x=y", 0x200000000ull);
    KTEST_CHECK(bootinfo_from_fdt(b.blob, len, &info) == KERR_OK);
    KTEST_CHECK(info.mem_base == 0x40000000ull);
    KTEST_CHECK(info.mem_size == 0x200000000ull);
    KTEST_CHECK(info.mem_base + info.mem_size == 0x240000000ull);
}

/* Anything that is not a tree is refused rather than parsed hopefully: the
 * firmware hands over one pointer and a kernel that trusts it blindly reads
 * whatever happens to be in memory. */
static void test_a_blob_that_is_not_a_tree_is_refused(void) {
    static const uint8_t rubbish[64] = {0xde, 0xad, 0xbe, 0xef};
    struct bootinfo info;
    KTEST_CHECK(bootinfo_from_fdt(rubbish, sizeof rubbish, &info) != KERR_OK);
    KTEST_CHECK(bootinfo_from_fdt(rubbish, 0, &info) != KERR_OK);
}

int main(void) {
    KTEST_RUN(test_clamps_to_board);
    KTEST_RUN(test_reads_a_riscv_device_tree);
    KTEST_RUN(test_reads_eight_gibibytes);
    KTEST_RUN(test_a_blob_that_is_not_a_tree_is_refused);
    return ktest_failures();
}
