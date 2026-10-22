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
#include "arch/riscv/plic.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/sbi.h"
#include "arch/riscv/trap.h"
#include "board/jh7110/console.h"
#include "board/jh7110/selftest.h"
#include "board/jh7110/uart.h"
#include "drivers/crg_jh7110.h"
#include "drivers/gpio_jh7110.h"
#include "drivers/nvme.h"
#include "drivers/pci.h"
#include "drivers/virtio_blk.h"
#include "board/jh7110/version.h"
#include "core/bootinfo.h"
#include "core/cmdline.h"
#include "core/console.h"
#include "core/errno.h"
#include "core/fdt.h"
#include "kernel/disk.h"
#include "kernel/irq.h"
#include "kernel/mem.h"
#include "kernel/xfsmount.h"
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

/* Ticks a second. 100 is enough to schedule by and cheap enough that the
 * handler's cost is noise; the ARM kernel used 1000 and spent measurably
 * more time in the tick than in the work it was timing. */
#define TICK_HZ 100u

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

/* The frame allocator and the heap, over the memory that is left.
 *
 * After translation, because the arena the page tables came from has to be
 * reserved and its size is only known once they are built. Every frame
 * starts out used and the free ones are released, so a reservation nobody
 * wrote loses memory rather than handing out memory in use (kernel/mem.h). */
