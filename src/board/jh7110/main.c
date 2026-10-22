/* main — the composition root.
 *
 * The one place in the kernel that names concrete adapters: it takes what
 * the firmware passed, builds the ports the core needs, and hands over. The
 * core (src/core) includes nothing from arch/ or board/ and never will.
 *
 * What the firmware gives us is two registers, and both matter:
 *
 *   a0  the hart id. **Not necessarily 0.** QEMU's virt machine picks a boot
 *       hart by lottery, and a machine with a monitor core alongside its
 *       application cores — sifive_u, and the JH7110 with its S7 beside four
 *       U74s — boots on hart 1. The kernel records what it was told and
 *       never assumes.
 *   a1  the physical address of a device tree, which is the only description
 *       of the machine there is. There is no ATAG alternative to tell apart.
 */
#include <stdint.h>

#include "arch/riscv/layout.h"
#include "arch/riscv/mmu.h"
#include "arch/riscv/sbi.h"
#include "arch/riscv/trap.h"
#include "board/jh7110/console.h"
#include "board/jh7110/selftest.h"
#include "board/jh7110/version.h"
#include "core/bootinfo.h"
#include "core/cmdline.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/ports.h"

#include <stdbool.h>

/* The device tree is read where the firmware put it, with a bound: a blob
 * that claims to be larger than this is refused rather than trusted. */
#define FDT_MAX_BYTES (2u * 1024u * 1024u)

static const char *boot_cmdline = "";

/* Says what we are, before anything can go wrong and leave nothing on the
 * wire. The hart id is in the banner because it is the one fact that
 * differs between machines that otherwise look identical. */
static void banner(uint64_t hart) {
    kprintf("\nminikern %s (RISC-V 64, rv64gc), hart %llu\n", MINIKERN_VERSION,
            (unsigned long long)hart);
}

/* What the firmware is willing to do for us. Probed rather than assumed:
 * a kernel that takes SRST for granted and finds it absent cannot even
 * power the machine off, and one that takes HSM for granted brings up no
 * other cores and says nothing about why. */
static void report_firmware(void) {
    kprintf("sbi: timer %s, hart-state %s, reset %s, debug-console %s\n",
            sbi_probe_extension(SBI_EXT_TIME) ? "yes" : "no",
            sbi_probe_extension(SBI_EXT_HSM) ? "yes" : "no",
            sbi_probe_extension(SBI_EXT_SRST) ? "yes" : "no",
            sbi_probe_extension(SBI_EXT_DBCN) ? "yes" : "no");
}

/* Reads the machine's own description. Everything after this point knows
 * how much memory there is and what the operator asked for. */
static void read_bootinfo(uint64_t dtb, struct bootinfo *info) {
    int rc = bootinfo_from_fdt((const void *)(uintptr_t)dtb, FDT_MAX_BYTES, info);
    if (rc != KERR_OK || info->source != BOOTINFO_FDT) {
        kprintf("boot: no usable device tree at %016llx (rc %d)\n", (unsigned long long)dtb, rc);
        return;
    }
    boot_cmdline = info->cmdline != NULL ? info->cmdline : "";
    kprintf("boot: device tree at %016llx, %u bytes\n", (unsigned long long)dtb, info->block_size);
    kprintf("mem: %llu MiB at %016llx\n", (unsigned long long)(info->mem_size / (1024u * 1024u)),
            (unsigned long long)info->mem_base);
    if (boot_cmdline[0] != '\0') {
        kprintf("cmdline: \"%s\"\n", boot_cmdline);
    }
}

/* Where the table allocator may carve its pages from: immediately above the
 * kernel image. Two mebibytes is far more than the identity map needs (it
 * uses gigapages where it can), and it is reclaimed by nothing because the
 * kernel's own map lives as long as the machine. */
#define TABLE_ARENA_BYTES (2u * 1024u * 1024u)

extern char __kernel_end[];

/* Turns translation on, and says what it cost.
 *
 * Deliberately after the trap vector is installed: if the new table were
 * wrong in a way that faults rather than one that stops fetching, a handler
 * exists to say so. And deliberately before the self-tests, so those faults
 * are taken with paging on. */
static void bring_up_translation(const struct bootinfo *info) {
    uint64_t arena = ((uint64_t)(uintptr_t)__kernel_end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    int rc;
    if (info->source != BOOTINFO_FDT || info->mem_size == 0) {
        kprintf("mmu: no memory map, so translation stays off\n");
        return;
    }
    mmu_set_table_arena(arena, TABLE_ARENA_BYTES);
    rc = mmu_init(info->mem_base, info->mem_size);
    if (rc != KERR_OK) {
        kprintf("mmu: could not build the kernel map (rc %d); translation stays off\n", rc);
        return;
    }
    kprintf("mmu: sv39 on, %u table pages, kernel text read-execute\n", mmu_table_pages_used());
}

/* True when the operator asked the machine to switch itself off at the end,
 * which is what makes a boot test a test rather than a timeout. */
static bool halt_requested(void) {
    return cmdline_equals(boot_cmdline, "minikern.halt", "poweroff");
}

void kmain(uint64_t hart, uint64_t dtb) {
    struct bootinfo info = {0};
    struct console_port con = jh7110_sbi_console();
    console_attach(&con);
    banner(hart);
    report_firmware();
    read_bootinfo(dtb, &info);
    /* Before anything can fault by accident: this hart's trap vector. It is
     * per hart, and a hart that skips it sends its first exception to
     * address zero. */
    trap_init_cpu();
    bring_up_translation(&info);
    (void)selftest_run(boot_cmdline);
    kprintf("minikern: boot complete\n");
    if (halt_requested()) {
        kprintf("minikern: powering off\n");
        sbi_shutdown();
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