static void bring_up_memory(const struct bootinfo *info) {
    uint64_t arena = ((uint64_t)(uintptr_t)__kernel_end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (mem_init(info, arena, TABLE_ARENA_BYTES) != KERR_OK) {
        kprintf("mem: no usable memory map\n");
        return;
    }
    kprintf("mem: %u of %u pages free (%u MiB of %u MiB)\n", mem_free_pages_count(),
            mem_total_pages(), mem_free_pages_count() / 256u, mem_total_pages() / 256u);
}

/* Takes the board's own UART over from the firmware's console.
 *
 * Deliberately after translation is on, because the UART's registers have to
 * be mapped — and deliberately **not** fatal: a board whose tree names no
 * 16550 keeps the SBI console, which is slower and works. A kernel that goes
 * silent because it could not find a better way to speak has made the
 * situation worse. */
static void adopt_the_real_uart(uint64_t dtb) {
    struct console_port port;
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (jh7110_uart_init(&fdt, &port) != KERR_OK) {
        kprintf("uart: no 16550 in the device tree; keeping the firmware's console\n");
        return;
    }
    console_attach(&port);
    kprintf("uart: %s at %016llx, taken over from sbi\n", jh7110_uart_path(),
            (unsigned long long)jh7110_uart_base());
}

/* The interrupt controller and the tick.
 *
 * Both come from the device tree: the PLIC's address and — the part nobody
 * can guess — **which context this hart's supervisor interrupts arrive in**,
 * which is a position in the controller's own list and not the hart id. The
 * timer's frequency likewise, because the counter's rate is a property of the
 * machine and nothing in the ISA states it.
 *
 * Interrupts are enabled last, deliberately: a kernel that opens the gate
 * before it has a handler for what comes through it crashes in the one place
 * where it cannot explain itself. */
static void bring_up_interrupts(uint64_t dtb, uint64_t hart) {
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (plic_init(&fdt, hart) != KERR_OK) {
        kprintf("plic: not found in the device tree; devices cannot interrupt\n");
    } else {
        kprintf("plic: at %016llx, hart %llu uses context %u\n",
                (unsigned long long)plic_base(), (unsigned long long)hart, plic_context());
    }
    if (timer_init(&fdt, TICK_HZ) != KERR_OK) {
        kprintf("timer: the tree does not say the counter's frequency; no tick\n");
        return;
    }
    kprintf("timer: %llu Hz counter, %u ticks a second\n",
            (unsigned long long)timer_frequency(), timer_hz());
    trap_enable_interrupts();
}

/* Waits for the tick to prove itself.
 *
 * `minikern.ticks=<n>` waits for n timer interrupts and reports how long
 * they took by the counter, which is the only way to show that interrupts
 * are *arriving* rather than merely that the timer was armed. A tick that is
 * armed and never delivered looks identical in a boot log — and on the
 * architecture this came from, that was the shape of a real bug: a core
 * whose interrupt controller it had not initialised ticked for ever without
 * ever entering the kernel.
 *
 * It gives up rather than hanging, because a kernel that waits for ever for
 * an interrupt tells you nothing about why it never came. */
static void wait_for_ticks(void) {
    size_t len = 0;
    const char *v = cmdline_value(boot_cmdline, "minikern.ticks", &len);
    uint64_t want = 0, start_ns, deadline;
    if (v == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        want = v[i] >= '0' && v[i] <= '9' ? want * 10u + (uint64_t)(v[i] - '0') : want;
    }
    start_ns = timer_ns();
    /* Twice as long as it should take, by the counter — not by a spin count,
     * which measures the host's speed rather than the guest's clock. */
    deadline = timer_now() + 2u * want * (timer_frequency() / (timer_hz() == 0 ? 1 : timer_hz()));
    while (timer_ticks() < want && timer_now() < deadline) {
        __asm__ volatile("wfi");
    }
    if (timer_ticks() < want) {
        kprintf("tick: only %llu of %llu ticks arrived — interrupts are not being delivered\n",
                (unsigned long long)timer_ticks(), (unsigned long long)want);
        return;
    }
    kprintf("tick: %llu ticks in %llu ms, %llu irqs dispatched, %llu spurious\n",
            (unsigned long long)timer_ticks(), (unsigned long long)((timer_ns() - start_ns) / 1000000u),
            (unsigned long long)irq_count(), (unsigned long long)irq_spurious());
}

/* Storage, and a round trip through it if asked.
 *
 * A machine with no disk is the ordinary case for a kernel that netboots, so
 * that is reported once and is not a failure. `minikern.disktest` writes a
 * pattern to the **last** sector, reads it back and restores the original —
 * the last, because a filesystem lives at the beginning and a test that
 * scribbles on sector 0 destroys what the next test wants to read. */
static void bring_up_storage(uint64_t dtb) {
    struct fdt fdt;
    const struct blockdev *dev;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (disk_attach(&fdt) != KERR_OK) {
        kprintf("disk: none attached\n");
        if (pci_ready() && cmdline_has(boot_cmdline, "minikern.pciscan")) {
            pci_list();
        }
        return;
    }
    dev = disk_device();
    if (pci_ready()) {
        kprintf("pci: ecam at %016llx, %u functions\n", (unsigned long long)pci_ecam_base(),
                pci_functions_seen());
        if (cmdline_has(boot_cmdline, "minikern.pciscan")) {
            pci_list();
        }
    }
    kprintf("disk: %s %s, %llu sectors of %u bytes (%llu MiB)\n", disk_kind(),
            disk_kind()[0] == 'n' ? nvme_model()
            : disk_kind()[0] == 'S' ? "card"
                                    : virtio_blk_path(),
            (unsigned long long)dev->blocks, dev->block_size,
            (unsigned long long)(dev->blocks * dev->block_size / (1024u * 1024u)));
    if (cmdline_has(boot_cmdline, "minikern.disktest")) {
        int rc = disk_selftest();
        kprintf("disk: round trip %s\n", rc == KERR_OK ? "ok" : "FAILED");
    }
}

/* The filesystem, if the disk has one.
 *
 * A disk with no XFS on it is not a failure — it is a blank disk, which is
 * what a board ships with — so each refusal is reported as the diagnosis it
 * is rather than as "mount failed". The implementation **refuses a dirty
 * log** rather than replaying it, and that refusal is the one most worth
 * naming: it means the filesystem was not unmounted cleanly and this kernel
 * will not guess at what was in flight (kernel/xfsmount.h). */
static void bring_up_filesystem(void) {
    int rc = xfsmount_attach();
    int32_t entries;
    if (rc == KERR_NODEV) {
        return; /* no disk: already said so */
    }
    if (rc != KERR_OK) {
        /* No paraphrase. The filesystem prints the precise reason it
         * refused — an unsupported feature bit, a dirty log, a bad magic —
         * and an earlier version of this line guessed a different reason
         * from the error code and printed "a dirty log" over the top of
         * "unsupported read-only features". Restating someone else's
         * diagnosis in your own words is how a log starts lying. */
        kprintf("xfs: not mounted (rc %d; the reason is above)\n", rc);
        return;
    }
    entries = xfsmount_list_root();
    if (entries < 0) {
        kprintf("xfs: mounted, but the root directory would not read (rc %d)\n", entries);
        return;
    }
    kprintf("xfs: mounted read-only, %d entries in /\n", entries);
    (void)xfsmount_show_file("/hello.txt");
}

/* The board's pins.
 *
 * Reports what it found and nothing more: driving a pin is something only a
 * board can be asked to do, and a kernel that pokes at pins on a machine
 * whose schematic it has not seen is a kernel that shorts something. Under
 * an emulator there is no pin controller at all, which is said rather than
 * passed over — "no GPIO" and "GPIO I failed to find" must not look the
 * same. */
static void bring_up_gpio(uint64_t dtb) {
    struct fdt fdt;
    uint32_t found;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    /* The clock generators first: a pin controller cannot be switched on
     * without them, and a driver that skips that step is the one the ARM
     * thermal sensor was (drivers/crg_jh7110.h). */
    if (crg_jh7110_init(&fdt) == 0) {
        kprintf("gpio: no JH7110 clock generator in the device tree\n");
        return;
    }
    kprintf("crg: sys at %016llx, aon at %016llx\n", (unsigned long long)crg_base(CRG_SYS),
            (unsigned long long)crg_base(CRG_AON));
    found = gpio_jh7110_init(&fdt);
    if (found == 0) {
        kprintf("gpio: no pin controller came up\n");
        return;
    }
    for (uint32_t i = 0; i < 2u; i++) {
        if (gpio_jh7110_path(i)[0] != '\0') {
            kprintf("gpio: %s at %016llx\n", gpio_jh7110_path(i),
                    (unsigned long long)gpio_jh7110_base(i));
        }
    }
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
    adopt_the_real_uart(dtb);
    bring_up_memory(&info);
    bring_up_interrupts(dtb, hart);
    bring_up_storage(dtb);
    bring_up_filesystem();
    bring_up_gpio(dtb);
    wait_for_ticks();
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
