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
#include "core/klog.h"
#include "kernel/irqwait.h"
#include "kernel/file.h"
#include "kernel/fdobjects.h"
#include "lib/kfmt.h"
#include "core/errno.h"
#include "core/fdt.h"
#include "kernel/disk.h"
#include "kernel/irq.h"
#include "kernel/mem.h"
#include "arch/riscv/fp.h"
#include "arch/riscv/smp.h"
#include "drivers/dwmmc.h"
#include "drivers/virtio_net.h"
#include "kernel/klock.h"
#include "kernel/net.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/swap.h"
#include "kernel/tty.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"
#include "kernel/uspace.h"
#include "kernel/probe.h"
#include "drivers/dwmac.h"
#include "kernel/procfs.h"
#include "kernel/wallclock.h"
#include "drivers/temp_jh7110.h"
#include "drivers/xhci.h"
#include "drivers/wdt_jh7110.h"
#include "drivers/pwm_jh7110.h"
#include "drivers/i2c_dw.h"
#include "drivers/spi_pl022.h"
#include "drivers/dwmac_probe.h"
#include "kernel/volumes.h"
#include "lib/sha256.h"
#include "kernel/probe.h"
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
    kprintf("sbi: timer %s, hart-state %s, reset %s, debug-console %s, remote-fence %s\n",
            sbi_probe_extension(SBI_EXT_TIME) ? "yes" : "no",
            sbi_probe_extension(SBI_EXT_HSM) ? "yes" : "no",
            sbi_probe_extension(SBI_EXT_SRST) ? "yes" : "no",
            sbi_probe_extension(SBI_EXT_DBCN) ? "yes" : "no",
            /* **The one whose absence is silent.** Without RFENCE a
             * mapping removed on one hart stays in another hart's TLB, and
             * a stale translation does not fault — it reads the wrong page.
             * Every other extension here announces itself by failing
             * loudly; this one announces itself by corrupting memory, so it
             * is worth a word in the boot log. */
            mmu_can_flush_remotely() ? "yes" : "**NO**");
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
/* A program the boot loader fetched into DRAM, as `<address>:<length>`.
 *
 * **The way a new test program reaches the board without a new SD card.**
 * U-Boot already has a network — it is how the kernel image itself arrives —
 * so it can `wget` a second file just as easily, and the kernel can build a
 * process from a buffer (`uspace_build_elf` takes one; reading a file is
 * only how the filesystem path gets there). That removes the dependency this
 * hunt was stuck behind: the fast reproducer is a Go binary, the card is the
 * only filesystem, and writing to the card is the operator's to do.
 *
 * Both numbers are hexadecimal, which `cmdline_number` reads since the
 * `minikern.isolate` fix. The range is **reserved before anything is
 * allocated**, because the frame allocator would otherwise hand out the
 * very memory the program is sitting in. */
static phys_t loaded_at;
static uint64_t loaded_bytes;
static phys_t disk_image_at;
static uint64_t disk_image_bytes;

/* An `<address>:<length>` pair from the command line, both hexadecimal.
 *
 * Shared by the two things the boot loader can leave in memory — a program
 * and a filesystem image — because they differ only in what the kernel does
 * with the bytes. A bare address is refused: without a length there is
 * nothing to reserve and nothing to bound a read by, and guessing either is
 * how a plausible-looking range turns into a fault somewhere else. */
static bool find_a_loaded_range(const char *key, phys_t *at, uint64_t *bytes) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, key, &len);
    size_t n = 0;
    if (spec == NULL || len == 0) {
        return false;
    }
    while (n < len && spec[n] != ':') {
        n++;
    }
    if (n == len) {
        return false;
    }
    *at = (phys_t)cmdline_number_in(spec, n, 0);
    *bytes = cmdline_number_in(spec + n + 1u, len - n - 1u, 0);
    if (*at == 0 || *bytes == 0) {
        *at = 0;
        *bytes = 0;
        return false;
    }
    return true;
}

static void bring_up_memory(const struct bootinfo *info) {
    /* **The physical end of the image, asked for rather than computed.**
     * `__kernel_end` reads as a high address once the kernel is running in
     * its own half, and an arena placed from it is nowhere near RAM. */
    phys_t image_at = 0;
    uint64_t image_size = 0;
    uint64_t arena;
    mmu_kernel_phys_range(&image_at, &image_size);
    arena = image_at == 0
                ? (((uint64_t)(uintptr_t)__kernel_end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u))
                : ((image_at + image_size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u));
    if (mem_init(info, arena, TABLE_ARENA_BYTES) != KERR_OK) {
        kprintf("mem: no usable memory map\n");
        return;
    }
    if (find_a_loaded_range("minikern.loadat", &loaded_at, &loaded_bytes)) {
        mem_reserve_range(loaded_at, loaded_bytes);
        kprintf("init: a program is already in memory at %016llx, %llu bytes\n",
                (unsigned long long)loaded_at, (unsigned long long)loaded_bytes);
    }
    if (find_a_loaded_range("minikern.diskat", &disk_image_at, &disk_image_bytes)) {
        mem_reserve_range(disk_image_at, disk_image_bytes);
        disk_use_ram(disk_image_at, disk_image_bytes);
        kprintf("disk: a filesystem image is in memory at %016llx, %llu bytes\n",
                (unsigned long long)disk_image_at, (unsigned long long)disk_image_bytes);
    }
    kprintf("mem: %u of %u pages free (%u MiB of %u MiB)\n", mem_free_pages_count(),
            mem_total_pages(), mem_free_pages_count() / 256u, mem_total_pages() / 256u);
}

/* Arms the SD controller's interrupt, if that is the disk in use.
 *
 * A wrapper rather than a direct call, because the disk may be the NVMe
 * drive or a RAM image — and arming an interrupt for a controller nothing
 * is reading would install a handler for a device that will never raise. */
static int dwmmc_use_interrupts_if_present(uint64_t dtb) {
    const char *kind = disk_present() ? disk_kind() : NULL;
    /* `disk_kind` is the probe's own word for what it attached, which is
     * the only thing that distinguishes the card from the NVMe drive or a
     * RAM image here. */
    if (kind == NULL || kind[0] != 's' || kind[1] != 'd') {
        return KERR_NODEV;
    }
    {
        struct fdt fdt;
        if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
            return KERR_INVAL;
        }
        return dwmmc_use_interrupts(&fdt);
    }
}

/* The same for the NVMe drive, which is a separate call because the two
 * devices are never both the attached disk and each reads its interrupt from
 * a different place: the card's is its own node's, the drive's is the PCIe
 * host bridge's. */
static int nvme_use_interrupts_if_present(uint64_t dtb) {
    const char *kind = disk_present() ? disk_kind() : NULL;
    if (kind == NULL || kind[0] != 'n') {
        return KERR_NODEV;
    }
    {
        struct fdt fdt;
        if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
            return KERR_INVAL;
        }
        return nvme_use_interrupts(&fdt);
    }
}

/* Writes the boot's log to `/var/log/kernel.0.log`, rotating what is there.
 *
 * **Why the kernel does this itself.** Everything interesting about a boot
 * happens before there is a program to read a log, and on a board whose
 * console is a serial cable the only record is whoever was watching. Keeping
 * the lines in memory and writing them once there is a filesystem is the
 * difference between a log and a transcript.
 *
 * **Right after the root filesystem is mounted and before init**, which is
 * the only window where it is both possible and complete: before the mount
 * there is nowhere to write, and after init the log would be missing
 * whatever init did to earn the attention of whoever is reading it.
 *
 * Rotation runs **downwards from the oldest**: `kernel.8.log` becomes
 * `kernel.9.log`, then 7 becomes 8, and so on to 0 becoming 1, so that
 * nothing is overwritten before it has been copied. This boot is always
 * `.0`. Ten boots is enough to compare a failure against the last time it
 * worked, which is what anybody actually does with these.
 *
 * **Each move is logged**, which is not ceremony: the log that records the
 * rotation is the log being rotated, so those lines are the only evidence
 * afterwards of which boot went where. A reader looking at `kernel.3.log`
 * can see it was once `.2` and when.
 *
 * `/var/log` is **not created** if it is missing, because this writer has no
 * `mkdir`. The log then stays in memory and says so — a kernel that wrote
 * its log into the root directory instead would be putting its files among
 * a user's, which is worse than a clear refusal.
 */
static void put_the_boot_log_on_disk(void) {
    static char text[KLOG_BYTES];
    char name[24];
    uint32_t n;
    int rc;
    if (klog_entries() == 0) {
        return;
    }
    /* Rotate downwards from the oldest, so that nothing is overwritten
     * before it has been copied. Reading each one back costs a little and
     * means the rotation cannot lose a boot to a failed write halfway. */
    {
        uint32_t moved = 0;
        for (uint32_t i = KLOG_KEEP_BOOTS; i-- > 1u;) {
            uint8_t *older = NULL;
            uint32_t size = 0;
            char from[32], to[24];
            kfmt_snprintf(from, sizeof from, "/var/log/kernel.%u.log", i - 1u);
            kfmt_snprintf(to, sizeof to, "kernel.%u.log", i);
            if (xfsmount_read_file(from, (uint32_t)sizeof text, &older, &size) != KERR_OK) {
                continue; /* that boot was never written; nothing to move */
            }
            rc = xfsmount_write_into("/var/log", to, older, size);
            kfree(older);
            if (rc != KERR_OK) {
                /* Said rather than skipped: a rotation that half happened
                 * leaves two boots claiming the same number, and the next
                 * reader needs to know which one lied. */
                klog(KLOG_WARN, "klog: kernel.%u.log could not become kernel.%u.log (rc %d)\n",
                     i - 1u, i, rc);
                continue;
            }
            klog(KLOG_INFO, "klog: rotated kernel.%u.log to kernel.%u.log, %u bytes\n", i - 1u, i,
                 size);
            moved++;
        }
        klog(KLOG_NOTICE, "klog: %u earlier boot%s rotated; this one becomes kernel.0.log\n",
             moved, moved == 1u ? "" : "s");
    }
    /* **Made if it is not there**, rather than reported as missing: a log
     * the kernel keeps and cannot store is a log nobody reads. Every
     * component, because a fresh filesystem has neither `/var` nor
     * `/var/log`, and an existing one is not an error. */
    rc = xfsmount_mkdir_p("/var/log");
    if (rc != KERR_OK) {
        kprintf("klog: /var/log could not be made (rc %d); the log stays in memory\n", rc);
        return;
    }
    /* **The snapshot is taken after the rotation**, so that this boot's
     * own file contains the lines describing where the others went. Taken
     * before, `kernel.0.log` would be the one file that cannot say what the
     * rotation did. */
    n = klog_all(text, (uint32_t)sizeof text);
    kfmt_snprintf(name, sizeof name, "kernel.0.log");
    rc = xfsmount_write_into("/var/log", name, text, n);
    if (rc != KERR_OK) {
        /* Named, with the reason, because "could not write the log" sends
         * the reader to guess between a missing directory, a read-only
         * mount and a full disk. */
        kprintf("klog: /var/log/%s could not be written (rc %d); the log stays in memory\n", name,
                rc);
        return;
    }
    kprintf("klog: %u bytes of this boot written to /var/log/%s%s\n", n, name,
            klog_dropped() > 0 ? " (the start was dropped: the buffer filled)" : "");
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
    /* `minikern.uart=sbi` keeps the firmware's console and never touches the
     * hardware. It exists because on a real board the two are not equally
     * trustworthy: the firmware's console is known to work — it printed the
     * banner — and this driver is the new thing. When output turned to
     * fragments on first contact with a VisionFive 2, this was what
     * separated "the kernel is stuck" from "the kernel is fine and cannot
     * be heard". */
    if (cmdline_has(boot_cmdline, "minikern.uart=sbi")) {
        kprintf("uart: keeping the firmware's console (minikern.uart=sbi)\n");
        return;
    }
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
    uint64_t want = 0, start_ns, deadline, base;
    if (v == NULL) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        want = v[i] >= '0' && v[i] <= '9' ? want * 10u + (uint64_t)(v[i] - '0') : want;
    }
    start_ns = timer_ns();
    /* **From a baseline**, not from the counter's absolute value: ticks have
     * been arriving since the timer was initialised, and anything that runs
     * before this — bringing up a disk, starting the other harts — takes
     * time that shows up as ticks already counted. The absolute value is
     * therefore a measure of how long the boot took, where what is wanted is
     * the rate. */
    base = timer_ticks();
    /* Twice as long as it should take, by the counter — not by a spin count,
     * which measures the host's speed rather than the guest's clock. */
    deadline = timer_now() + 2u * want * (timer_frequency() / (timer_hz() == 0 ? 1 : timer_hz()));
    while (timer_ticks() - base < want && timer_now() < deadline) {
        __asm__ volatile("wfi");
    }
    if (timer_ticks() - base < want) {
        kprintf("tick: only %llu of %llu ticks arrived — interrupts are not being delivered"
                " (hart %llu index %u, %llu ticks across all harts)\n",
                (unsigned long long)(timer_ticks() - base), (unsigned long long)want,
                (unsigned long long)smp_this_hart(), smp_hart_index(),
                (unsigned long long)timer_ticks_all_harts());
        return;
    }
    kprintf("tick: %llu ticks in %llu ms, %llu irqs dispatched, %llu spurious\n",
            (unsigned long long)(timer_ticks() - base),
            (unsigned long long)((timer_ns() - start_ns) / 1000000u),
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
    size_t kindlen = 0;
    const char *kind = cmdline_value(boot_cmdline, "minikern.disk", &kindlen);
    dwmmc_set_trace(cmdline_has(boot_cmdline, "minikern.mmctrace"));
    disk_prefer(kind, (uint32_t)kindlen);
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
static bool parse_pin(const char *spec, size_t len, uint32_t *at, uint32_t *first,
                      uint32_t *second);
static void checksum_a_file(void);

/* Reads sequentially from the attached disk and reports what it managed.
 *
 * `minikern.diskbench=<mebibytes>:<blocks per read>` — the second number is
 * the point. A driver that issues one command per block and one that issues
 * one command per sixteen do the same work at the device and a very
 * different amount of work around it, and the only way to know which
 * dominates is to vary it on the hardware rather than reason about it.
 *
 * Reads only, and nothing is written: a benchmark that writes to the
 * operator's disk to find out how fast it is has misunderstood the
 * question. */
static uint8_t bench_buffer[64u * 1024u] __attribute__((aligned(4096)));

static void run_disk_benchmark(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.diskbench", &len);
    uint32_t at = 0, mebibytes = 0, per_read = 0;
    const struct blockdev *dev = disk_device();
    uint64_t started, elapsed_ms, blocks, done = 0;
    if (spec == NULL || !disk_present() || dev == NULL) {
        return;
    }
    if (!parse_pin(spec, len, &at, &mebibytes, &per_read) || mebibytes == 0) {
        return;
    }
    per_read = per_read == 0 ? 1u : per_read;
    if ((uint64_t)per_read * dev->block_size > sizeof bench_buffer) {
        per_read = (uint32_t)(sizeof bench_buffer / dev->block_size);
    }
    blocks = ((uint64_t)mebibytes << 20) / dev->block_size;
    blocks = blocks > dev->blocks ? dev->blocks : blocks;
    started = timer_ns();
    while (done < blocks) {
        uint32_t n = (uint64_t)per_read > blocks - done ? (uint32_t)(blocks - done) : per_read;
        if (dev->read(dev->self, done, n, bench_buffer) != KERR_OK) {
            kprintf("bench: read of %u blocks at %llu failed\n", n, (unsigned long long)done);
            return;
        }
        done += n;
    }
    elapsed_ms = (timer_ns() - started) / 1000000u;
    elapsed_ms = elapsed_ms == 0 ? 1u : elapsed_ms;
    kprintf("bench: %llu KiB in %llu ms, %llu KiB/s, %u blocks a read\n",
            (unsigned long long)((done * dev->block_size) / 1024u),
            (unsigned long long)elapsed_ms,
            (unsigned long long)((done * dev->block_size) / 1024u * 1000u / elapsed_ms),
            per_read);
}

/* Lists the virtual `/volumes` directory: what is mounted, and where.
 *
 * It is synthesised from the mount table rather than read from a disk —
 * there is no filesystem behind `/volumes`, which is the point of it. A
 * directory that exists because the kernel says so needs no block,
 * no inode and no space, and cannot be inconsistent with what is
 * actually mounted. */
static void list_the_volumes(void) {
    uint32_t n = volumes_count();
    kprintf("volumes: %u mounted\n", n);
    for (uint32_t i = 0; i < n; i++) {
        kprintf("volumes:   %s -> %s%s\n", volumes_at(i),
                volumes_label(i)[0] != '\0' ? volumes_label(i) : "(no label)",
                volumes_readonly(i) ? " (read-only)" : "");
    }
}

/* Mounts everything the disk offers, each where its label says.
 *
 * **Before the single-filesystem path below**, because that one is the
 * fallback: a kernel told which device to use (`minikern.disk=`) or handed a
 * RAM disk by a test still mounts it by hand. Automount is what a machine
 * does when nobody told it anything. */
static void automount_the_disk(void) {
    (void)volumes_automount();
}

/* Whether two NUL-terminated strings are the same. The kernel has no libc
 * and this is the only place in this file that needs one. */
static bool same_text(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/* Checks automount put each volume where its label says.
 *
 * **The decision, not the mounting.** That both filesystems mount is the
 * easy half and the old kernel managed it for one; what this asserts is
 * that the one labelled `ROOT` became `/` and the one labelled `DATA`
 * became `/volumes/DATA` — and, crucially, that a file on DATA is reachable
 * *only* through that path. A kernel that mounted both at `/` would pass a
 * test that merely counted mounts.
 *
 * `minikern.volumes` runs it. */
static void run_volumes_selftest(void) {
    bool ok = volumes_count() == 2u;
    const char *root = NULL, *data = NULL;
    for (uint32_t i = 0; i < volumes_count(); i++) {
        root = same_text(volumes_label(i), "ROOT") ? volumes_at(i) : root;
        data = same_text(volumes_label(i), "DATA") ? volumes_at(i) : data;
    }
    kprintf("volumes: ROOT at %s, DATA at %s\n", root != NULL ? root : "(absent)",
            data != NULL ? data : "(absent)");
    ok = ok && root != NULL && same_text(root, "/");
    ok = ok && data != NULL && same_text(data, "/volumes/DATA");
    /* Each volume's own file, through its own path. The names differ per
     * volume, so reading one through the other's mount point cannot
     * accidentally succeed. */
    ok = ok && xfsmount_show_file("/onROOT") == KERR_OK;
    ok = ok && xfsmount_show_file("/volumes/DATA/onDATA") == KERR_OK;
    /* And the negative: DATA's file is not in the root filesystem. A test
     * without this would pass on a kernel that ignored mount points and
     * answered every path from one filesystem. */
    ok = ok && xfsmount_show_file("/onDATA") != KERR_OK;
    kprintf("volumes: %s\n", ok ? "every volume where its label says" : "FAILED");
}

static void bring_up_filesystem(void) {
    int rc;
    int32_t entries;
    automount_the_disk();
    rc = xfsmount_attach();
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
    if (xfs_quota_soft_exceeded(xfsmount_fs()) != 0) {
        /* **Reported, since it cannot be enforced.** A soft limit's grace
         * period needs a wall clock and this board has no real-time clock,
         * so the crossings are counted instead of timed — and a count
         * nobody prints is a count nobody reads. */
        kprintf("xfs: %u charge(s) went past a soft quota limit\n",
                xfs_quota_soft_exceeded(xfsmount_fs()));
    }
    /* **After** `xfsmount_attach`, because that is when the root
     * filesystem enters the table: listing before it would report every
     * volume except the one the machine is running from. */
    list_the_volumes();
    (void)xfsmount_show_file("/hello.txt");
    checksum_a_file();
}

/* Reads a whole file and prints its length and SHA-256.
 *
 * `minikern.sha=/path` — the point is the *whole* file. Listing a directory
 * proves the inode and its name; reading one block proves the first extent.
 * A file of a few megabytes is the first thing that needs the extent list
 * walked to its end, and a hash of it is a statement the operator can check
 * against the same file on another machine, which is a stronger claim than
 * "no error was reported".
 *
 * **A `+` in the path means a space.** A kernel command line is separated by
 * spaces, so a file whose name contains one cannot be written literally —
 * the option ends at the space and the path is silently truncated, which is
 * how `/2026-09-12 20-49-06.mp4` arrived here as `/2026-09-12`. The
 * substitution is the same convention a query string uses, and it is needed
 * precisely because a filename with a space in it is ordinary while a
 * command line that can carry one is not. */
static void checksum_a_file(void) {
    size_t len = 0;
    const char *path = cmdline_value(boot_cmdline, "minikern.sha", &len);
    static char name[128];
    uint8_t digest[32];
    uint64_t size = 0;
    int rc;
    if (path == NULL || len == 0 || len >= sizeof name) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        name[i] = path[i] == '+' ? ' ' : path[i];
    }
    name[len] = '\0';
    rc = xfsmount_hash_file(name, digest, &size);
    if (rc != KERR_OK) {
        kprintf("xfs: %s could not be read (rc %d)\n", name, rc);
        return;
    }
    {
        char hex[65];
        sha256_format(digest, hex);
        kprintf("xfs: %s is %llu bytes, sha256 %s\n", name, (unsigned long long)size, hex);
    }
}

static void drive_a_pin(void);
static void read_a_pin(void);
static void loopback_pins(void);
static void dump_pin_registers(void);
static void set_a_padcfg(void);

/* ── the other harts ───────────────────────────────────────────────────────── */

/* How many secondaries have reached kernel code and said so. Written by
 * each of them and read by the boot hart, so it is `volatile`: the boot hart
 * spins on it, and a compiler that cached it in a register would spin for
 * ever. It is a count rather than a flag per hart because the question the
 * boot hart asks is "have they all arrived?". */
static volatile uint32_t harts_arrived;

/* Defined in start.S: the physical address a started hart arrives at. */
void _secondary_start(void);

/* Where a secondary hart begins its kernel life.
 *
 * Called from `_secondary_start` with the hart id the firmware gave it and
 * the boot hart's `satp` to adopt. Each hart does for itself exactly what
 * is its own — its trap vector and its translation — and nothing that is
 * shared, because the boot hart has already done that and doing it twice
 * would undo it.
 *
 * It parks in `wfi` afterwards. Running user threads on it needs one lock
 * around the kernel first (P3.2): this scheduler keeps `current` in a
 * single variable, and two harts in it at once would each believe they owned
 * the same thread. Starting the harts and then not using them is the honest
 * intermediate state — the bring-up is proved, and the lock is a separate
 * piece of work that can be tested on its own.
 */
void smp_secondary_main(uint64_t hart, uint64_t satp);
static void secondary_hart_loop(void);
static void report_hart_usage(void);
static void start_swap_if_asked(void);
static void swap_selftest(void);
static void report_swap(void);

void smp_secondary_main(uint64_t hart, uint64_t satp) {
    smp_set_this_hart(hart);
    fp_enable();
    trap_init_cpu();
    /* The boot hart's page tables, which map the kernel globally — so this
     * hart can reach the kernel's code and data the moment translation is
     * on, and no bring-up table is needed. The fence is not optional: a
     * hart's TLB starts with whatever the firmware left in it. */
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero" : : "r"(satp) : "memory");
    timer_init_cpu();
    smp_secondary_arrived(hart);
    __atomic_add_fetch(&harts_arrived, 1u, __ATOMIC_SEQ_CST);
    secondary_hart_loop();
}

/* What a secondary hart does for the rest of the machine's life: run
 * threads whenever there is a process, and wait when there is not.
 *
 * It waits with interrupts **on** around `wfi`, because the thing that will
 * give it work happens on another hart and the only event that reaches this
 * one is its own timer tick. A `wfi` with interrupts off on a hart with
 * nothing pending is a hart that never wakes up. */
static void secondary_hart_loop(void) {
    for (;;) {
        struct uproc *p;
        while (!sched_wants_harts()) {
            __asm__ volatile("csrsi sstatus, 2\n\twfi\n\tcsrci sstatus, 2" : : : "memory");
        }
        /* Checked again, and for NULL: the process may have ended between
         * the wait and here. Joining a process that is gone would adopt a
         * page table that is being freed. */
        p = uspace_current();
        if (p != NULL) {
            uspace_join(p);
        }
    }
}

/* Starts the other harts and waits for them, with a bound.
 *
 * The bound matters: a hart the firmware accepted and that then never
 * arrives would hang the boot here, and "one core fewer" is a better
 * outcome than a machine that does not boot. What it waits on is the harts'
 * own report rather than the firmware's acceptance — `sbi_hart_start`
 * returning 0 means the request was taken, not that the hart ran.
 */
static void bring_up_other_harts(uint64_t dtb, uint64_t boot_hart) {
    struct fdt fdt;
    uint64_t satp;
    uint32_t want, waited;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (smp_enumerate(&fdt, boot_hart) != KERR_OK) {
        kprintf("smp: no cpus node, so this is a one-hart machine\n");
        return;
    }
    kprintf("smp: %u harts in the tree, booted on %llu\n", smp_hart_count(),
            (unsigned long long)boot_hart);
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        kprintf("smp:   hart %llu %s%s\n", (unsigned long long)h->id,
                h->id == boot_hart ? "is this one" : h->has_sv39 ? "can be started" : "has no usable MMU",
                h->has_sv39 ? "" : " (skipped)");
    }
    if (cmdline_has(boot_cmdline, "minikern.smp=off")) {
        kprintf("smp: left the other harts alone (minikern.smp=off)\n");
        return;
    }
    __asm__ volatile("csrr %0, satp" : "=r"(satp));
    /* **A physical entry point, because the firmware starts a hart with
     * translation off.** `&_secondary_start` reads as the kernel's own high
     * address now that this runs after the jump, and handing SBI that
     * address starts a hart at a number it cannot fetch from: all three
     * were started and none arrived — "0 of 3 started harts reached the
     * kernel", with nothing to say why, because the hart died before it
     * could reach a console. */
    want = smp_start_others(mmu_virt_to_phys((const void *)_secondary_start), satp);
    /* Bounded, because a hart the firmware accepted and that then never
     * arrives must cost one core rather than the whole boot. */
    for (waited = 0; harts_arrived < want && waited < 50000000u; waited++) {
    }
    kprintf("smp: %u of %u started harts reached the kernel, %u running in total\n",
            harts_arrived, want, smp_running_count());
    /* **The miss counter, said out loud.** `smp_hart_misses` existed and
     * nothing ever called it — so the one number that says whether a hart
     * failed to find its own index, and silently became hart 0, was being
     * kept and never read. It is also the check on the id-to-index table
     * that replaced the scan: a wrong table shows up here and nowhere
     * else, because every other symptom of it is a hart doing another
     * hart's work. */
    if (smp_hart_misses() != 0u) {
        kprintf("smp: **%u hart(s) could not find their own index**\n", smp_hart_misses());
    }
}

/* Everything the kernel noticed that it should not have.
 *
 * **Called after a program has run, which it was not.** This block used to
 * sit at the end of hart startup — before any program existed — so every
 * counter in it was printed as zero in every run, including the ones added
 * while hunting the four-hart race. `ROADMAP.md` §0 said those detectors
 * "all read zero in the failing runs", and they did: they were read before
 * the run. A detector reported at the wrong moment is worse than no
 * detector, because it is evidence of the wrong thing.
 *
 * `trap:` is printed unconditionally, because "one hart inside the kernel"
 * is the assumption every other argument in this kernel rests on and a
 * number nobody has ever seen is not a measurement. */
/* Defined below, beside the other reporting. */
static void report_lock_contention(void);
static void report_the_buses(uint64_t dtb);
static void report_the_network_after_everything(void);
static void report_the_thermal_range(void);

static void report_detectors(void) {
    /* `trap:` counts traps in flight rather than harts holding the lock —
     * a trap that yields is still counted until it is resumed, so more than
     * four is ordinary on a four-hart machine and the number is here for
     * scale rather than as an assertion. The allocator's collision count is
     * the real test: it is taken at the one place a lost update costs a
     * frame with two owners. */
    kprintf("trap: %u traps in flight at most, %u frames handed out twice,"
            " %u allocator collisions\n",
            trap_most_harts_inside(), pmm_double_allocations(mem_frames()),
            mem_allocator_collisions());
    {
        uint64_t taken = 0, given = 0;
        mem_heap_traffic(&taken, &given);
        kprintf("mem: the kernel heap took %llu pages and gave back %llu\n",
                (unsigned long long)taken, (unsigned long long)given);
    }
    if (uspace_unrecorded_regions() != 0) {
        kprintf("uspace: **%u mappings the address-space record could not hold**\n",
                uspace_unrecorded_regions());
    }
    if (uspace_misplaced_mappings() != 0) {
        kprintf("uspace: **%u mappings were placed on a range already taken**\n",
                uspace_misplaced_mappings());
    }
    if (uspace_fixed_over_live() != 0) {
        kprintf("uspace: **%u live pages were replaced by a fixed mapping**\n",
                uspace_fixed_over_live());
    }
    if (klock_breaches() != 0) {
        kprintf("klock: **the kernel lock was not exclusive %u times**\n", klock_breaches());
    }
    report_lock_contention();
    report_the_network_after_everything();
    if (uspace_backed_twice() != 0) {
        kprintf("uspace: **%u pages were backed twice** by two harts at once\n",
                uspace_backed_twice());
    }
    if (uspace_replaced_pages() != 0) {
        kprintf("uspace: **%u live pages replaced by an unfixed mapping**\n",
                uspace_replaced_pages());
    }
    if (uspace_dirty_frames() != 0) {
        kprintf("uspace: **%u frames were not zero after being zeroed**\n",
                uspace_dirty_frames());
    }
    if (uspace_impossible_frames() != 0) {
        kprintf("uspace: **%u frames returned to user mode with a kernel pc or sp**\n",
                uspace_impossible_frames());
    }
    if (uspace_stale_records() != 0) {
        kprintf("uspace: **%u pages mapped at an address the record called free**\n",
                uspace_stale_records());
    }
    if (trap_mismatched_entries() != 0) {
        kprintf("trap: **%llu traps entered by the wrong path** (sscratch armed in the kernel)\n",
                (unsigned long long)trap_mismatched_entries());
    }
    if (sched_bad_resumes() != 0) {
        kprintf("sched: **%u threads could not be resumed on their own stack**\n",
                sched_bad_resumes());
    }
    if (sched_spawn_failures() != 0 || sched_stack_overflows() != 0 ||
        sched_slot_aliases() != 0) {
        kprintf("sched: %u threads refused, %u stacks overflowed, %u slots aliased\n",
                sched_spawn_failures(), sched_stack_overflows(), sched_slot_aliases());
    }
    if (smp_hart_misses() != 0) {
        kprintf("smp: **%u harts failed to identify themselves** and shared hart 0's state\n",
                smp_hart_misses());
    }
}

/* The network: the device, the stack, and a lease.
 *
 * `minikern.net=off` leaves it down. Otherwise the device is found in the
 * tree, the stack is given its hardware address, and DHCP runs — and the
 * kernel waits for the lease, because almost everything a network is for
 * needs an address first and a kernel that carried on would report "no
 * address" for a lease that arrived a millisecond later.
 *
 * The frames arrive two ways on purpose: the device's interrupt, and the
 * tick. An interrupt-only driver loses whatever arrived while it was not
 * looking, which on this transport is possible because the device may
 * coalesce — the ARM project's EMAC had to be re-kicked for the same
 * reason. */
static void on_net_interrupt(void *self) {
    (void)self;
    virtio_net_poll(knet_interface());
}

/* The UART as the terminal's input port.
 *
 * A port rather than a direct call, because `kernel/tty.c` must not name a
 * UART: the hexagon rule is what let the whole input side be written and
 * tested before a byte of it ran on a board. */
static int32_t console_input_read(void *self, void *buf, uint32_t n) {
    int byte;
    (void)self;
    if (n == 0) {
        return 0;
    }
    /* **Whichever console this kernel is actually using.** With
     * `minikern.uart=sbi` the kernel never takes the UART over, so reading
     * its registers reads an uninitialised device and returns nothing for
     * ever — which presented as a terminal that blocked correctly and never
     * saw a keystroke. The driver reports -1 when it has no device, so
     * asking it first and falling back to the firmware covers both consoles
     * without the board needing to say which it is. */
    byte = jh7110_uart_getc();
    if (byte < 0) {
        byte = sbi_console_getchar();
    }
    if (byte < 0) {
        return 0; /* nothing waiting, which is not an error */
    }
    *(uint8_t *)buf = (uint8_t)byte;
    return 1;
}

static int console_input_ready(void *self) {
    (void)self;
    return 0; /* polled: readiness is whatever the next read finds */
}

/* The terminal, driven from the housekeeping beat.
 *
 * The same mechanism the polled network uses, for the same reason: there is
 * no interrupt wired from the UART yet. A 10 ms beat is well inside a
 * typist's tolerance and would be visible on a paste, which is why the poll
 * drains everything waiting rather than one byte a beat. */
static void poll_tty_from_beat(void) {
    ktty_poll();
}

/* Which device the stack is running over. The poll differs, so the tick
 * hook has to know — and a hook that polled the wrong one would leave every
 * received frame in a ring nobody reads, which looks exactly like a network
 * that is not answering. */
static bool net_over_gmac;

/* The GMAC raised its receive interrupt: acknowledge at the device and drain
 * the ring into the stack. */
static void on_gmac_interrupt(void *self) {
    (void)self;
    (void)dwmac_handle_interrupt();
}

static void poll_net_from_tick(void) {
    if (net_over_gmac) {
        (void)dwmac_poll();
        return;
    }
    virtio_net_poll(knet_interface());
}

/* **What the big kernel lock costs**, which is the question its roadmap
 * entry poses: it says splitting the lock "should wait until something is
 * actually measured to be blocked on it", and nothing had ever measured it.
 *
 * The share of acquisitions that waited says how often four harts wanted the
 * kernel at once; the average and worst waits say whether that mattered. The
 * two are reported together because either alone is misleading — a kernel
 * where every acquisition waits twenty cycles is not blocked on its lock,
 * and one where a thousandth of them wait a millisecond is. */
/* The network's counters **after the programs have run**, which is the only
 * moment they answer anything.
 *
 * They were already reported — at the end of bringing the interface up, when
 * the only traffic that had happened was the DHCP exchange. So a question
 * like "did the resolver's query get an answer" read the counts from before
 * the resolver existed, and this file has now paid for the same mistake
 * three times in one session: a detector read at the wrong moment is not a
 * weak instrument, it is a misleading one. The moment a number is taken is
 * part of the number. */
static void report_the_network_after_everything(void) {
    struct netif *nif = knet_interface();
    if (nif == NULL || nif->ip == 0) {
        return;
    }
    kprintf("net: at the end of boot: rx %u tx %u; irq %llu dispatched, %llu unclaimed\n",
            nif->stats.rx_frames, nif->stats.tx_frames, (unsigned long long)irq_count(),
            (unsigned long long)irq_spurious());
    if (net_over_gmac) {
        uint64_t sent = 0, received = 0, dropped = 0;
        dwmac_counters(&sent, &received, &dropped);
        kprintf("net: the GMAC sent %llu, received %llu, dropped %llu\n",
                (unsigned long long)sent, (unsigned long long)received,
                (unsigned long long)dropped);
    }
}

static void report_lock_contention(void) {
    uint64_t acquires = 0, waited = 0, cycles = 0, worst = 0;
    klock_contention(&acquires, &waited, &cycles, &worst);
    if (acquires == 0u) {
        return;
    }
    /* Per mille rather than per cent: on a workload that is not syscall-bound
     * the share rounds to zero as a percentage, and "0%" and "nothing
     * measured" must not read the same. */
    kprintf("klock: %llu acquisitions, %llu waited (%llu per mille)\n",
            (unsigned long long)acquires, (unsigned long long)waited,
            (unsigned long long)(waited * 1000u / acquires));
    if (waited != 0u) {
        kprintf("klock: %llu cycles waiting, %llu average, %llu worst (%llu us at 1 GHz)\n",
                (unsigned long long)cycles, (unsigned long long)(cycles / waited),
                (unsigned long long)worst, (unsigned long long)(worst / 1000u));
    }
    {
        /* **The holder's side**, which is what makes the wait actionable: a
         * hart cannot wait longer than somebody else held. */
        uint64_t total = 0, worst_held = 0;
        klock_held_times(&total, &worst_held);
        kprintf("klock: held %llu cycles total, %llu average, %llu worst (%llu us at 1 GHz)\n",
                (unsigned long long)total, (unsigned long long)(total / acquires),
                (unsigned long long)worst_held, (unsigned long long)(worst_held / 1000u));
        kprintf("klock: the longest hold was taken at %016llx, doing %llu\n",
                (unsigned long long)klock_worst_caller(),
                (unsigned long long)klock_worst_tag());
    }
}

/* What the board's tree says about the buses this kernel has no driver for.
 *
 * **Measured before a line of driver code is written.** This tree has
 * already cost three wrong guesses that each took a board run: the SD
 * controller was documented as Cadence SDHCI and is a DesignWare MSHC, the
 * GMAC's generation had to be read from a version register, and the GMAC's
 * interrupt is in `interrupts-extended` rather than `interrupts`. A
 * compatible string guessed from the SoC's documentation is a guess; the one
 * in the tree is a fact. */
static void report_one_bus(const struct fdt *fdt, const char *compat) {
    char path[96];
    for (uint32_t i = 0; i < 8u; i++) {
        phys_t base = 0;
        uint64_t size = 0;
        struct fdt_prop p;
        uint64_t irq = 0;
        if (fdt_find_compatible_nth(fdt, compat, i, path, sizeof path) != KERR_OK) {
            return;
        }
        (void)fdt_device_reg(fdt, path, 0, &base, &size);
        if (fdt_get_prop(fdt, path, "interrupts", &p) == KERR_OK) {
            (void)fdt_prop_cells(&p, 0, 1, &irq);
        } else if (fdt_get_prop(fdt, path, "interrupts-extended", &p) == KERR_OK) {
            (void)fdt_prop_cells(&p, 1, 1, &irq);
        }
        kprintf("bus: %s at %016llx (%llu bytes), irq %llu, \"%s\"\n", path,
                (unsigned long long)base, (unsigned long long)size, (unsigned long long)irq,
                compat);
    }
}

static void report_the_buses(uint64_t dtb) {
    /* Every spelling worth trying for the five the roadmap names, SoC-specific
     * first because that is what a vendor tree carries. */
    static const char *const want[] = {
        "starfive,jh7110-spi",  "snps,dw-apb-ssi",       "arm,pl022",
        "starfive,jh7110-i2c",  "snps,designware-i2c",   "starfive,jh7110-pwm",
        "opencores,pwm",        "starfive,pwm0",         "starfive,jh7110-rtc",
        "starfive,jh7110-wdt",  "starfive,wdt",          "snps,dw-wdt",
        "starfive,jh7110-temp", "starfive,jh7110-pwmdac",
    };
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    for (uint32_t i = 0; i < sizeof want / sizeof want[0]; i++) {
        report_one_bus(&fdt, want[i]);
    }
    kprintf("bus: that is every spelling asked about\n");
}

/* ── the buses: SPI, I2C, PWM and the watchdog ──────────────────────────── */

/* Brings up one of each and proves it, in the only way available on a board
 * with nothing wired to its header.
 *
 * **Each check is something that cannot pass if the driver is wrong.** A
 * driver that maps the wrong window, or whose block is unclocked, reads
 * zeroes — so every one of these reads a value that is *not* zero and not
 * constant:
 *
 *   - SPI reports the PrimeCell identifier, which must be ARM's 0x41 and
 *     part 0x022, and then sends a byte through **internal loopback** and
 *     gets it back. The GMAC was proved the same way before a cable existed.
 *   - I2C reports its DesignWare component type, which is the ASCII
 *     "DW" signature every one of these blocks carries.
 *   - PWM and the watchdog are read **twice** with their counters running:
 *     a value that has moved is a block that is clocked, and nothing else
 *     demonstrates that without an instrument on a pin.
 *
 * The watchdog's reset is never armed (`wdt_jh7110.h` says why). */
static void prove_the_spi(const struct fdt *fdt) {
    struct spi_pl022 bus;
    uint8_t back = 0;
    if (spi_pl022_init(&bus, fdt, "/soc/spi@10060000") != KERR_OK) {
        kprintf("spi: /soc/spi@10060000 did not bind\n");
        return;
    }
    {
        uint32_t id = spi_pl022_part(&bus);
        /* The low twelve bits are the part number and the next eight the
         * designer. Printed whole so that a wrong answer is diagnosable
         * rather than just a failure. */
        kprintf("spi: PrimeCell id %08x (part %03x, designer %02x)\n", id, id & 0xfffu,
                (id >> 12) & 0xffu);
        if ((id & 0xfffu) != 0x022u) {
            kprintf("spi: **not a PL022 at that address**\n");
            return;
        }
    }
    if (spi_pl022_configure(&bus, 1000000u, true) != KERR_OK) {
        return;
    }
    if (spi_pl022_transfer(&bus, 0xa5u, &back) == KERR_OK) {
        kprintf("spi: loopback sent a5, received %02x%s\n", back,
                back == 0xa5u ? " — the data path works" : " — **wrong byte**");
    } else {
        kprintf("spi: **the transfer timed out**\n");
    }
}

static void prove_the_i2c(const struct fdt *fdt) {
    struct i2c_dw bus;
    if (i2c_dw_init(&bus, fdt, "/soc/i2c@10030000") != KERR_OK) {
        kprintf("i2c: /soc/i2c@10030000 did not bind\n");
        return;
    }
    /* `0x44570140` is "DW" and a version: the signature every DesignWare I2C
     * carries, and the thing that says this is the right register map. */
    kprintf("i2c: component type %08x, version %08x\n", i2c_dw_component_type(&bus),
            i2c_dw_component_version(&bus));
}

static void prove_a_counter_moves(const char *what, uint32_t first, uint32_t second) {
    kprintf("%s: counter %u then %u%s\n", what, first, second,
            first != second ? " — it is clocked and running" : " — **it did not move**");
}

static void prove_the_pwm(const struct fdt *fdt) {
    struct pwm_jh7110 pwm;
    uint32_t first, second;
    if (pwm_jh7110_init(&pwm, fdt, "/soc/pwm@120d0000") != KERR_OK) {
        kprintf("pwm: /soc/pwm@120d0000 did not bind\n");
        return;
    }
    /* A long period so that two reads fall inside one cycle rather than
     * aliasing to the same value by landing a whole period apart. */
    if (pwm_jh7110_start(&pwm, 0, 0x00ffffffu, 0x00800000u) != KERR_OK) {
        return;
    }
    first = pwm_jh7110_counter(&pwm, 0);
    for (volatile uint32_t i = 0; i < 20000u; i++) {
    }
    second = pwm_jh7110_counter(&pwm, 0);
    prove_a_counter_moves("pwm", first, second);
    (void)pwm_jh7110_stop(&pwm, 0);
}

static void prove_the_watchdog(const struct fdt *fdt) {
    struct wdt_jh7110 wdt;
    uint32_t first, second;
    if (wdt_jh7110_init(&wdt, fdt, "/soc/watchdog@13070000") != KERR_OK) {
        kprintf("wdt: /soc/watchdog@13070000 did not bind\n");
        return;
    }
    if (wdt_jh7110_start_counting(&wdt, 0x40000000u) != KERR_OK) {
        return;
    }
    first = wdt_jh7110_counter(&wdt);
    for (volatile uint32_t i = 0; i < 20000u; i++) {
    }
    second = wdt_jh7110_counter(&wdt);
    prove_a_counter_moves("wdt", first, second);
    /* **The window and the search, only when the counter did not move.**
     * They are how this driver's offsets were found — the published ones are
     * wrong for this board — and leaving them on every boot would be noise;
     * leaving them out would mean the next person rediscovers the method.
     * So they run exactly when they are needed. */
    if (second == first) {
        wdt_jh7110_dump(&wdt, 16u);
        if (wdt_jh7110_find_enable(&wdt) == 0) {
            kprintf("wdt: **no candidate register started the counter**\n");
        }
    }
    /* **Stopped again.** It cannot reset the board — the bit is never set —
     * but leaving a counter running that nothing feeds is still a mechanism
     * with no owner, which this file's rules are against. */
    (void)wdt_jh7110_stop(&wdt);
}

/* USB: the VL805 the PCIe work has been enumerating and ignoring.
 *
 * Reported in stages because each is provable on its own — the controller's
 * own description of itself first, then whether it reached its running
 * state, then what is attached. A version and a port count that are
 * plausible say the register window is right; a controller that runs says
 * the rings and the DMA are right; a port that enables says it can see the
 * operator's flash drive. */
static void bring_up_usb(void) {
    uint32_t version = 0, ports = 0, slots = 0;
    int rc;
    xhci_trace_ports(cmdline_has(boot_cmdline, "minikern.usbtrace"));
    rc = xhci_init();
    if (rc == KERR_NOENT) {
        kprintf("usb: no XHCI controller on the bus\n");
        return;
    }
    xhci_describe(&version, &ports, &slots);
    kprintf("usb: xhci %x.%x, %u ports, %u device slots\n", (version >> 8) & 0xffu,
            version & 0xffu, ports, slots);
    if (rc != KERR_OK || !xhci_running()) {
        kprintf("usb: **the controller did not reach its running state** (rc %d)\n", rc);
        return;
    }
    kprintf("usb: the controller is running\n");
    {
        uint32_t attached = xhci_scan_ports();
        kprintf("usb: %u port(s) with something attached, contexts %u bytes\n", attached,
                xhci_context_bytes());
    }
    {
        struct xhci_device dev = {0};
        int rc = xhci_enumerate_first_port(&dev);
        if (rc == KERR_OK) {
            /* **The vendor and product the device reports**, which is the
             * assertion: those bytes came out of the device over a control
             * transfer, so the slot, both contexts, the Address Device
             * command, the transfer ring and the event ring all worked. A
             * plausible-looking driver that had any of them wrong would time
             * out or return zeroes here. */
            kprintf("usb: port %u slot %u speed %u: %04x:%04x, class %02x, ep0 packet %u\n",
                    dev.port, dev.slot, dev.speed, dev.vendor, dev.product, dev.device_class,
                    dev.max_packet0);
            kprintf("usb: a device descriptor was read over a control transfer\n");
        } else {
            kprintf("usb: no device could be enumerated (rc %d)\n", rc);
        }
    }
}

/* ── the SoC's temperature, through /proc ──────────────────────────────── */

/* The sensor, kept for as long as the kernel runs: `/proc/thermal` is read
 * whenever somebody asks, so the block stays clocked and converting rather
 * than being brought up per read. */
static struct temp_jh7110 soc_temp;

/* Writes a signed millidegree value as degrees with three decimals.
 *
 * By hand because the formatter has no `%f` and should not grow one for this
 * — and because a temperature rendered by integer division has to handle a
 * negative value without the sign being lost from the fraction, which is the
 * one thing that goes wrong here. `-500` is `-0.500`, not `0.-500`. */
static uint32_t write_degrees(char *out, uint32_t room, int32_t mdeg) {
    uint32_t at = 0;
    int32_t whole = mdeg / 1000;
    int32_t frac = mdeg % 1000;
    if (frac < 0) {
        frac = -frac;
    }
    if (mdeg < 0 && whole == 0) {
        /* The sign lives on the whole part, and a whole part of zero has
         * nowhere to put it. */
        at += (uint32_t)kfmt_snprintf(out + at, room - at, "-");
    }
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "%d.%03d", whole, frac);
    return at;
}

/* `/proc/thermal`: the die temperature, in the two forms anybody wants.
 *
 * The millidegree line is first and is named the way Linux's thermal
 * interface names it, so a script that already reads
 * `/sys/class/thermal/thermal_zone0/temp` needs one path changed and no
 * parsing changed. The human line is for a person reading it over the
 * network, which on this board is the usual case.
 *
 * The **raw** conversion is reported too. It is what separates a wrong
 * answer from a wrong sensor: the arithmetic is one multiply and a subtract,
 * so if the degrees look implausible the raw value says immediately whether
 * the block is returning nonsense or the formula is. */
/* **What the cooling actually did, once the board has been busy.**
 *
 * One reading at bring-up says the sensor works and nothing about whether
 * the heatsink is coping: the die is coldest at the moment the kernel first
 * looks at it. The range over the whole boot is the number that answers the
 * question, and it is free because the housekeeping beat has been sampling
 * all along.
 *
 * The reading count is printed with it because a range drawn from a handful
 * of samples is not a range — if the beat was not running, this says so by
 * reporting a small number rather than by looking plausible. */
static void report_the_thermal_range(void) {
    int32_t lowest;
    int32_t highest;
    uint64_t readings;
    temp_jh7110_extremes(&soc_temp, &lowest, &highest, &readings);
    if (readings == 0) {
        return; /* no sensor, or it never gave a plausible reading */
    }
    {
        char low[16];
        char high[16];
        low[write_degrees(low, sizeof low - 1, lowest)] = '\0';
        high[write_degrees(high, sizeof high - 1, highest)] = '\0';
        kprintf("temp: %s to %s over %llu readings\n", low, high,
                (unsigned long long)readings);
    }
}

static uint32_t thermal_file(char *out, uint32_t room) {
    int32_t mdeg = temp_jh7110_millidegrees(&soc_temp);
    uint32_t at = 0;
    if (mdeg == TEMP_UNREADABLE) {
        return (uint32_t)kfmt_snprintf(out, room,
                                   "temp_millidegrees: unreadable\nraw: %u\nsensor: %s\n",
                                   temp_jh7110_raw(&soc_temp),
                                   soc_temp.up ? "out of range" : "not bound");
    }
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "temp_millidegrees: %d\n", mdeg);
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "temp_celsius: ");
    at += write_degrees(out + at, room - at, mdeg);
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "\n");
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "raw: %u\n", temp_jh7110_raw(&soc_temp));
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "sensor: starfive,jh7110-temp\n");
    {
        int32_t lowest;
        int32_t highest;
        uint64_t readings;
        temp_jh7110_extremes(&soc_temp, &lowest, &highest, &readings);
        if (readings != 0) {
            at += (uint32_t)kfmt_snprintf(out + at, room - at, "lowest_millidegrees: %d\n",
                                          lowest);
            at += (uint32_t)kfmt_snprintf(out + at, room - at, "highest_millidegrees: %d\n",
                                          highest);
            at += (uint32_t)kfmt_snprintf(out + at, room - at, "highest_celsius: ");
            at += write_degrees(out + at, room - at, highest);
            at += (uint32_t)kfmt_snprintf(out + at, room - at, "\n");
            at += (uint32_t)kfmt_snprintf(out + at, room - at, "readings: %llu\n",
                                          (unsigned long long)readings);
        }
    }
    return at;
}

/* `/proc/meminfo`, in the two fields anything reading it actually wants.
 *
 * **The units are the file's, not the kernel's.** `MemTotal: N kB` is the
 * format every reader of this file has been written against, and its `kB` is
 * 1024 bytes however it is spelled; reporting pages here, or bytes with a
 * `kB` suffix, is the kind of mistake that reads as a working file with a
 * thousand-fold wrong number in it.
 *
 * `MemAvailable` is the free-page count rather than an estimate of what
 * could be reclaimed: this kernel reclaims nothing, so the two are the same
 * number and the honest one is the one it can measure. */
static uint32_t meminfo_file(char *out, uint32_t room) {
    uint64_t total_kb = (uint64_t)mem_total_pages() * (PAGE_SIZE / 1024ull);
    uint64_t free_kb = (uint64_t)mem_free_pages_count() * (PAGE_SIZE / 1024ull);
    uint32_t at = 0;
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "MemTotal:       %llu kB\n",
                                  (unsigned long long)total_kb);
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "MemFree:        %llu kB\n",
                                  (unsigned long long)free_kb);
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "MemAvailable:   %llu kB\n",
                                  (unsigned long long)free_kb);
    return at;
}

/* `/proc/stat`'s `cpu` line, from the one quantity this kernel measures.
 *
 * **Four columns, and two of them are honestly zero.** The format is
 * `cpu user nice system idle`, and this kernel has no user/kernel time
 * split: every nanosecond a hart is not idle is spent on behalf of a
 * program, in a kernel that programs enter by trapping. So the busy time
 * goes in `system`, where it is true, rather than being divided between
 * `user` and `system` by a guess — a reader computing utilisation adds the
 * three busy columns and gets the right answer either way, and a reader
 * showing them separately is told something measured rather than invented.
 *
 * The unit is the file's: USER_HZ ticks of 10 ms, which is what every reader
 * of this file assumes regardless of the kernel's own tick.
 *
 * **What is deliberately not here**: `ctxt`, `processes`, the per-hart `cpuN`
 * lines, and the rest of the file. The thermal reading is what this is for
 * at the moment, and a `cpu` line computed from a quantity the scheduler
 * actually measures is worth more than a fuller file with guessed columns in
 * it. The roadmap carries the remainder. */
static uint32_t stat_file(char *out, uint32_t room) {
    uint64_t harts = smp_hart_count() == 0 ? 1u : smp_hart_count();
    uint64_t idle_ticks = sched_idle_ns() / 10000000ull;
    uint64_t wall_ticks = (timer_ns() / 10000000ull) * harts;
    uint64_t busy_ticks = wall_ticks > idle_ticks ? wall_ticks - idle_ticks : 0;
    uint32_t at = 0;
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "cpu  0 0 %llu %llu\n",
                                  (unsigned long long)busy_ticks,
                                  (unsigned long long)idle_ticks);
    at += (uint32_t)kfmt_snprintf(out + at, room - at, "harts %llu\n",
                                  (unsigned long long)harts);
    return at;
}

/* Keeps the extremes current when nobody is reading.
 *
 * **The peak is the number the operator wants**, and it happens while the
 * board is busy rather than while something is asking about it. A reader
 * that samples every few seconds sees neither the spike nor the recovery,
 * so the kernel takes the readings and the reader collects the extremes. */
static void sample_thermal_from_beat(void) {
    temp_jh7110_sample(&soc_temp);
}

static void bring_up_thermal(uint64_t dtb) {
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (temp_jh7110_init(&soc_temp, &fdt, "/soc/temperature-sensor@120e0000") != KERR_OK) {
        kprintf("temp: /soc/temperature-sensor@120e0000 did not bind\n");
        return;
    }
    /* **Registered before anything can open it**, which is this file's own
     * rule: a port installed late is a port that was not installed, and that
     * has cost this tree two bugs. */
    (void)procfs_add("/proc/thermal", thermal_file);
    (void)procfs_add("/proc/meminfo", meminfo_file);
    (void)procfs_add("/proc/stat", stat_file);
    /* **Sampling starts with the sensor**, not with the terminal it was
     * first attached to: the range is only the boot's range if it begins
     * where the readings begin. */
    (void)timer_add_housekeeping_hook(sample_thermal_from_beat);
    {
        int32_t mdeg = temp_jh7110_millidegrees(&soc_temp);
        if (mdeg == TEMP_UNREADABLE) {
            kprintf("temp: the sensor is bound but its reading is out of range (raw %u)\n",
                    temp_jh7110_raw(&soc_temp));
            temp_jh7110_dump(&soc_temp);
            return;
        }
        /* **A plausibility verdict, not just a number**, so that a board
         * case can assert something a broken sensor would fail. The range
         * is deliberately wide — the operator has a radiator and a fan, but
         * a hot day in a case is a different number — and what it excludes
         * is the failure modes this driver actually had: a raw zero reading
         * as -81 C, and a wrong register offset reading as something
         * absurd. A test that asserted a temperature would fail on a cold
         * morning; one that asserts "plausible" fails only when it should. */
        bool plausible = mdeg > 10000 && mdeg < 90000;
        kprintf("temp: the die is at %d millidegrees (raw %u), which is %s; /proc/thermal "
                "reports it\n",
                mdeg, temp_jh7110_raw(&soc_temp),
                plausible ? "a plausible die temperature" : "**not a plausible temperature**");
    }
}

static void bring_up_the_buses(uint64_t dtb) {
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    prove_the_spi(&fdt);
    prove_the_i2c(&fdt);
    prove_the_pwm(&fdt);
    prove_the_watchdog(&fdt);
    /* **And the one that is not there.** The roadmap asks for an RTC; this
     * board's tree names none, and `minikern.buses` is the evidence — every
     * spelling tried, nothing found. The VisionFive 2 has no battery-backed
     * clock fitted, so there is no device to drive rather than a driver
     * missing. Said out loud, because "no RTC driver" and "no RTC" are
     * different facts and only one of them is work. */
    kprintf("rtc: the tree names no real-time clock on this board\n");
}

static void bring_up_network(uint64_t dtb) {
    struct fdt fdt;
    const struct netdev_port *port = NULL;
    uint8_t mac[ETH_ALEN];
    uint32_t xid = 0;
    int rc;
    if (cmdline_has(boot_cmdline, "minikern.net=off")) {
        kprintf("net: left down (minikern.net=off)\n");
        return;
    }
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    /* **virtio first, then the board's own Ethernet.** virtio is what the
     * emulator offers and the tests run against; the GMAC is what the
     * board has. Trying virtio first keeps every existing case behaving
     * identically, and a board with no virtio falls through to real
     * hardware instead of reporting that it has no network — which is what
     * it did, with the driver sitting right there working. */
    rc = virtio_net_init(&fdt, &port, mac);
    if (rc != KERR_OK) {
        /* Before probing: the filter is written while the address is taken. */
        dwmac_use_strict_filter(cmdline_has(boot_cmdline, "minikern.netfilter"));
        rc = dwmac_init(&fdt, &port);
        if (rc != KERR_OK) {
            kprintf("net: no network device in the tree (rc %d)\n", rc);
            return;
        }
        dwmac_address(mac);
        net_over_gmac = true;
    }
    {
        /* **The sink is what the driver hands received frames to**, and it
         * comes back from `knet_init` — so it has to be captured, not
         * discarded as the virtio path could afford to. A GMAC with no sink
         * receives into its ring and delivers nothing. */
        static struct frame_sink sink;
        sink = knet_init(mac, port);
        if (net_over_gmac) {
            dwmac_set_sink(&sink);
        }
    }
    if (!net_over_gmac) {
        static const struct irq_handler handler = {.handle = on_net_interrupt, .self = NULL};
        (void)irq_register(virtio_net_irq(), &handler);
    } else {
        /* **The GMAC's own interrupt, which this device cannot do without.**
         * Polling from the housekeeping beat is enough for DHCP and not
         * enough for a blocking read: a thread in `recvfrom` has no
         * deadline, so under dynamic ticks nothing is scheduled, nothing
         * polls, and the reply sits in the ring while the only thread that
         * wants it waits for ever. Found exactly that way — the deadlock
         * detector fired on a user-space resolver that had already sent its
         * query. */
        static const struct irq_handler gmac = {.handle = on_gmac_interrupt, .self = NULL};
        uint32_t source = dwmac_irq();
        dwmac_report_filter();
        if (cmdline_has(boot_cmdline, "minikern.nettrace")) {
            dwmac_trace_destinations(true);
        }
        if (source != 0 && irq_register(source, &gmac) == KERR_OK) {
            dwmac_enable_rx_interrupt();
            kprintf("net: receive interrupt %u armed\n", source);
        } else {
            kprintf("net: **no interrupt for the GMAC (source %u); reads will only "
                    "complete while something else keeps the tick alive**\n",
                    source);
        }
    }
    if (net_over_gmac) {
        uint32_t mbit = 0;
        bool full = false;
        /* **Said before DHCP is attempted**, because a link that is down
         * makes every answer below a timeout, and "no lease" with no
         * mention of the cable is the least useful way to report that. */
        if (dwmac_link_up(&mbit, &full)) {
            kprintf("net: over the board's Ethernet, link up at %u Mbit %s duplex\n", mbit,
                    full ? "full" : "half");
        } else {
            kprintf("net: over the board's Ethernet, **link down** — DHCP will time out\n");
        }
    }
    (void)timer_add_housekeeping_hook(poll_net_from_tick);
    random_fill(&xid, sizeof xid);
    knet_start(xid);
    if (knet_wait_address(5000u) != KERR_OK) {
        kprintf("net: no address after 5 s — carrying on without one\n");
        return;
    }
    knet_report();
}

/* The board's pins.
 *
 * Reports what it found and nothing more: driving a pin is something only a
 * board can be asked to do, and a kernel that pokes at pins on a machine
 * whose schematic it has not seen is a kernel that shorts something. Under
 * an emulator there is no pin controller at all, which is said rather than
 * passed over — "no GPIO" and "GPIO I failed to find" must not look the
 * same. */
/* The clock generators, **before anything that needs a clock**.
 *
 * This used to be the first half of bringing up GPIO, which put it after
 * storage — so the SD controller asked to be switched on before the thing
 * that switches peripherals on existed, and reported "could not be clocked"
 * on a board where its clocks were perfectly available. A step that
 * everything else depends on belongs on its own, early. */
static void bring_up_clocks(uint64_t dtb) {
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (crg_jh7110_init(&fdt) == 0) {
        kprintf("crg: no JH7110 clock generator in the device tree\n");
        return;
    }
    kprintf("crg: sys at %016llx, aon at %016llx\n", (unsigned long long)crg_base(CRG_SYS),
            (unsigned long long)crg_base(CRG_AON));
}

/* Roadmap §4: what the board's own Ethernet actually is, before a line of
 * driver is written for it. `minikern.gmac` asks.
 *
 * Here beside the other hardware bring-up because it needs the device tree
 * and clocks, and deliberately *before* a driver exists: the GMAC is a
 * larger block than the SD controller this tree already wrote twice, once
 * for the wrong register map. */
/* Roadmap §4: the board's own Ethernet brought up, as far as the link.
 *
 * `minikern.gmacup` asks. Stopping at the link is deliberate: it is the
 * point at which the clocks, the register mapping, MDIO and the PHY are all
 * known to work together, and every one of those was a separate wrong guess
 * before the probe measured it. Frames need descriptor rings, which is the
 * next step and has nothing left to discover. */
/* Where received frames go for the bring-up test: it prints the first few
 * and counts the rest. The frame is valid only during the call, which is
 * the port's contract, so anything kept would have to be copied — and
 * nothing here needs to be. */
static void show_frame(void *self, const uint8_t *frame, uint32_t len) {
    static uint32_t said;
    (void)self;
    if (said < 4u) {
        said++;
        kprintf("dwmac: frame of %u bytes, %02x:%02x:%02x:%02x:%02x:%02x -> "
                "%02x:%02x:%02x:%02x:%02x:%02x type %02x%02x\n",
                len, frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], frame[0],
                frame[1], frame[2], frame[3], frame[4], frame[5], frame[12], frame[13]);
    }
}

static const struct frame_sink show_sink = {.frame = show_frame, .self = NULL};

static void bring_up_the_ethernet(uint64_t dtb) {
    struct fdt fdt;
    const struct netdev_port *port = NULL;
    uint32_t mbit = 0;
    bool full = false;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        kprintf("dwmac: no device tree\n");
        return;
    }
    dwmac_set_sink(&show_sink);
    if (dwmac_init(&fdt, &port) != KERR_OK) {
        kprintf("dwmac: not brought up\n");
        return;
    }
    if (!dwmac_link_up(&mbit, &full)) {
        return;
    }
    kprintf("dwmac: ready at %u Mbit %s duplex\n", mbit, full ? "full" : "half");
    /* **A frame the switch is obliged to answer.** An ARP request for this
     * board's own address, broadcast: anything on the segment that has that
     * address replies, and nothing does, so what comes back is whatever
     * else is talking — which on any real network is plenty. The point is
     * not the reply, it is that the receive path delivers *something*: a
     * link that is up and a ring that never fills means the rings are
     * wrong, and that is indistinguishable from a quiet network unless
     * something is sent to stir it. */
    if (port != NULL) {
        uint8_t arp[42];
        uint8_t mac[6];
        uint64_t sent = 0, got = 0, dropped = 0;
        dwmac_address(mac);
        for (uint32_t i = 0; i < sizeof arp; i++) {
            arp[i] = 0;
        }
        for (uint32_t i = 0; i < 6u; i++) {
            arp[i] = 0xffu;      /* to everybody */
            arp[6u + i] = mac[i]; /* from this board */
            arp[22u + i] = mac[i];
        }
        arp[12] = 0x08;
        arp[13] = 0x06; /* ARP */
        arp[14] = 0x00;
        arp[15] = 0x01; /* Ethernet */
        arp[16] = 0x08;
        arp[17] = 0x00; /* IPv4 */
        arp[18] = 6u;
        arp[19] = 4u;
        arp[21] = 0x01; /* request */
        /* **In loopback**, so the answer does not depend on who else is on
         * the wire. The frame the board sends is the frame it must receive,
         * and `show_frame` prints what came back for comparison. */
        dwmac_set_loopback(true);
        if (port->send(port->self, arp, sizeof arp) != KERR_OK) {
            kprintf("dwmac: the first frame was refused\n");
        }
        /* Polled for a while, because nothing interrupts yet. */
        for (uint32_t i = 0; i < 2000000u; i++) {
            (void)dwmac_poll();
        }
        dwmac_set_loopback(false);
        dwmac_counters(&sent, &got, &dropped);
        kprintf("dwmac: %llu sent, %llu received, %llu dropped; dma status %08x, "
                "last tx desc %08x\n",
                (unsigned long long)sent, (unsigned long long)got,
                (unsigned long long)dropped, dwmac_dma_status(), dwmac_last_tx_status());
    }
}

static void probe_the_ethernet(uint64_t dtb) {
    struct fdt fdt;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        kprintf("gmac: no device tree\n");
        return;
    }
    (void)dwmac_probe(&fdt);
}

static void bring_up_gpio(uint64_t dtb) {
    struct fdt fdt;
    uint32_t found;
    if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) != KERR_OK) {
        return;
    }
    if (!crg_present(CRG_SYS)) {
        kprintf("gpio: no clock generator, so no pin controller\n");
        return;
    }
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
    read_a_pin();
    dump_pin_registers();
    set_a_padcfg();
    loopback_pins();
    drive_a_pin();
}

/* Drives a pin and reads it back.
 *
 * `minikern.gpio=<controller>:<pin>` sets the pin high, reads the
 * controller's input register, sets it low, and reads again — which on real
 * hardware is a loopback through the pad and the only way to show that a
 * write reached the pin rather than merely reaching a register.
 *
 * It is **opt-in and names the pin**, because driving one on a board whose
 * schematic the kernel has not seen is how a kernel shorts something: a pin
 * that is an input with a pull-up, or wired to a power-enable, does not
 * forgive being driven low. So the operator says which pin, having looked.
 *
 * This cannot run under an emulator — nothing models this controller — so a
 * machine without one says so and does nothing, which is the honest
 * outcome rather than a test that appears to pass. */
/* One `<controller>:<pin>` at `*at`, advancing past it and any comma. False
 * when there is no number there, which ends a list. */
static bool parse_pin(const char *spec, size_t len, uint32_t *at, uint32_t *controller,
                      uint32_t *pin) {
    uint32_t digits = 0;
    *controller = 0;
    *pin = 0;
    while (*at < len && spec[*at] >= '0' && spec[*at] <= '9') {
        *controller = *controller * 10u + (uint32_t)(spec[(*at)++] - '0');
        digits++;
    }
    *at += *at < len && spec[*at] == ':' ? 1u : 0u;
    while (*at < len && spec[*at] >= '0' && spec[*at] <= '9') {
        *pin = *pin * 10u + (uint32_t)(spec[(*at)++] - '0');
        digits++;
    }
    *at += *at < len && spec[*at] == ',' ? 1u : 0u;
    return digits > 0;
}

/* A single `<controller>:<pin>` from a named option. False when the option is
 * absent, so a caller can tell "not asked for" from "asked for pin 0". */
static bool pin_from_cmdline(const char *option, uint32_t *controller, uint32_t *pin) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, option, &len);
    uint32_t at = 0;
    if (spec == NULL) {
        return false;
    }
    return parse_pin(spec, len, &at, controller, pin);
}

/* Reads a pin without driving it.
 *
 * `minikern.gpio.read=<controller>:<pin>` — separate from driving one
 * because reading is safe on any pin and driving is not, and because there
 * is a pin on this board whose state is **known independently**: the SD
 * slot's card-detect line. The tree says `cd-gpios = <… 41 1>`, active low,
 * so with a card in the slot it must read low and with the slot empty it
 * must read high. That makes it the one GPIO check on real silicon whose
 * answer is not supplied by the thing being tested. */
static void report_one_pin(uint32_t controller, uint32_t pin) {
    bool high = false;
    int rc = gpio_jh7110_set_input(controller, pin);
    if (rc != KERR_OK) {
        kprintf("gpio: could not make controller %u pin %u an input (rc %d)\n", controller, pin,
                rc);
        return;
    }
    rc = gpio_jh7110_read(controller, pin, &high);
    if (rc != KERR_OK) {
        kprintf("gpio: could not read controller %u pin %u (rc %d)\n", controller, pin, rc);
        return;
    }
    kprintf("gpio: controller %u pin %u reads %s\n", controller, pin, high ? "high" : "low");
}

static void read_a_pin(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.gpio.read", &len);
    uint32_t at = 0, controller = 0, pin = 0;
    if (spec == NULL) {
        return;
    }
    /* A list, because **one reading proves very little**: a register path
     * that returns the same value for every pin would pass a single check on
     * the card-detect line and mean nothing. Several pins that do not all
     * agree is the evidence that the reading is the pin's and not the
     * driver's. */
    while (parse_pin(spec, len, &at, &controller, &pin)) {
        report_one_pin(controller, pin);
    }
}

/* Drives one pin and reads another, with a wire between them.
 *
 * `minikern.gpio.loop=<controller>:<out>,<controller>:<in>` — the operator's
 * own suggestion, and a **strictly better test than a pad loopback**: a pad
 * loopback cannot distinguish a signal that reached the pin from a
 * controller that merely remembers what it was told, because both the write
 * and the read go through the same pin's logic. A jumper makes the two pins
 * independent, so a reading on the far pin is evidence that the level
 * physically left the near one and travelled down a wire.
 *
 * It also tests both directions at once — one pin configured as an output
 * and another as an input — and it tests them *against each other*, which
 * nothing inside the kernel can fake.
 *
 * Both levels are tried, because a pin stuck high reads correctly for half
 * the test, and that half is the one a hasty check runs. */
static void one_loopback(uint32_t out_ctrl, uint32_t out_pin, uint32_t in_ctrl, uint32_t in_pin) {
    uint32_t matched = 0;
    if (gpio_jh7110_set_input(in_ctrl, in_pin) != KERR_OK) {
        kprintf("gpio: could not make %u:%u an input\n", in_ctrl, in_pin);
        return;
    }
    for (uint32_t step = 0; step < 2u; step++) {
        bool want = step == 0, got = false;
        if (gpio_jh7110_set_output(out_ctrl, out_pin, want) != KERR_OK ||
            gpio_jh7110_read(in_ctrl, in_pin, &got) != KERR_OK) {
            kprintf("gpio: %u:%u -> %u:%u could not drive or read\n", out_ctrl, out_pin, in_ctrl,
                    in_pin);
            return;
        }
        matched += got == want ? 1u : 0u;
    }
    kprintf("gpio: %u:%u -> %u:%u  %s (%u of 2 levels)\n", out_ctrl, out_pin, in_ctrl, in_pin,
            matched == 2u ? "WIRE OK" : "MISMATCH", matched);
}

static void loopback_pins(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.gpio.loop", &len);
    uint32_t at = 0, out_ctrl = 0, out_pin = 0, in_ctrl = 0, in_pin = 0;
    if (spec == NULL) {
        return;
    }
    /* Pairs of pairs: each wire is tested in **both** directions, which also
     * proves a pin can be turned round from an output into an input — a
     * one-way test leaves half the driver unexercised and would pass on a
     * pin that is stuck in whichever mode it was first put into. */
    while (parse_pin(spec, len, &at, &out_ctrl, &out_pin) &&
           parse_pin(spec, len, &at, &in_ctrl, &in_pin)) {
        one_loopback(out_ctrl, out_pin, in_ctrl, in_pin);
    }
}

/* Drives each named pin and reads it back on itself.
 *
 * `minikern.gpio=<controller>:<pin>[,...]` — a list, so one boot can compare
 * several pins side by side. That comparison is the point: a pin that drives
 * and a pin that does not, in the same boot, localises a fault to the pad
 * rather than to the driver.
 *
 * It is **opt-in and names the pins**, because driving one on a board whose
 * schematic the kernel has not seen is how a kernel shorts something: a pin
 * that is an input with a pull-up, or wired to a power-enable, does not
 * forgive being driven low. So the operator says which pin, having looked.
 *
 * A self-readback is weaker evidence than a wire (`minikern.gpio.loop`): it
 * cannot separate "the level reached the pad" from "the controller remembers
 * what it was told". It is here for when there is no wire. */
/* Dumps a window of a pin controller's registers.
 *
 * `minikern.gpio.dump=<controller>:<word index>` prints 16 words from there.
 * It is **read-only**, and it exists because this driver drives a pin's
 * output registers without selecting the pad's function: on the board one
 * header pin could be driven and its neighbour could not. Which register
 * holds that selection is a thing to *measure* — by comparing the words
 * belonging to a pin that works against one that does not — rather than to
 * guess from a datasheet this kernel cannot check. A guess written to a pin
 * controller can drive a pad wired to something that does not forgive it. */
/* Sets one named pin's pad configuration.
 *
 * `minikern.gpio.padcfg=<controller>:<pin>:<value>` — an experiment, not a
 * feature. It exists because measuring showed a single pad-config bit
 * separating the one header pin that could be driven from three that could
 * not, and the only way to confirm which bit that is, is to set it and look.
 * Scoped to a named pin so the write cannot land on an unrelated pad. */
static void set_a_padcfg(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.gpio.padcfg", &len);
    uint32_t at = 0, controller = 0, pin = 0, value = 0;
    if (spec == NULL || !parse_pin(spec, len, &at, &controller, &pin)) {
        return;
    }
    at += at < len && spec[at] == ':' ? 1u : 0u; /* the separator before the value */
    while (at < len && spec[at] >= '0' && spec[at] <= '9') {
        value = value * 10u + (uint32_t)(spec[at++] - '0');
    }
    kprintf("gpio: setting %u:%u padcfg to %02x\n", controller, pin, value);
    (void)gpio_jh7110_set_padcfg(controller, pin, value);
}

static void dump_pin_registers(void) {
    uint32_t controller = 0, first = 0;
    if (!pin_from_cmdline("minikern.gpio.dump", &controller, &first)) {
        return;
    }
    for (uint32_t i = 0; i < 16u; i += 4u) {
        uint32_t w[4] = {0, 0, 0, 0};
        for (uint32_t j = 0; j < 4u; j++) {
            (void)gpio_jh7110_peek(controller, (first + i + j) * 4u, &w[j]);
        }
        kprintf("gpio: +%04x  %08x %08x %08x %08x\n", (first + i) * 4u, w[0], w[1], w[2], w[3]);
    }
}

static void drive_one_pin(uint32_t controller, uint32_t pin) {
    uint32_t matched = 0;
    uint32_t pad = 0;
    if (gpio_jh7110_get_padcfg(controller, pin, &pad) == KERR_OK) {
        kprintf("gpio: %u:%u padcfg %02x\n", controller, pin, pad);
    }
    for (uint32_t step = 0; step < 2u; step++) {
        bool want = step == 0, got = false;
        int rc = gpio_jh7110_set_output(controller, pin, want);
        if (rc != KERR_OK) {
            kprintf("gpio: could not drive %u:%u (rc %d)\n", controller, pin, rc);
            return;
        }
        if (gpio_jh7110_read(controller, pin, &got) != KERR_OK) {
            kprintf("gpio: could not read %u:%u back\n", controller, pin);
            return;
        }
        kprintf("gpio: %u:%u drove %s, reads %s\n", controller, pin, want ? "high" : "low",
                got ? "high" : "low");
        matched += got == want ? 1u : 0u;
    }
    kprintf("gpio: %u:%u self-readback %s (%u of 2)\n", controller, pin,
            matched == 2u ? "OK" : "FAILED", matched);
    (void)gpio_jh7110_set_input(controller, pin);
}

static void drive_a_pin(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.gpio", &len);
    uint32_t at = 0, controller = 0, pin = 0;
    if (spec == NULL) {
        return;
    }
    while (parse_pin(spec, len, &at, &controller, &pin)) {
        drive_one_pin(controller, pin);
    }
}

/* Runs the user-mode programs, if asked.
 *
 * `minikern.user=1` is the proof that user mode works: a program of its own
 * in an address space of its own, making syscalls, and a second one that
 * faults on purpose so that a **user** fault can be seen to end the process
 * rather than the kernel. */
extern const char user_program_start[], user_program_end[];
extern const char user_faulter_start[], user_faulter_end[];
extern const char user_execone_start[], user_execone_end[];
extern const char user_spin_start[], user_spin_end[];
extern const char user_nice_start[], user_nice_end[];
extern const char user_pin_start[], user_pin_end[];
extern const char user_rt_start[], user_rt_end[];
extern const char user_hostile_start[], user_hostile_end[];
extern const char user_wait_start[], user_wait_end[];
extern const char user_pgrp_start[], user_pgrp_end[];
extern const char user_term_start[], user_term_end[];
extern const char user_uid_start[], user_uid_end[];
extern const char user_privsep_start[], user_privsep_end[];
extern const char user_aslr_start[], user_aslr_end[];
extern const char user_klogdev_start[], user_klogdev_end[];
extern const char user_rlimit_start[], user_rlimit_end[];
extern const char user_mremap_start[], user_mremap_end[];
extern const char user_mapflags_start[], user_mapflags_end[];
extern const char user_endsib_start[], user_endsib_end[];
extern const char user_huge_start[], user_huge_end[];
extern const char user_filemap_start[], user_filemap_end[];
extern const char user_tstp_start[], user_tstp_end[];
extern const char user_swap_start[], user_swap_end[];
extern const char user_pressure_start[], user_pressure_end[];
extern const char user_regs_start[], user_regs_end[];
extern const char user_dontneed_start[], user_dontneed_end[];
extern const char user_mapchurn_start[], user_mapchurn_end[];

/* The three numbers that tell how a mapping's pages were paid for: on first
 * touch, up front because the caller asked, and re-pointed by a move. They
 * are what turn "demand paging works" and "nothing is copied" into
 * measurements rather than claims, so they are printed together. */
static void report_mapping_counters(void) {
    kprintf("mem: %llu frames freed by unmapping\n", (unsigned long long)uspace_freed_frames());
    kprintf("mem: %llu pages backed on demand, %llu populated up front, %llu moved by mremap,"
            " %llu huge, %llu from a file\n",
            (unsigned long long)uspace_demand_pages(), (unsigned long long)uspace_populated_pages(),
            (unsigned long long)uspace_moved_pages(), (unsigned long long)uspace_huge_pages(),
            (unsigned long long)uspace_file_pages());
    /* **Whether the program discards memory at all**, which decides whether
     * honouring `MADV_DONTNEED` is on its path or beside it. A runtime's
     * scavenger is the only thing that calls it, and a zero here says a
     * whole explanation does not apply to this run — which is worth seeing
     * in every report rather than inferred from its absence. */
    kprintf("mem: %llu pages given up on advice\n",
            (unsigned long long)uspace_discarded_pages());
}

/* Whether the kernel had to catch a thread mid-flight on another hart, and
 * how long it took. Printed rather than asserted: a zero count means the
 * race was not provoked, and that is as important to see as a non-zero one
 * is — a test that cannot tell the two apart proves nothing. */
static void report_await(void) {
    uint32_t caught = 0;
    uint64_t worst = 0;
    sched_await_report(&caught, &worst);
    {
        uint32_t pokes = 0, wakes = 0, loops = 0;
        sched_await_detail(&pokes, &wakes, &loops);
        kprintf("sched: waited for %u thread(s) to leave a hart, worst %llu us\n", caught,
                (unsigned long long)worst);
        kprintf("sched:   %u pokes aimed, %u woken by a departure, %u looks, %llu pokes taken\n",
                pokes, wakes, loops, (unsigned long long)trap_software_interrupts());
    }
}

static void run_one_user_program(const char *name, const char *from, const char *to) {
    struct uproc *proc = uspace_alloc();
    int64_t status;
    int rc;
    if (proc == NULL) {
        kprintf("uspace: no free process slot for %s\n", name);
        return;
    }
    /* Through integers, not as pointers. `from` and `to` are two separate
     * linker symbols, and subtracting pointers that the standard cannot see
     * into one array is undefined behaviour however reliably it works — they
     * do bound one section here, but the compiler is not told that. Casting
     * first makes it ordinary integer arithmetic. */
    rc = uspace_build(proc, from, (uint32_t)((uintptr_t)to - (uintptr_t)from));
    if (rc != KERR_OK) {
        kprintf("uspace: could not build %s (rc %d)\n", name, rc);
        uspace_release(proc);
        return;
    }
    status = uspace_run(proc);
    kprintf("uspace: %s exited with %lld after %llu syscalls\n", name, (long long)status,
            (unsigned long long)uspace_syscalls());
    uspace_destroy(proc);
    uspace_release(proc);
}

/* What each hart actually did.
 *
 * The measurement that distinguishes "four harts are available" from "four
 * harts were used". A hart that never switched to a thread never ran one —
 * and on the architecture this migrated from, the equivalent check was
 * satisfiable by a core that had never left its idle loop, which is why
 * this counts switches rather than asking whether a hart is up. */
static void report_detectors(void);

static void report_hart_usage(void) {
    report_detectors();
    {
        uint64_t sleeps = 0, average = 0, worst = 0;
        sched_sleep_report(&sleeps, &average, &worst);
        {
            uint32_t busiest = 0;
            uint64_t times = 0;
            signal_busiest(&busiest, &times);
            kprintf("signal: %llu delivered (most often %u, %llu times), %llu frames"
                    " overwritten\n",
                    (unsigned long long)signal_delivered(), busiest, (unsigned long long)times,
                    (unsigned long long)signal_clobbered_frames());
        }
        kprintf("sched: %llu sleeps, woken %llu us late on average, %llu us at worst\n",
                (unsigned long long)sleeps, (unsigned long long)average,
                (unsigned long long)worst);
        /* Both must be zero. A missed poke is a wake-up that a timeout
         * rescued; a lost deadline is one nothing would have rescued. */
        kprintf("sched: %u missed pokes, %u lost deadlines\n", sched_missed_pokes(),
                sched_lost_deadlines());
    }
    kprintf("smp: switches/ticks by hart:");
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        kprintf(" %llu=%u/%llut", (unsigned long long)h->id, sched_hart_switches(i),
                (unsigned long long)timer_ticks_of(i));
    }
    kprintf("\n");
}

/* Runs a real program: an ELF read off the filesystem, loaded at the
 * addresses its own headers name, started with the state a C runtime
 * expects. This is the path a Go binary will take. */
/* The largest `/init` this will read.
 *
 * **Sixteen megabytes, because a Go init with its symbols is five.** Four
 * was chosen when the only thing being loaded was a hand-written test
 * program, and a real userland walked into it: the board answered EFBIG and
 * the whole boot stopped at a number nobody had revisited. `execve`'s own
 * limit is eight, so this being smaller meant init had a tighter bound than
 * the programs it starts — which is the wrong way round. */
#define INIT_MAX_BYTES (16u * 1024u * 1024u)

/* Exercises the write path, which had never been run at all.
 *
 * **`xfs_create` had no caller.** Create, unlink and write were written,
 * compiled and reachable from nothing — so "writing works for the simple
 * cases" was a statement about the code existing. This is the test that
 * turns it into a statement about behaviour, and it is a board test because
 * the filesystem it writes to is a RAM disk the boot loader fetched
 * (`minikern.disk=ram`): the image is discarded at the next power cycle, so
 * a test that corrupts it costs nothing and the operator's card is never
 * touched.
 *
 * The sequence is chosen so that each step checks what the previous one
 * claimed, rather than only its return value: a file is created and read
 * back, linked and found under both names with a count of two, renamed and
 * found under neither the old name nor the replaced one, and unlinked once
 * — after which the *other* name must still read the same bytes, which is
 * the whole point of a link count and the case `unlink` used to refuse.
 *
 * Each step prints its own line, because "the write test failed" is not
 * something anybody can act on.
 */
static bool wrote_and_checked(const char *what, int rc) {
    kprintf("fswrite: %-28s %s (rc %d)\n", what, rc == KERR_OK ? "ok" : "FAILED", rc);
    return rc == KERR_OK;
}

/* **A symlink written and read back as a symlink.**
 *
 * The round trip is the test, not the creation: an inode whose mode says
 * link and whose data is the target is easy to write and easy to write
 * *wrong* — the format byte left saying EXTENTS, or the size left at zero —
 * and either produces something `xfs_repair` offers to delete while every
 * creation call still reports success. Reading it back through the ordinary
 * path is what proves the two sides agree.
 *
 * A relative target on purpose: `../bin/sh` is the ordinary case and the one
 * a path-validating copy-in would have rejected. */
static int symlink_round_trip(void) {
    const char *target = "../bin/sh";
    char got[64];
    uint32_t n = 0;
    int rc = xfsmount_symlink("/w_link", target);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_readlink("/w_link", got, sizeof got, &n);
    if (rc != KERR_OK) {
        return rc;
    }
    if (n != 9u) {
        kprintf("fswrite:   the target came back %u bytes, want 9\n", n);
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (got[i] != target[i]) {
            kprintf("fswrite:   the target differs at byte %u\n", i);
            return KERR_INVAL;
        }
    }
    return KERR_OK;
}

/* A target too long for the inode is refused rather than half-written.
 *
 * The bound is the literal area, so the test asks the filesystem for its own
 * limit instead of quoting a number that changes with the inode size. */
static int long_target_refused(void) {
    char target[600];
    uint32_t max = xfsmount_symlink_limit();
    uint32_t len = max + 1u < sizeof target ? max + 1u : (uint32_t)sizeof target - 1u;
    int rc;
    if (max == 0 || len <= max) {
        return KERR_OK; /* no mount, or an inode big enough that this cannot be tested */
    }
    for (uint32_t i = 0; i < len; i++) {
        target[i] = 'x';
    }
    target[len] = '\0';
    rc = xfsmount_symlink("/w_toolong", target);
    if (rc == KERR_OK) {
        kprintf("fswrite:   a %u-byte target was accepted past a %u-byte limit\n", len, max);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* readlink on a regular file is EINVAL, which is the check that stops a
 * caller mistaking a file's contents for a link's target. */
static int readlink_refuses_a_file(void) {
    char got[32];
    uint32_t n = 0;
    int rc = xfsmount_readlink("/w1", got, sizeof got, &n);
    if (rc == KERR_OK) {
        kprintf("fswrite:   readlink returned %u bytes for a regular file\n", n);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* A rename through the path-level call, which is the one a program reaches.
 *
 * The write self-test already renames through the filesystem's own call; this
 * exercises the layer above it — the one that splits two paths into two
 * directories and two leaves, holds two directory inodes at once, and is
 * where a frame-size refusal or a mixed-up argument order would live. The
 * old name must be gone *and* the new one present: either alone is a bug
 * that reads as success.
 */
static int rename_moves_a_name(void) {
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    uint32_t mode = 0;
    uint64_t fsize = 0;
    uint32_t nlink = 0;
    int rc = xfsmount_rename_path("/w_link", "/w_moved");
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfsmount_stat_full("/w_link", &mode, &fsize, &nlink) == KERR_OK) {
        kprintf("fswrite:   the old name is still there after a rename\n");
        return KERR_INVAL;
    }
    rc = xfsmount_stat_full("/w_moved", &mode, &fsize, &nlink);
    if (rc != KERR_OK) {
        kprintf("fswrite:   the new name is not there after a rename\n");
        return rc;
    }
    (void)bytes;
    (void)size;
    return KERR_OK;
}

/* A mode written and read back, which is the whole of chmod working.
 *
 * The permission bits only: the read compares against `0777` of the mode,
 * because the type bits are not the caller's to set and a test that compared
 * the whole word would fail for the right reason and look like the wrong
 * one. */
/* **There is no cwd test here, and this is the fourth time that mattered.**
 *
 * This self-test runs during boot with no current *process*: `uspace_current()`
 * is NULL, so a working directory cannot be set and a relative path has
 * nothing to resolve against. A version of this test ran, reported that it
 * could not run, and still printed `ok` — a pass that proves nothing, which
 * is worse than an absence.
 *
 * The pattern is now plain and worth stating once: **this self-test can
 * exercise filesystem operations and not process ones.** The seek test, the
 * umask test and this one all failed or lied for the same reason. Anything
 * that reads the current process — a descriptor, a credential, a working
 * directory — belongs in a user-mode test, and `minikern.execone` is the
 * pattern for one.
 *
 * What *is* tested below needs no process: the dot-component refusal, which
 * holds whatever the working directory is.
 */

/* **A dot component is refused, and that is deliberate.** Collapsing `..`
 * textually is the classic way out of a sandbox, because a `..` through a
 * symlink does not mean what the path says. Until the resolver understands
 * the tree, anything containing a dot component is `ENOENT` — and a test
 * that asserts the refusal is what stops someone "fixing" it with a string
 * operation. */
static int dots_are_refused(void) {
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t nlink = 0;
    if (syscall_resolve_for_test("../w1", &mode, &size, &nlink) == KERR_OK) {
        kprintf("fswrite:   a path with .. was resolved\n");
        return KERR_INVAL;
    }
    if (syscall_resolve_for_test("./w1", &mode, &size, &nlink) == KERR_OK) {
        kprintf("fswrite:   a path with . was resolved\n");
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* **The accounting is the test, not the size.** Setting `di_size` is one
 * line; the part that corrupts quietly is the free-block count, because a
 * filesystem whose counter disagrees with its allocation groups mounts,
 * reads and writes perfectly and is condemned by `xfs_repair` later. So this
 * writes enough to own several blocks, truncates to nothing, and asserts
 * that the volume's free count went *up* by what the file owned. */
static int truncate_frees_blocks(void) {
    static uint8_t body[32768];
    struct xfs_counters before = {0};
    struct xfs_counters after = {0};
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t nlink = 0;
    int rc;
    for (uint32_t i = 0; i < sizeof body; i++) {
        body[i] = (uint8_t)i;
    }
    rc = xfsmount_write_at("/", "w_trunc", 0, body, (uint32_t)sizeof body);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_counters("/", &before);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_truncate_path("/w_trunc", 0);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_counters("/", &after);
    if (rc != KERR_OK) {
        return rc;
    }
    if (after.free_blocks <= before.free_blocks) {
        kprintf("fswrite:   truncating 32K freed nothing: %llu then %llu blocks free\n",
                (unsigned long long)before.free_blocks, (unsigned long long)after.free_blocks);
        return KERR_INVAL;
    }
    rc = xfsmount_stat_full("/w_trunc", &mode, &size, &nlink);
    if (rc != KERR_OK) {
        return rc;
    }
    if (size != 0) {
        kprintf("fswrite:   after truncating to 0 the size is %llu\n", (unsigned long long)size);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* **Rounding up, which is where the data would be lost.** A file of one
 * byte still owns the block holding it, so truncating a larger file down to
 * one byte must free everything *after* that block and not the block
 * itself. Rounding down would free it, and the byte the caller kept would
 * read back as zero — a loss that looks like success. */
static int truncate_rounds_up(void) {
    static uint8_t body[8192];
    uint8_t got[4] = {0};
    uint8_t *bytes = NULL;
    uint32_t read = 0;
    int rc;
    for (uint32_t i = 0; i < sizeof body; i++) {
        body[i] = 'A';
    }
    rc = xfsmount_write_at("/", "w_round", 0, body, (uint32_t)sizeof body);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_truncate_path("/w_round", 1);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_read_file("/w_round", 64u, &bytes, &read);
    if (rc != KERR_OK) {
        return rc;
    }
    got[0] = read > 0 ? bytes[0] : 0;
    kfree(bytes);
    if (read != 1 || got[0] != 'A') {
        kprintf("fswrite:   after truncating to 1 the file is %u bytes starting %02x\n", read,
                got[0]);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* Growing allocates nothing: a hole is the absence of an extent, so a
 * truncate past the end must not cost blocks. The free count staying put is
 * the assertion — a version that filled the gap would turn a one-line call
 * into as many writes as the gap is long. */
static int truncate_grows_sparsely(void) {
    struct xfs_counters before = {0};
    struct xfs_counters after = {0};
    int rc = xfsmount_counters("/", &before);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_truncate_path("/w_round", 1u << 20);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_counters("/", &after);
    if (rc != KERR_OK) {
        return rc;
    }
    if (after.free_blocks != before.free_blocks) {
        kprintf("fswrite:   growing to 1M cost %llu blocks\n",
                (unsigned long long)(before.free_blocks - after.free_blocks));
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* **The test that stops `umask` being a stored number nobody reads.**
 *
 * A per-process field that nothing consults is the failure this tree has
 * recorded seven times, and the only defence is a test that asserts what a
 * created file's mode actually is. The mask is installed by the composition
 * root, so this exercises the whole path: the field, the installed reader,
 * and the create.
 *
 * `0022` is the ordinary mask and `0666 & ~0022` is `0644`, which is what
 * every Unix gives a new file. Zero afterwards, because the rest of this
 * self-test expects what it always got.
 */
static uint32_t test_umask_source(void) {
    return 0022u;
}

static int umask_is_consulted(void) {
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t nlink = 0;
    int rc;
    /* **The mask is installed rather than set on a process**, because this
     * self-test has no current process — `uspace_current()` is NULL here, so
     * setting a process field does nothing and the first version of this
     * test failed for that reason rather than for a real one. Installing a
     * source exercises what lives in the filesystem: `create_permissions`
     * and the create path. The process field and its reader are three lines
     * and are checked by the syscall's own contract. */
    xfsmount_set_umask_source(test_umask_source);
    rc = xfsmount_write_at("/", "w_umask", 0, "x", 1u);
    xfsmount_set_umask_source(uspace_current_umask);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_stat_full("/w_umask", &mode, &size, &nlink);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((mode & 07777u) != 0644u) {
        kprintf("fswrite:   a create under umask 022 gave mode %o, want 644\n", mode & 07777u);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* `statfs`'s counters, checked for being *real* rather than for a value.
 *
 * The block count is the filesystem's own and the free count must be less
 * than it and more than zero — a fixture with a few files in it is neither
 * empty nor full. Asserting an exact number would pin the fixture's
 * contents, which is not what this is about.
 */
static int statfs_is_real(void) {
    struct xfs_counters c = {0};
    int rc = xfsmount_counters("/", &c);
    if (rc != KERR_OK) {
        return rc;
    }
    if (c.blocksize != 4096u || c.blocks == 0) {
        kprintf("fswrite:   statfs says %llu blocks of %u bytes\n",
                (unsigned long long)c.blocks, c.blocksize);
        return KERR_INVAL;
    }
    if (c.free_blocks == 0 || c.free_blocks >= c.blocks) {
        kprintf("fswrite:   statfs says %llu of %llu blocks free\n",
                (unsigned long long)c.free_blocks, (unsigned long long)c.blocks);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* **Why there is no seek test here**, and where one belongs.
 *
 * `lseek` and `fsync` are implemented, and the obvious place to prove them
 * looked like this self-test. It is the wrong place: this runs during boot
 * with no current *process*, so `table_of_current()` is NULL and
 * `fd_install` answers `KERR_BADF` — every descriptor-based operation fails
 * here for a reason that has nothing to do with the operation. The first
 * attempt failed with exactly that and read as a broken seek.
 *
 * Anything fd-based has to be tested from user mode, where there is a
 * descriptor table. `minikern.execone` is the pattern: a small assembly
 * program that makes the syscalls and reports. That is the right home for a
 * seek test and it is not written yet — which is said here rather than left
 * as a gap somebody has to rediscover.
 */

/* The attribute tests' own file.
 *
 * **Created here rather than borrowed.** The first version used `/w1`, which
 * an earlier step in this self-test unlinks — so the mode test failed with
 * ENOENT and looked like a broken chmod. A test that depends on another
 * test's leftovers is a test that reports the wrong thing. */
static int make_attr_subject(void) {
    return xfsmount_write_at("/", "w_attr", 0, "x", 1u);
}

static int chmod_round_trip(void) {
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t nlink = 0;
    int rc = make_attr_subject();
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_chmod_path("/w_attr", 0640u);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_stat_full("/w_attr", &mode, &size, &nlink);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((mode & 07777u) != 0640u) {
        kprintf("fswrite:   the mode came back %o, want 640\n", mode & 07777u);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* **The check that matters more than the round trip.** A caller passing a
 * whole mode word must not be able to turn a regular file into a directory
 * or a symlink: the type bits come from disk and the caller's are masked
 * away. Asking for a directory's type on a regular file and finding it still
 * a regular file is what proves that. */
static int chmod_keeps_the_type(void) {
    uint32_t mode = 0;
    uint64_t size = 0;
    uint32_t nlink = 0;
    int rc = xfsmount_chmod_path("/w_attr", 0040755u); /* S_IFDIR | 0755 */
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_stat_full("/w_attr", &mode, &size, &nlink);
    if (rc != KERR_OK) {
        return rc;
    }
    if ((mode & 0170000u) != 0100000u) {
        kprintf("fswrite:   the type became %o; a chmod changed what the file is\n",
                mode & 0170000u);
        return KERR_INVAL;
    }
    if ((mode & 07777u) != 0755u) {
        kprintf("fswrite:   the permissions came back %o, want 755\n", mode & 07777u);
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* An owner written and read back, including the -1 convention: changing the
 * group alone must leave the owner where it was, which is the case a
 * version that always wrote both would break silently. */
static int chown_round_trip(void) {
    uint32_t uid = 0;
    uint32_t gid = 0;
    int rc = xfsmount_chown_path("/w_attr", 1000u, 1001u);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_owner_of("/w_attr", &uid, &gid);
    if (rc != KERR_OK) {
        return rc;
    }
    if (uid != 1000u || gid != 1001u) {
        kprintf("fswrite:   the owner came back %u:%u, want 1000:1001\n", uid, gid);
        return KERR_INVAL;
    }
    rc = xfsmount_chown_path("/w_attr", XFS_OWNER_UNCHANGED, 1002u);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_owner_of("/w_attr", &uid, &gid);
    if (rc != KERR_OK) {
        return rc;
    }
    if (uid != 1000u || gid != 1002u) {
        kprintf("fswrite:   after changing only the group: %u:%u, want 1000:1002\n", uid, gid);
        return KERR_INVAL;
    }
    return KERR_OK;
}

static int read_back(const char *name, const char *want, uint32_t len) {
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    char path[64];
    uint32_t n = 0;
    int rc;
    path[n++] = '/';
    while (name[n - 1u] != '\0' && n + 1u < sizeof path) {
        path[n] = name[n - 1u];
        n++;
    }
    path[n] = '\0';
    rc = xfsmount_read_file(path, INIT_MAX_BYTES, &bytes, &size);
    if (rc != KERR_OK) {
        return rc;
    }
    /* **Which part was wrong, not merely that something was.** A single
     * `KERR_IO` for "not found", "wrong length" and "wrong bytes" sends the
     * next person to read three different pieces of code. */
    if (size != len) {
        kprintf("fswrite: %s is %u bytes, wanted %u\n", path, size, len);
        kfree(bytes);
        return KERR_IO;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (bytes[i] != (uint8_t)want[i]) {
            kprintf("fswrite: %s differs at byte %u: 0x%x not 0x%x\n", path, i, bytes[i],
                    (uint8_t)want[i]);
            kfree(bytes);
            return KERR_IO;
        }
    }
    kfree(bytes);
    return KERR_OK;
}

/* Fills a directory until it must change shape, then empties it.
 *
 * **The step that proves the conversion.** A shortform directory lives in
 * its inode and holds about ten names; past that it has to become a block
 * directory, and this kernel used to answer `KERR_NOSPC` and stop. Forty
 * names is comfortably past the boundary, so the conversion happens in the
 * middle of the loop without the test having to know where — and then every
 * name is read back *after* all of them are added, because a conversion
 * that loses the entries it was converting would otherwise pass.
 *
 * Removing them again matters as much: a block directory's entries leave
 * free spans behind, and the next addition has to reuse one. The second
 * pass of adds is what tests that.
 */
/* How many names to add.
 *
 * Past shortform's dozen, so the directory must become a block directory.
 * Past the sixty-odd free inodes a fresh filesystem's first chunk holds, so
 * the inode allocator must make a **second chunk**. And past the hundred
 * and twenty a single block holds, so the directory must become a **leaf**
 * directory. Each of those was a gap this number found by being raised:
 * twenty stopped at shortform, sixty at an allocator that would not grow,
 * two hundred at a directory that could not leave one block, four hundred
 * at a leaf directory that could not have a second data block, and seven
 * hundred at a leaf whose hashes no longer fit in one block, and twelve
 * hundred at a node directory whose two leaves were themselves full.
 * Asking for more than the last run managed is how a test finds the next
 * thing rather than confirming the last one. */
#define NAMES 1200u

static int grow_a_directory(void) {
    char name[8] = "g00";
    uint64_t ino = 0;
    uint32_t links = 0;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t i = 0; i < NAMES; i++) {
            name[1] = (char)('0' + i / 10u);
            name[2] = (char)('0' + i % 10u);
            int rc = xfsmount_create(name, 0100644u);
            if (rc != KERR_OK) {
                kprintf("fswrite:   pass %u: create %s failed (rc %d)\n", pass, name, rc);
                xfsmount_root_shape();
                return rc;
            }
        }
        for (uint32_t i = 0; i < NAMES; i++) {
            name[1] = (char)('0' + i / 10u);
            name[2] = (char)('0' + i % 10u);
            int rc = xfsmount_inode_of(name, &ino, &links);
            if (rc != KERR_OK) {
                kprintf("fswrite:   pass %u: %s is not there (rc %d)\n", pass, name, rc);
                return rc;
            }
        }
        for (uint32_t i = 0; i < NAMES; i++) {
            name[1] = (char)('0' + i / 10u);
            name[2] = (char)('0' + i % 10u);
            int rc = xfsmount_unlink(name);
            if (rc != KERR_OK) {
                kprintf("fswrite:   pass %u: unlink %s failed (rc %d)\n", pass, name, rc);
                return rc;
            }
        }
    }
    return KERR_OK;
}

/* Writes until the allocator has to leave the group it started in.
 *
 * **The only way to reach that path is to fill a group**, and the fixture's
 * are the default four of eighty megabytes — raising the count to make them
 * smaller makes `mkfs` demand a log that will not fit in one. So this
 * writes twenty thousand blocks, which costs nothing against a RAM disk.
 * It stops the moment the counter moves rather than writing a fixed
 * amount, because how much it takes depends on where `mkfs` put the log.
 *
 * The bytes are checked afterwards: an allocation from a second group is
 * also a second *extent*, and a file whose extents are right in the inode
 * and wrong on disk reads back as somebody else's data. */
static int fill_past_one_group(void) {
    static uint8_t chunk[65536];
    uint64_t at = 0;
    for (uint32_t i = 0; i < sizeof chunk; i++) {
        chunk[i] = (uint8_t)(i * 7u + 3u); /* neither zeroes nor random */
    }
    int rc = xfsmount_create("gg", 0100644u);
    while (rc == KERR_OK && xfsmount_alloc_other_group() == 0 && at < 120u * 1024u * 1024u) {
        rc = xfsmount_write("gg", at, chunk, sizeof chunk);
        at += sizeof chunk;
    }
    if (rc != KERR_OK) {
        return rc;
    }
    if (xfsmount_alloc_other_group() == 0) {
        return KERR_NOSPC; /* never crossed: the test proved nothing */
    }
    kprintf("fswrite: crossed a group after %llu KiB\n", (unsigned long long)(at / 1024u));
    {
        static uint8_t got[sizeof chunk];
        rc = xfsmount_read_at("gg", at - sizeof chunk, got, sizeof got);
        for (uint32_t i = 0; rc == KERR_OK && i < sizeof got; i++) {
            if (got[i] != chunk[i]) {
                /* **Where, and what instead.** A bare `KERR_IO` here says
                 * only that a cross-group file read back wrong, and the two
                 * ways that happens — a missing extent, which reads as
                 * zeroes, and a misaddressed one, which reads as another
                 * file — want different investigations. */
                kprintf("fswrite: gg differs at %llu+%u: 0x%x not 0x%x\n",
                        (unsigned long long)(at - sizeof chunk), i, got[i], chunk[i]);
                rc = KERR_IO;
            }
        }
    }
    return rc;
}

/* The control: the same write, with the search switched off, must fail.
 *
 * **Without this the test above proves only that writing works.** The first
 * group is full by the time it runs, so an allocator confined to it has
 * nowhere to go — and if this *succeeds*, the crossing above happened for
 * some other reason and the claim is wrong. The guarantee and its control
 * belong in one run on one machine; this project has the scar for learning
 * that twice. */
static int one_group_refuses(void) {
    static uint8_t chunk[65536];
    int rc = xfsmount_create("gh", 0100644u);
    xfsmount_single_group(true);
    for (uint64_t at = 0; rc == KERR_OK && at < 4u * 1024u * 1024u; at += sizeof chunk) {
        rc = xfsmount_write("gh", at, chunk, sizeof chunk);
    }
    xfsmount_single_group(false);
    return rc == KERR_NOSPC ? KERR_OK : (rc == KERR_OK ? KERR_IO : rc);
}

/* A filesystem left as a power cut leaves it, then mounted again.
 *
 * **The kernel dirties its own log** rather than a fixture script patching
 * a byte: it logs a transaction and omits the unmount record, which is
 * exactly what a machine losing power mid-write produces. A hand-damaged
 * log proves the parser; a log this kernel wrote and failed to close proves
 * the path a real machine takes.
 *
 * What is asserted is the whole arrangement: the filesystem **mounts** (a
 * power cut must not make a disk unreadable), its log is seen to have been
 * **dirty**, it was **replayed**, it is therefore **writable** again, and
 * the file written before the cut is still there. Replay is the step that
 * turns "readable" into "as it was", and without the writable check the
 * test would pass on a kernel that only checked and mounted read-only. */
static int dirty_then_remount(void) {
    static const char text[] = "written before the power went";
    int rc = xfsmount_create("d1", 0100644u);
    rc = rc != KERR_OK ? rc : xfsmount_write("d1", 0, text, sizeof text - 1u);
    if (rc != KERR_OK) {
        return rc;
    }
    xfsmount_leave_log_dirty(true);
    rc = xfsmount_create("d2", 0100644u);
    xfsmount_leave_log_dirty(false);
    if (rc != KERR_OK) {
        return rc;
    }
    rc = xfsmount_remount();
    if (rc != KERR_OK) {
        kprintf("fswrite: a dirty filesystem would not mount (rc %d)\n", rc);
        return rc;
    }
    if (!xfsmount_log_was_dirty()) {
        return KERR_IO; /* the log closed cleanly: the test proved nothing */
    }
    if (!xfsmount_log_replayed() || !xfsmount_writable()) {
        kprintf("fswrite: replayed %u, writable %u\n", xfsmount_log_replayed() ? 1u : 0u,
                xfsmount_writable() ? 1u : 0u);
        return KERR_IO;
    }
    rc = read_back("d1", text, sizeof text - 1u);
    kprintf("fswrite: after replay, /d1 reads rc %d\n", rc);
    if (rc != KERR_OK) {
        return rc;
    }
    /* And it really is writable, not merely marked so. */
    rc = xfsmount_create("d3", 0100644u);
    kprintf("fswrite: after replay, create /d3 rc %d\n", rc);
    return rc;
}

/* The control: with replay refused, the same filesystem still mounts and is
 * still read-only.
 *
 * **Without this, "replayed" would be unfalsifiable.** The read-only
 * fallback is what a kernel does when it meets a log item it cannot apply,
 * so it is a live path rather than dead history — and a test that only ever
 * saw the replay succeed could not tell a working fallback from one that
 * had rotted. */
static int refused_replay_is_read_only(void) {
    int rc;
    xfsmount_leave_log_dirty(true);
    rc = xfsmount_create("d4", 0100644u);
    xfsmount_leave_log_dirty(false);
    if (rc != KERR_OK) {
        return rc;
    }
    xfsmount_refuse_replay(true);
    rc = xfsmount_remount();
    xfsmount_refuse_replay(false);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!xfsmount_log_was_dirty() || xfsmount_log_replayed() || xfsmount_writable()) {
        return KERR_IO;
    }
    return KERR_OK;
}

/* Counts what a listing reports, and notices the one it is looking for. */
struct attr_tally {
    uint32_t seen;
    bool found_colour;
};

static int tally_attr(void *self, const uint8_t *name, uint32_t namelen, uint8_t flags,
                      uint32_t valuelen) {
    struct attr_tally *t = self;
    t->seen++;
    /* `flags` is the namespace alone, so zero means `user.` whether the
     * attribute is in the inode or in a leaf block. */
    t->found_colour = t->found_colour || (namelen == 6u && name[0] == 'c' && flags == 0);
    (void)valuelen;
    return KERR_OK;
}

/* Extended attributes: set, read back, list, remove — and then **mount the
 * filesystem again** and read them once more.
 *
 * The remount is the part that matters. Everything before it would pass on
 * a kernel that kept attributes in a variable: the writer and the reader
 * are the same code and would agree with each other about anything. Only
 * re-reading the inode from the disk proves the bytes went there, and the
 * shortform fork lives in the inode's literal area, which is also the one
 * place where getting the offset wrong would quietly overwrite the file's
 * own extent list.
 *
 * Three namespaces, because XFS stores the namespace as a **flag** and the
 * name without its prefix: a test using only `user.` would pass just as
 * well on a kernel that stored the prefix as part of the name, which Linux
 * would then not find. */
static int attributes_round_trip(void) {
    static const uint8_t purple[] = {'p', 'u', 'r', 'p', 'l', 'e'};
    static const uint8_t level[] = {'9'};
    uint8_t out[32];
    uint32_t len = 0;
    struct attr_tally t = {0, false};
    uint64_t conversions = 0;
    int rc = xfsmount_create("a1", 0100644u);
    rc = rc != KERR_OK ? rc : xfsmount_attr_set("a1", "user.colour", purple, sizeof purple);
    rc = rc != KERR_OK ? rc : xfsmount_attr_set("a1", "trusted.level", level, sizeof level);
    if (rc != KERR_OK) {
        kprintf("fswrite: setting an attribute failed (rc %d)\n", rc);
        return rc;
    }
    /* A name in a namespace this kernel does not know is refused, not
     * stored as a user attribute: a name that round-trips here and means
     * something else to Linux is worse than one that was rejected. */
    if (xfsmount_attr_set("a1", "nonsense.x", level, 1u) == KERR_OK) {
        return KERR_IO;
    }
    rc = xfsmount_attr_get("a1", "user.colour", out, sizeof out, &len);
    if (rc != KERR_OK || len != sizeof purple || out[0] != 'p' || out[5] != 'e') {
        kprintf("fswrite: user.colour read back wrong (rc %d, %u bytes)\n", rc, len);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    rc = xfsmount_attr_list("a1", tally_attr, &t);
    if (rc != KERR_OK || t.seen != 2u || !t.found_colour) {
        kprintf("fswrite: listing saw %u attribute(s)\n", t.seen);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    /* **Across a mount.** The attributes have to be on the disk. */
    rc = xfsmount_remount();
    if (rc != KERR_OK) {
        return rc;
    }
    len = 0;
    rc = xfsmount_attr_get("a1", "trusted.level", out, sizeof out, &len);
    if (rc != KERR_OK || len != 1u || out[0] != '9') {
        kprintf("fswrite: trusted.level did not survive the mount (rc %d)\n", rc);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    /* And the namespaces really are distinct: `user.level` was never set. */
    if (xfsmount_attr_get("a1", "user.level", out, sizeof out, &len) != KERR_NOENT) {
        return KERR_IO;
    }
    /* **Past what the inode holds**, so the fork has to become a leaf
     * block. Half a 512-byte inode's literal area is 168 bytes, and each of
     * these costs about thirty, so a dozen crosses it comfortably. Without
     * this the leaf writer would never run — and the leaf *reader* would
     * stay unexercised too, which is how its name/value layout came to be
     * wrong in the first place. */
    for (uint32_t i = 0; rc == KERR_OK && i < 12u; i++) {
        char attr[20] = "user.filler00";
        uint8_t body[16];
        attr[11] = (char)('0' + i / 10u);
        attr[12] = (char)('0' + i % 10u);
        for (uint32_t k = 0; k < sizeof body; k++) {
            body[k] = (uint8_t)('a' + i);
        }
        rc = xfsmount_attr_set("a1", attr, body, sizeof body);
    }
    conversions = xfsmount_attrs_to_leaf();
    if (rc != KERR_OK || conversions == 0) {
        kprintf("fswrite: the attribute fork did not become a leaf (rc %d, %llu)\n", rc,
                (unsigned long long)conversions);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    /* Everything still there, through the leaf: the two originals and the
     * twelve fillers. And read **after a mount**, so the leaf block is read
     * from the disk rather than from the buffer that wrote it. */
    rc = xfsmount_remount();
    if (rc != KERR_OK) {
        return rc;
    }
    len = 0;
    rc = xfsmount_attr_get("a1", "user.filler07", out, sizeof out, &len);
    if (rc != KERR_OK || len != 16u || out[0] != 'h') {
        kprintf("fswrite: a leaf attribute read back wrong (rc %d, %u bytes, 0x%x)\n", rc,
                len, len > 0 ? out[0] : 0u);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    t.seen = 0;
    t.found_colour = false;
    rc = xfsmount_attr_list("a1", tally_attr, &t);
    if (rc != KERR_OK || t.seen != 14u || !t.found_colour) {
        kprintf("fswrite: the leaf listed %u attribute(s), wanted 14\n", t.seen);
        return rc != KERR_OK ? rc : KERR_IO;
    }
    /* **A value that cannot live in a block with its name**, so it goes
     * into blocks of its own. Three kilobytes needs one on a 4 KiB
     * filesystem; the bytes are a pattern rather than a constant so that a
     * block written at the wrong offset, or two blocks swapped, is a
     * mismatch rather than a coincidence. */
    {
        static uint8_t big[3000];
        static uint8_t got[3000];
        for (uint32_t i = 0; i < sizeof big; i++) {
            big[i] = (uint8_t)(i * 31u + 7u);
        }
        rc = xfsmount_attr_set("a1", "user.blob", big, sizeof big);
        if (rc != KERR_OK) {
            kprintf("fswrite: a remote value would not be written (rc %d)\n", rc);
            return rc;
        }
        /* Across a mount, so the value's blocks are read from the disk. */
        rc = xfsmount_remount();
        rc = rc != KERR_OK ? rc
                           : xfsmount_attr_get("a1", "user.blob", got, sizeof got, &len);
        if (rc != KERR_OK || len != sizeof big) {
            kprintf("fswrite: a remote value read back %u bytes (rc %d)\n", len, rc);
            return rc != KERR_OK ? rc : KERR_IO;
        }
        for (uint32_t i = 0; i < sizeof big; i++) {
            if (got[i] != big[i]) {
                kprintf("fswrite: the remote value differs at byte %u\n", i);
                return KERR_IO;
            }
        }
        /* And a buffer too small is refused rather than half filled. */
        if (xfsmount_attr_get("a1", "user.blob", got, 16u, &len) != KERR_RANGE) {
            return KERR_IO;
        }
        kprintf("fswrite: a %u-byte value went to its own blocks and came back\n",
                (uint32_t)sizeof big);
    }
    rc = xfsmount_attr_remove("a1", "user.colour");
    rc = rc != KERR_OK ? rc
                       : (xfsmount_attr_get("a1", "user.colour", out, sizeof out, &len) ==
                                  KERR_NOENT
                              ? KERR_OK
                              : KERR_IO);
    /* Removing one must not remove the other. */
    rc = rc != KERR_OK ? rc : xfsmount_attr_get("a1", "trusted.level", out, sizeof out, &len);
    /* **The captured count, not a fresh read.** The mount below this point
     * resets the filesystem's counters, so reading them at the end reports
     * zero for work that certainly happened — which it did, and said `0 to
     * leaf` on a run where the conversion had just been asserted a few
     * lines above. Third time in this session that a re-mount wiped a
     * number that was then read: the rule is that a counter is captured
     * where it is meaningful, not where it is convenient to print. */
    kprintf("fswrite: attributes set, listed, survived a mount, removed; %llu to leaf\n",
            (unsigned long long)conversions);
    return rc;
}

/* Reproduces exactly what the writable descriptor did, against whatever
 * filesystem is mounted.
 *
 * **Written because that path corrupted a real card**: the operator's root
 * filesystem went from 8 entries to 1116 with names that are not text, and
 * the files themselves still read — so the damage is in the root
 * directory's own blocks. The descriptor layer did two things no other
 * caller does, and this does both in the same order so that a fixture image
 * can say which one is at fault:
 *
 *   1. a **zero-length create** at open, `write_at(dir, leaf, 0, NULL, 0)`;
 *   2. **chunked writes at increasing offsets**, 32 KiB at a time, which is
 *      what a Go `io.Copy` produces.
 *
 * It then lists the root and counts, because the corruption's signature is
 * the entry count rather than any one failure — every individual call
 * returned success on the card. A test that only checked return codes would
 * have passed on the run that did the damage. */
#define CHUNK_PROBE_BYTES 32768u
#define CHUNK_PROBE_CHUNKS 3u

static int32_t count_root_entries(void) {
    return xfsmount_list_root();
}

static void run_chunked_write_probe(void) {
    static uint8_t chunk[CHUNK_PROBE_BYTES];
    int32_t before, after;
    int rc;
    if (!xfsmount_writable()) {
        kprintf("chunked: this filesystem may not be written\n");
        return;
    }
    for (uint32_t i = 0; i < CHUNK_PROBE_BYTES; i++) {
        chunk[i] = (uint8_t)(i ^ 0x5au);
    }
    before = count_root_entries();
    /* The zero-length create the descriptor layer does at open. */
    rc = xfsmount_write_at("/", "chunky", 0, NULL, 0);
    kprintf("chunked: the zero-length create said rc %d\n", rc);
    for (uint32_t c = 0; c < CHUNK_PROBE_CHUNKS && rc == KERR_OK; c++) {
        rc = xfsmount_write_at("/", "chunky", (uint64_t)c * CHUNK_PROBE_BYTES, chunk,
                               CHUNK_PROBE_BYTES);
        kprintf("chunked: chunk %u at offset %u said rc %d\n", c, c * CHUNK_PROBE_BYTES, rc);
    }
    after = count_root_entries();
    /* **The count is the assertion.** Every call above can return success
     * while the directory grows by hundreds of entries, which is exactly
     * what happened on the card. */
    kprintf("chunked: the root had %d entries and now has %d\n", before, after);
    if (after != before + 1) {
        kprintf("chunked: **the directory gained %d entries for one file**\n", after - before);
    }
    (void)xfsmount_unlink_path("/chunky");
}

static void run_write_selftest(void) {
    static const char text[] = "a file this kernel wrote";
    const uint32_t len = (uint32_t)(sizeof text - 1u);
    uint64_t one = 0, two = 0;
    uint32_t links = 0;
    bool ok = true;
    if (!xfsmount_writable()) {
        kprintf("fswrite: this filesystem may not be written\n");
        return;
    }
    /* **Cleared first, so the test can be run twice.** It left `/w1` and
     * `/w2` on the card, and the second run then failed at its first step
     * with `EEXIST` — which reads as "writing is broken" when it means "it
     * worked last time". A test that only passes on a fresh disk is a test
     * that stops being runnable the moment it passes. Failures here are
     * ignored on purpose: the files are usually absent, and that is not an
     * error. */
    (void)xfsmount_unlink_path("/w2");
    (void)xfsmount_unlink_path("/w1");
    ok = ok && wrote_and_checked("create /w1", xfsmount_create("w1", 0100644u));
    ok = ok && wrote_and_checked("write /w1", xfsmount_write("w1", 0, text, len));
    ok = ok && wrote_and_checked("read /w1 back", read_back("w1", text, len));
    ok = ok && wrote_and_checked("link /w1 -> /w2", xfsmount_link("w1", "w2"));
    ok = ok && wrote_and_checked("read /w2 back", read_back("w2", text, len));
    if (ok && xfsmount_inode_of("w2", &two, &links) == KERR_OK) {
        ok = wrote_and_checked("two names, one inode", links == 2u ? KERR_OK : KERR_IO);
        (void)xfsmount_inode_of("w1", &one, &links);
        ok = ok && wrote_and_checked("the same inode", one == two ? KERR_OK : KERR_IO);
    }
    ok = ok && wrote_and_checked("rename /w2 -> /w3", xfsmount_rename("w2", "w3"));
    ok = ok && wrote_and_checked("read /w3 back", read_back("w3", text, len));
    ok = ok && wrote_and_checked("/w2 is gone",
                                 xfsmount_inode_of("w2", &two, &links) == KERR_OK ? KERR_IO
                                                                                  : KERR_OK);
    /* The link count's reason for existing: removing one name must leave
     * the file reachable under the other, with its contents intact. */
    ok = ok && wrote_and_checked("unlink /w1", xfsmount_unlink("w1"));
    ok = ok && wrote_and_checked("/w3 survives it", read_back("w3", text, len));
    ok = ok && wrote_and_checked("unlink /w3", xfsmount_unlink("w3"));
    ok = ok && wrote_and_checked("/w3 is gone",
                                 xfsmount_inode_of("w3", &two, &links) == KERR_OK ? KERR_IO
                                                                                  : KERR_OK);
    ok = ok && wrote_and_checked("grow past shortform", grow_a_directory());
    /* **Said out loud, because the alternative is a test that cannot
     * fail.** Seven hundred names need a second chunk of inodes, a block
     * directory, a leaf, a second data block under it and then a node only
     * if each boundary is actually crossed — and a run that crossed none of
     * them would print every step as passing while never touching the code
     * that does the growing.
     *
     * Printed **here** rather than at the end: the dirty-log step below
     * mounts the filesystem again, which resets the counters it would
     * otherwise be read after. The first version did read them after, and
     * reported five zeroes on a run that had done all five. */
    {
        uint64_t to_block = 0, to_leaf = 0, to_node = 0, data_blocks = 0;
        xfsmount_growth_counts(&to_block, &to_leaf, &to_node, &data_blocks);
        kprintf("fswrite: %llu inode chunk(s), %llu to block, %llu to leaf, %llu to node, "
                "%llu data block(s), %llu leaf split(s)\n",
                (unsigned long long)xfsmount_inode_chunks_added(),
                (unsigned long long)to_block, (unsigned long long)to_leaf,
                (unsigned long long)to_node, (unsigned long long)data_blocks,
                (unsigned long long)xfsmount_leaf_splits());
    }
    ok = ok && wrote_and_checked("a symlink round trips", symlink_round_trip());
    ok = ok && wrote_and_checked("a long target is refused", long_target_refused());
    ok = ok && wrote_and_checked("readlink refuses a file", readlink_refuses_a_file());
    ok = ok && wrote_and_checked("a dot component is refused", dots_are_refused());
    ok = ok && wrote_and_checked("a truncate frees its blocks", truncate_frees_blocks());
    ok = ok && wrote_and_checked("a truncate keeps the last block", truncate_rounds_up());
    ok = ok && wrote_and_checked("a grow makes a hole", truncate_grows_sparsely());
    ok = ok && wrote_and_checked("a create honours the umask", umask_is_consulted());
    ok = ok && wrote_and_checked("statfs reports real counters", statfs_is_real());
    ok = ok && wrote_and_checked("a mode is set and read back", chmod_round_trip());
    ok = ok && wrote_and_checked("chmod cannot change the file type", chmod_keeps_the_type());
    ok = ok && wrote_and_checked("an owner is set and read back", chown_round_trip());
    ok = ok && wrote_and_checked("a rename moves a name", rename_moves_a_name());
    ok = ok && wrote_and_checked("allocate past one group", fill_past_one_group());
    ok = ok && wrote_and_checked("one group alone must refuse", one_group_refuses());
    /* Last, because it leaves the filesystem read-only: anything after it
     * would be testing a mount that refuses writes by design. */
    /* Before the dirty-log steps, which leave the filesystem read-only in
     * the control's case and would refuse an attribute write. */
    ok = ok && wrote_and_checked("attributes round trip", attributes_round_trip());
    ok = ok && wrote_and_checked("a dirty log is replayed", dirty_then_remount());
    ok = ok && wrote_and_checked("refused replay stays read-only",
                                 refused_replay_is_read_only());
    kprintf("fswrite: %s\n", ok ? "every step passed" : "a step FAILED");
}

/* Reads a named file and says what it found: `minikern.cat=<path>`.
 *
 * **The instrument the filesystem work needs.** Listing a directory walks
 * its data blocks; *finding* a file by name is a different operation, and on
 * anything bigger than a single-block directory it goes through the hash
 * index that leaf and node formats add. Reading the file then exercises the
 * extent map, which is where a large or fragmented file moves into a b-tree.
 * So one knob covers lookup and extent reading, which are the two halves of
 * `ROADMAP.md` §3's open gaps.
 *
 * A checksum rather than the contents: the point is whether the right bytes
 * came back, and a 5 MB file on a serial console is not an answer anybody
 * reads. Addition is enough — this is detecting a reader that returns the
 * wrong blocks, not an adversary. */
static void cat_a_file(const char *path) {
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    uint64_t sum = 0;
    int rc = xfsmount_read_file(path, INIT_MAX_BYTES, &bytes, &size);
    if (rc != KERR_OK) {
        /* **The counters on the failure path too.** Which lookup path ran
         * is the first question a failure raises — the hash index and the
         * walk fail for different reasons — and printing them only on
         * success is how a failure becomes a mystery. */
        uint64_t hashed = 0, walked = 0;
        xfsmount_lookup_counts(&hashed, &walked);
        kprintf("cat: %s cannot be read (rc %d, %llu hashed, %llu walked)\n", path, rc,
                (unsigned long long)hashed, (unsigned long long)walked);
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        sum += bytes[i];
    }
    {
        uint64_t hashed = 0, walked = 0;
        xfsmount_lookup_counts(&hashed, &walked);
        kprintf("cat: %s is %u bytes, checksum %llu (%llu hashed, %llu walked)\n", path, size,
                (unsigned long long)sum, (unsigned long long)hashed,
                (unsigned long long)walked);
    }
    kfree(bytes);
}

/* Builds and runs a process from an ELF already in memory.
 *
 * The disk path and this one differ only in where the bytes come from, so
 * everything after the build is the same — and the reporting is the part
 * that matters, which is why it is repeated here rather than hidden behind
 * a flag on one function that then has to explain itself twice. */
static void run_program_from_memory(const uint8_t *image, uint32_t size, const char *name) {
    struct uproc *proc = uspace_alloc();
    int64_t status;
    int rc;
    if (proc == NULL) {
        kprintf("init: no free process slot\n");
        return;
    }
    rc = uspace_build_elf(proc, image, size, name);
    if (rc != KERR_OK) {
        kprintf("init: %s will not load (rc %d)\n", name, rc);
        uspace_release(proc);
        return;
    }
    status = uspace_run(proc);
    if (cmdline_has(boot_cmdline, "minikern.audit")) {
        uspace_audit_aliases(proc);
    }
    kprintf("init: %s exited with %lld after %llu syscalls (%llu stale-translation faults)\n",
            name, (long long)status, (unsigned long long)uspace_syscalls(),
            (unsigned long long)uspace_stale_faults());
    report_mapping_counters();
    report_hart_usage();
    uspace_destroy(proc);
}

static void run_init_from_disk(const char *path) {
    struct uproc *proc = uspace_alloc();
    uint8_t *image = NULL;
    uint32_t size = 0;
    int64_t status;
    int rc;
    if (proc == NULL) {
        kprintf("init: no free process slot\n");
        return;
    }
    rc = xfsmount_read_file(path, INIT_MAX_BYTES, &image, &size);
    if (rc != KERR_OK) {
        kprintf("init: cannot read %s (rc %d)\n", path, rc);
        uspace_release(proc);
        return;
    }
    kprintf("init: %s is %u bytes\n", path, size);
    rc = uspace_build_elf(proc, image, size, path);
    /* The image is copied into the process's pages, so it is not needed
     * once the address space is built. */
    kfree(image);
    if (rc != KERR_OK) {
        /* Each refusal says which one it was. "Headers refused" for
         * everything made a collision while building the address space look
         * like a bad executable, which sent the search to the wrong file. */
        kprintf("init: %s will not load (rc %d: %s)\n", path, rc,
                rc == KERR_NOEXEC  ? "not a static RISC-V 64 executable"
                : rc == KERR_INVAL ? "the headers contradict the file"
                : rc == KERR_EXIST ? "its address space collides with the kernel's mappings"
                : rc == KERR_NOMEM ? "out of memory"
                                   : "refused");
        uspace_release(proc);
        return;
    }
    status = uspace_run(proc);
    /* **The alias audit, on a real address space.** Asked here because a
     * clean answer from it is load-bearing for §0 — it is the reason "two
     * virtual addresses over one frame" is excluded — and an audit that
     * examines nothing gives exactly the same clean answer. Run at exit,
     * where the program's mappings are all still in place and at their most
     * numerous, it reports how many user pages it looked at, which is what
     * turns its answer into evidence. */
    if (cmdline_has(boot_cmdline, "minikern.audit")) {
        uspace_audit_aliases(proc);
    }
    kprintf("init: %s exited with %lld after %llu syscalls (%llu stale-translation faults)\n",
            path, (long long)status, (unsigned long long)uspace_syscalls(),
            (unsigned long long)uspace_stale_faults());
    /* Pages backed on first touch rather than at `mmap`. Zero would mean
     * demand paging is not happening whatever the code says — the one
     * number that tells "lazy" from "eager somewhere else". */
    report_mapping_counters();
    report_hart_usage();
    /* The stack's own counters after a program that used it. A reset or a
     * drop is the difference between "the program's logic is wrong" and
     * "the stack refused something", and they are indistinguishable from
     * the program's error message alone. */
    if (cmdline_has(boot_cmdline, "minikern.netstat")) {
        knet_report();
    }
    uspace_destroy(proc);
}

/* The watermark, and its control: a program that asks for nothing.
 *
 * **The pressure has to be brought to the program.** No program this kernel
 * can run will put 8 GiB of DRAM under real memory pressure, so the
 * watermark is set to the free-page count as it stands: the program's very
 * first allocation then drops free below it and the kernel starts paging
 * out. Reclaim frees frames, so free rises back above the mark and reclaim
 * stops — the equilibrium a real machine under pressure sits in, reached
 * from the other direction.
 *
 * The control is the same program with the watermark at **zero**, which is
 * what swappiness 0 means and the setting an operator actually checks for.
 * Without it, "pages went out" would not distinguish the policy working
 * from the policy ignoring its own switch. */
static void run_pressure_selftest(void) {
    uint64_t before, unasked, chances;
    uint32_t saved_swappiness;
    if (!swap_enabled()) {
        kprintf("swappress: swap is not enabled, so there is no policy to test\n");
        return;
    }
    /* The control first, while nothing has been paged out unasked yet.
     *
     * **Swappiness to zero, not the override to zero.** The override's zero
     * means "derive the watermark", so setting it expecting the policy to be
     * off gets the derived watermark — sixty thousand pages here — and a
     * control that passes because free happens to be two million. That is a
     * control proving nothing, which is worse than none: it would have
     * signed off a policy that ignored its own switch. */
    saved_swappiness = swap_swappiness();
    swap_set_swappiness(0);
    swap_set_watermark(0);
    before = uspace_pages_out_unasked();
    run_one_user_program("pressure", user_pressure_start, user_pressure_end);
    if (uspace_pages_out_unasked() != before) {
        kprintf("swappress: FAILED — pages went out with the watermark at zero\n");
        return;
    }
    kprintf("swappress: with swappiness at zero, nothing went out unasked\n");
    swap_set_swappiness(saved_swappiness);
    /* **A little above free, not at it.** The watermark is a deficit the
     * kernel works to close, so setting it exactly at the free count asks
     * for a page out on every fault for ever; a modest margin asks for a
     * bounded amount of paging and then stops, which is what a machine
     * under real pressure does. */
    {
        uint32_t free_now = mem_free_pages_count();
        kprintf("swappress: free is %u pages; setting the watermark to %u\n", free_now,
                free_now + 64u);
        swap_set_watermark(free_now + 64u);
    }
    kprintf("swappress: running the program again, under the watermark\n");
    uspace_trace_faults(10u);
    run_one_user_program("pressure", user_pressure_start, user_pressure_end);
    kprintf("swappress: the second run returned\n");
    swap_set_swappiness(0);
    swap_set_watermark(0);
    unasked = uspace_pages_out_unasked() - before;
    chances = uspace_second_chances();
    kprintf("swappress: %llu out unasked, %llu second chance(s), %llu A-bit fault(s)\n",
            (unsigned long long)unasked, (unsigned long long)chances,
            (unsigned long long)uspace_accessed_faults());
    /* Both numbers, because each alone can be satisfied by the wrong code:
     * pages going out says the watermark fired, and second chances say the
     * **clock** chose them rather than the old take-the-first-page policy,
     * which would report none. */
    kprintf("swappress: %s\n", unasked > 0 && chances > 0
                                    ? "the watermark and the clock both acted"
                                    : "FAILED");
}

static void run_user_programs(void) {
    /* **The address-space switch goes on before any program runs.**
     *
     * `minikern.noaslr` was applied *below* most of the self-tests — twenty
     * eight lines below `wait` — so every program before that point ran
     * with randomisation on however the operator asked. Making a
     * layout-sensitive failure reproducible is the only thing the option
     * exists for, and it could not do it for the tests that needed it most.
     *
     * That is the **fourth** silent no-op recorded in this tree: a textual
     * edit that matched nothing, twice; a hex value a decimal-only parser
     * read as zero; and now a switch applied after its subject. They share
     * a shape — the operator asked, the kernel agreed, and nothing
     * happened — and the only defence is to apply a switch where nothing
     * can run before it. */
    if (cmdline_has(boot_cmdline, "minikern.noaslr")) {
        uspace_set_aslr(false);
        kprintf("aslr: off, by request\n");
    }
    /* Set before anything runs, not between the programs: a trace that
     * starts half way through is worse than none, because the first half
     * looks quiet. */
    sched_set_trace(cmdline_has(boot_cmdline, "minikern.trace"));
    /* **On**, with `minikern.periodic` to put the old clock back. Keeping
     * the switch is worth one flag: it separates "this is broken" from
     * "this is broken without a tick nudging it", which is how both of the
     * timer bugs above were identified. */
    /* `minikern.isolate=<mask>` reserves harts, by index into the hart
     * table. Indices rather than ids because the ids are the firmware's and
     * start at 1 on this board, so a mask of ids would be off by one
     * against every per-hart array in the kernel. */
    sched_set_isolated(cmdline_number(boot_cmdline, "minikern.isolate", 0u));
    sched_set_dynticks(!cmdline_has(boot_cmdline, "minikern.periodic"));
    /* `minikern.calls=N` traces the first N calls. A fixed budget was
     * useless for anything that happens late — the budget was gone long
     * before a child process made its first call. */
    syscall_set_trace(cmdline_number(boot_cmdline, "minikern.calls", 0u));
    syscall_trace_sleeps(cmdline_has(boot_cmdline, "minikern.sleeps"));
    syscall_trace_errors(cmdline_has(boot_cmdline, "minikern.errors"));
    signal_set_delivery(!cmdline_has(boot_cmdline, "minikern.nosignals"));
    uspace_broadcast_additions(cmdline_has(boot_cmdline, "minikern.tlbsafe"));
    {
        size_t envlen = 0;
        const char *env = cmdline_value(boot_cmdline, "minikern.env", &envlen);
        uspace_set_extra_env(env, (uint32_t)envlen);
    }
    syscall_allow_tgkill(!cmdline_has(boot_cmdline, "minikern.nopreempt"));
    /* `minikern.keepadvice` keeps pages a program said it did not need —
     * the defect §0 came down to, kept so the test for it can fail. */
    syscall_keep_discarded_pages(cmdline_has(boot_cmdline, "minikern.keepadvice"));
    /* Puts `gettid`'s old wrong answer back, for measuring what it cost. */
    syscall_one_tid(cmdline_has(boot_cmdline, "minikern.onetid"));
    /* Keeps the kernel heap's pages out of the frame pool — a diagnostic for
     * §0's race, which leaks about 1.5 MiB. */
    mem_heap_keep_pages(cmdline_has(boot_cmdline, "minikern.heapkeep"));
    /* No frame is ever handed out twice: a free becomes a leak. */
    mem_never_reuse(cmdline_has(boot_cmdline, "minikern.neverreuse"));
    /* Every mapping backed at `mmap`, as it was before demand paging. */
    uspace_back_eagerly(cmdline_has(boot_cmdline, "minikern.eager"));
    /* Every thread stays on the hart it first ran on. */
    sched_pin_threads(cmdline_has(boot_cmdline, "minikern.nomigrate"));
    /* `minikern.cooperative` stops the tick taking a hart from a running
     * thread: threads still switch, but only where they chose to. */
    sched_set_preemption(!cmdline_has(boot_cmdline, "minikern.cooperative"));
    /* The checks that are too expensive to leave on. */
    uspace_set_paranoid(cmdline_has(boot_cmdline, "minikern.paranoid"));
    start_swap_if_asked();
    /* The controlling terminal, before anything can read it. */
    {
        static const struct console_input_port port = {.read = console_input_read,
                                                       .ready = console_input_ready};
        ktty_init(&port, 1u);
        (void)timer_add_housekeeping_hook(poll_tty_from_beat);
    }
    /* **One thread, one execve**: the comparison that separates "exec is
     * broken" from "exec after Go's multi-threaded fork is broken". It needs
     * a filesystem with /bin/qservice on it, which the QuokkaOS fixture
     * image has. */
    uspace_patch_entry_for_measurement(cmdline_has(boot_cmdline, "minikern.execpatch"));
    if (cmdline_has(boot_cmdline, "minikern.execone")) {
        run_one_user_program("execone", user_execone_start, user_execone_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.user")) {
        run_one_user_program("hello", user_program_start, user_program_end);
        run_one_user_program("faulter", user_faulter_start, user_faulter_end);
    }
    /* Phase 1's measurement: one busy thread, no syscalls, and whatever
     * timer interrupts the model takes for it. Embedded because the board's
     * only other way to run a program is the operator's SD card. */
    if (cmdline_has(boot_cmdline, "minikern.spin")) {
        uint64_t started = timer_ns();
        run_one_user_program("spin", user_spin_start, user_spin_end);
        kprintf("spin: %llu ms of one busy thread\n",
                (unsigned long long)((timer_ns() - started) / 1000000u));
        report_hart_usage();
    }
    /* Phase 2's measurement: two threads, nice +5 against nice -5, dividing
     * one hart. Run it with `minikern.smp=off` — with four harts the two
     * threads simply run and no weighting is observable, which is a property
     * of proportional share rather than a limitation of this test. */
    if (cmdline_has(boot_cmdline, "minikern.nice")) {
        run_one_user_program("nice", user_nice_start, user_nice_end);
        report_hart_usage();
    }
    /* Phase 4's measurement: a thread pinned to hart index 2. The per-hart
     * report below is the whole test — that hart does the switches and the
     * others do none. */
    if (cmdline_has(boot_cmdline, "minikern.pin")) {
        run_one_user_program("pin", user_pin_start, user_pin_end);
        report_hart_usage();
    }
    /* Phase 5's measurement: a SCHED_FIFO thread against a fair one on one
     * hart. Run with `minikern.smp=off` for the same reason as the nice
     * test — a class only shows under contention. */
    if (cmdline_has(boot_cmdline, "minikern.rt")) {
        run_one_user_program("rt", user_rt_start, user_rt_end);
        report_hart_usage();
    }
    /* Rule 25's syscall-surface suite. The exit status is a bitmask of the
     * hostile arguments the kernel **accepted**, so 0 is a pass and any
     * other value names the missing check. */
    if (cmdline_has(boot_cmdline, "minikern.hostile")) {
        run_one_user_program("hostile", user_hostile_start, user_hostile_end);
    }
    /* Roadmap §1: fork, then wait for the child. The exit status is the
     * answer — 7 means the whole path works, 101 or 102 name where it
     * stops, and a run that never finishes means the parent is blocked. */
    if (cmdline_has(boot_cmdline, "minikern.wait")) {
        run_one_user_program("wait", user_wait_start, user_wait_end);
    }
    /* Roadmap §1: process groups, and a job-control signal sent to a group
     * rather than to a process. 7 means every step worked; 111..115 name
     * which one did not. */
    if (cmdline_has(boot_cmdline, "minikern.pgrp")) {
        run_one_user_program("pgrp", user_pgrp_start, user_pgrp_end);
    }
    /* Roadmap §1: the controlling terminal. Asks its size, reads a line,
     * writes it back. 0 means the whole path worked; 121..123 name where it
     * stopped. */
    if (cmdline_has(boot_cmdline, "minikern.term")) {
        run_one_user_program("term", user_term_start, user_term_end);
    }
    /* Roadmap §1: the identity model. 0 means privilege dropped and stayed
     * dropped; 131..134 name the step that failed. */
    if (cmdline_has(boot_cmdline, "minikern.uid")) {
        run_one_user_program("uid", user_uid_start, user_uid_end);
    }
    /* Roadmap §1: a resource limit that bites. 0 means it was read
     * honestly, lowered, enforced at the fifth descriptor, and could not be
     * raised again after dropping privilege. */
    /* Capabilities and `seccomp`: 0 means every step held, 151..160 name
     * the one that did not. */
    /* **Address randomisation is on unless this says otherwise.** A fixed
     * layout is what makes a crash address mean the same thing in two runs,
     * which is the first thing anybody debugging a fault wants. */
    if (cmdline_has(boot_cmdline, "minikern.privsep")) {
        run_one_user_program("privsep", user_privsep_start, user_privsep_end);
    }
    /* Prints its stack, heap and mapping addresses. One run proves nothing;
     * the harness runs it twice and compares. */
    if (cmdline_has(boot_cmdline, "minikern.aslr")) {
        run_one_user_program("aslr", user_aslr_start, user_aslr_end);
    }
    /* Reads `/dev/virtual/kernel_logger` as a program would — from a
     * process, because a descriptor needs one. The kernel-side version of
     * this check was refused with `EBADF`, correctly: there is no table to
     * install into at boot. */
    if (cmdline_has(boot_cmdline, "minikern.klogdev")) {
        run_one_user_program("klogdev", user_klogdev_start, user_klogdev_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.rlimit")) {
        run_one_user_program("rlimit", user_rlimit_start, user_rlimit_end);
    }
    /* Roadmap §2: `mremap`, and the record `munmap` used to keep. The exit
     * status is a bitmask of six checks, so **63** is a pass and a smaller
     * number names the half that works. The `mremap` line below says how
     * many pages were re-pointed rather than copied. */
    if (cmdline_has(boot_cmdline, "minikern.mremap")) {
        run_one_user_program("mremap", user_mremap_start, user_mremap_end);
        report_mapping_counters();
    }
    /* Roadmap §2: the `mmap` flags. Seven checks in a bitmask, so **127** is
     * a pass. The populated count below is the half the program cannot see:
     * `MAP_POPULATE`'s pages are backed at `mmap` and so never appear in the
     * demand count. */
    if (cmdline_has(boot_cmdline, "minikern.mapflags")) {
        run_one_user_program("mapflags", user_mapflags_start, user_mapflags_end);
        report_mapping_counters();
    }
    /* Roadmap §0: a process ending while one of its threads is mid-flight
     * on another hart. **5** means the whole sequence worked; the kernel's
     * own "waited for" line says whether it had to catch anything, which is
     * what makes the closing of the window a measurement. */
    /* Roadmap §2: huge pages. **127** is a pass, and the count of 2 MiB
     * entries below is the half the program cannot see. */
    if (cmdline_has(boot_cmdline, "minikern.huge")) {
        run_one_user_program("huge", user_huge_start, user_huge_end);
        report_mapping_counters();
    }
    /* Roadmap §2: a mapping backed by a file. **63** is a pass, and the
     * count of pages filled from a file is the half the program cannot see
     * — a file mapping is otherwise indistinguishable from an anonymous one
     * somebody wrote the same bytes into. Needs a filesystem with `/init`
     * on it, so: `minikern.disk=sd`. */
    if (cmdline_has(boot_cmdline, "minikern.filemap")) {
        run_one_user_program("filemap", user_filemap_start, user_filemap_end);
        report_mapping_counters();
    }
    /* Roadmap §1: `^Z` from the keyboard to a job. **7** is a pass, and it
     * needs the harness to type `^Z` when the program says it is ready —
     * `runtype.py` does that; a plain boot leaves this program waiting and
     * then reporting 174, which is the honest answer to a keystroke that
     * never came. */
    if (cmdline_has(boot_cmdline, "minikern.tstp")) {
        run_one_user_program("tstp", user_tstp_start, user_tstp_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.endsib")) {
        run_one_user_program("endsib", user_endsib_start, user_endsib_end);
        report_await();
    }
    if (cmdline_has(boot_cmdline, "minikern.swaptest")) {
        swap_selftest();
    }
    /* Roadmap §2: a program's pages out to swap and back, through a fault.
     * **0** is a pass, and the swap counters below are what prove the pages
     * went anywhere — the program cannot tell, which is the promise. */
    if (cmdline_has(boot_cmdline, "minikern.swapout")) {
        run_one_user_program("swap", user_swap_start, user_swap_end);
        report_swap();
    }
    /* Roadmap §2: the watermark — pages out because memory is short, with
     * the control that proves the policy and not merely the mechanism. */
    if (cmdline_has(boot_cmdline, "minikern.swappress")) {
        run_pressure_selftest();
    }
    /* Roadmap §0: every register a preempted thread is entitled to keep.
     * **0** is a pass; any other value is the register that came back
     * wrong. Several copies at once, because one thread alone is never
     * preempted in favour of anything interesting. */
    /* Roadmap §0: the guarantee the corruption came from breaking.
     * **0** is a pass; 190..194 name the step. */
    /* Roadmap §0: two mappings must never be the same memory. **0** is a
     * pass; 100 + the slot names the region whose stamp went wrong. */
    if (cmdline_has(boot_cmdline, "minikern.mapchurn")) {
        run_one_user_program("mapchurn", user_mapchurn_start, user_mapchurn_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.dontneed")) {
        run_one_user_program("dontneed", user_dontneed_start, user_dontneed_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.regs")) {
        run_one_user_program("regs", user_regs_start, user_regs_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.init")) {
        run_init_from_disk("/init");
    }
    /* Roadmap §3: the write path, on a filesystem in memory. */
    if (cmdline_has(boot_cmdline, "minikern.fswrite")) {
        run_write_selftest();
    }
    if (cmdline_has(boot_cmdline, "minikern.chunked")) {
        run_chunked_write_probe();
    }
    /* Automount's decision: a labelled volume where its label says. */
    if (cmdline_has(boot_cmdline, "minikern.volumes")) {
        run_volumes_selftest();
    }
    /* `minikern.cat=<path>` — lookup and extent reading, in one answer. */
    {
        size_t len = 0;
        const char *spec = cmdline_value(boot_cmdline, "minikern.cat", &len);
        char path[128];
        size_t n = 0;
        while (spec != NULL && n + 1u < sizeof path && n < len) {
            path[n] = spec[n];
            n++;
        }
        if (spec != NULL && n > 0u) {
            path[n] = '\0';
            cat_a_file(path);
        }
    }
    /* **A program the boot loader left in memory**, which is how a test
     * program reaches the board without the card being rewritten. The ELF
     * is read straight out of DRAM: `uspace_build_elf` wants a buffer, and
     * the filesystem path differs only in how it obtains one. */
    if (loaded_bytes != 0 && cmdline_has(boot_cmdline, "minikern.runloaded")) {
        run_program_from_memory((const uint8_t *)(uintptr_t)loaded_at,
                                (uint32_t)loaded_bytes, "loaded");
    }
    /* **Any program on the card, named on the command line.**
     *
     * `/init` was the only thing that could be run from the filesystem, and
     * that made the card a fixed surface: a new test program meant a new
     * card, which only the operator can produce. The hunt in §0 needs its
     * reproducer on the **board** rather than under the emulator, and a
     * path is all that stands between the two. */
    {
        size_t len = 0;
        const char *spec = cmdline_value(boot_cmdline, "minikern.run", &len);
        char path[64];
        size_t n = 0;
        while (spec != NULL && n + 1u < sizeof path && n < len) {
            path[n] = spec[n];
            n++;
        }
        if (spec != NULL && n > 0u) {
            path[n] = '\0';
            run_init_from_disk(path);
        }
    }
}

/* Swap, if the operator asked for it: `minikern.swap=<path>[:<swappiness>]`.
 *
 * **Off unless asked**, which is the operator's decision and the right
 * default: a kernel that starts paging without being told is one whose
 * memory behaviour nobody chose. The path is a file that already exists and
 * is fully allocated — `dd if=/dev/zero of=/swapfile bs=1M count=64` — and
 * the second field is how willing the kernel is to use it, 0 to 100.
 *
 * It is enabled here, after the filesystem is mounted and before any program
 * runs, because resolving the file's blocks needs the one and the other must
 * not be able to run without swap if swap was asked for. */
static void start_swap_if_asked(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.swap", &len);
    char path[64];
    size_t n = 0;
    uint32_t swappiness = 60u; /* Linux's default, and a sane middle */
    int rc;
    if (spec == NULL || len == 0) {
        return;
    }
    while (n + 1u < sizeof path && n < len && spec[n] != ':') {
        path[n] = spec[n];
        n++;
    }
    path[n] = '\0';
    if (n < len && spec[n] == ':') {
        swappiness = (uint32_t)cmdline_number_in(spec + n + 1u, len - n - 1u, 60u);
    }
    rc = swap_enable(path, swappiness);
    if (rc != KERR_OK) {
        /* Said out loud, because a kernel that was asked for swap and
         * quietly has none will later be blamed for the memory pressure it
         * could not relieve. */
        kprintf("swap: cannot use %s (rc %d) — running without swap\n", path, rc);
    }
}

/* Does a page survive a round trip through the swap file?
 *
 * The one thing worth proving before anything is paged out for real: a
 * frame written to a slot and read back into a different frame must come
 * back byte for byte. It uses two frames rather than one on purpose —
 * reading a page back into the frame it came from would pass even if the
 * read did nothing at all.
 *
 * The pattern is derived from the offset, so a read that lands on the wrong
 * slot or at the wrong offset within one is caught rather than looking
 * right. */
static void swap_selftest(void) {
    phys_t out = 0, back = 0;
    uint32_t slot = 0;
    int rc;
    if (!swap_enabled()) {
        kprintf("swap: not enabled, so there is nothing to test\n");
        return;
    }
    if (mem_alloc_pages(1, 1, &out) != KERR_OK || mem_alloc_pages(1, 1, &back) != KERR_OK) {
        kprintf("swap: no memory for the test\n");
        return;
    }
    for (uint32_t i = 0; i < 4096u; i++) {
        ((volatile uint8_t *)(uintptr_t)out)[i] = (uint8_t)(i * 7u + 1u);
        ((volatile uint8_t *)(uintptr_t)back)[i] = 0xff;
    }
    rc = swap_out(out, &slot);
    if (rc != KERR_OK) {
        kprintf("swap: could not write a page out (rc %d)\n", rc);
        return;
    }
    rc = swap_in(slot, back);
    if (rc != KERR_OK) {
        kprintf("swap: could not read slot %u back (rc %d)\n", slot, rc);
        return;
    }
    for (uint32_t i = 0; i < 4096u; i++) {
        if (((volatile uint8_t *)(uintptr_t)back)[i] != (uint8_t)(i * 7u + 1u)) {
            kprintf("swap: **slot %u came back wrong at byte %u**\n", slot, i);
            return;
        }
    }
    kprintf("swap: a page went out to slot %u and came back byte for byte\n", slot);
    mem_free_pages(out, 1);
    mem_free_pages(back, 1);
}

/* What swap has actually done. Printed after a program that used it,
 * because the program cannot see any of this and that is the point: the
 * only evidence that a page went out and came back is the kernel's. */
static void report_swap(void) {
    uint32_t slots = 0, used = 0;
    uint64_t written = 0, read_back = 0, in = 0, out = 0;
    swap_report(&slots, &used, &written, &read_back);
    uspace_swap_report(&in, &out);
    kprintf("swap: %u of %u slots in use; %llu pages written, %llu read back\n", used, slots,
            (unsigned long long)written, (unsigned long long)read_back);
    kprintf("swap: %llu pages sent out by a process, %llu faulted back in\n",
            (unsigned long long)out, (unsigned long long)in);
}

/* True when the operator asked the machine to switch itself off at the end,
 * which is what makes a boot test a test rather than a timeout. */
static bool halt_requested(void) {
    return cmdline_equals(boot_cmdline, "minikern.halt", "poweroff");
}

static void kmain_after_devices(uint64_t hart, uint64_t dtb);
static void enter_the_kernels_own_half(uint64_t hart, uint64_t dtb);

/* The boot, from the point where the kernel is running at its own address.
 *
 * **Everything registered before the jump is registered again here**,
 * because each of those registrations stored a *low* address: the console's
 * function pointers, the kernel lock's two entry points, and this hart's
 * trap vector. They still work while the identity map is there — which is
 * why the jump can be verified before that map is dropped — but a missed
 * one is a fault the moment it goes. Re-registering is cheaper than keeping
 * a list of pointers to relocate, and cannot go stale. */
static void kmain_high(uint64_t hart, uint64_t dtb) {
    struct console_port con = jh7110_sbi_console();
    struct bootinfo info = {0};
    console_attach(&con);
    trap_set_lock(klock_enter, klock_exit);
    /* The tag port, beside the lock itself: a port installed late is a port
     * that was not installed, and this file has paid for that twice. */
    trap_set_lock_tag(klock_tag);
    trap_init_cpu();
    kprintf("mmu: the kernel runs at %llx, its own half\n",
            (unsigned long long)(uintptr_t)&kmain_high);
    /* Read again rather than carried over: the old struct lived on the old
     * stack, and a pointer to it is exactly the kind of low address this
     * function exists to stop using. */
    read_bootinfo(dtb, &info);
    adopt_the_real_uart(dtb);
    bring_up_memory(&info);
    bring_up_interrupts(dtb, hart);
    bring_up_clocks(dtb);
    bring_up_storage(dtb);
    /* **After the interrupt controller and the storage probe, both.** The
     * root filesystem is on the card, so the card is read before the PLIC
     * exists; arming its interrupt is therefore a second step rather than
     * part of the probe. The scheduler's sleep goes in here too — before it
     * does, a wait releases the lock and `wfi`s, which is all the boot path
     * can do and all it needs. */
    irqwait_set_sleeper(sched_wait_on, sched_wake_on);
    /* **`minikern.mmcpoll` keeps it polled**, so that the same build can be
     * measured both ways. Without it the only available comparison is
     * against a different binary, and "it is no slower than something else
     * I built" is not a measurement. */
    /* **Opt-in, for now.** Reads through the interrupt are proven — twenty
     * dispatched, nineteen waits woken, none timed out — but a *write*
     * through the same path does not complete, and a disk that cannot be
     * written is worse than one that is read by polling. So the default
     * stays polled and `minikern.mmcirq` turns it on, which keeps the
     * finished part usable and the unfinished part visible.
     *
     * The primitive itself and the `sie` fix below it are not opt-in: they
     * are what made any of this reachable. */
    if (cmdline_has(boot_cmdline, "minikern.mmcirq")) {
        dwmmc_arm_only(cmdline_has(boot_cmdline, "minikern.mmcirqonly"));
        (void)dwmmc_use_interrupts_if_present(dtb);
    }
    /* **The drive has its own switch**, because it is its own device: it
     * reads its interrupt from the PCIe host bridge rather than from its own
     * node, it is the attached disk on this board where the card is not, and
     * the card's write path through the interrupt is the unfinished part
     * above — none of which is a reason to leave the drive polling. */
    if (cmdline_has(boot_cmdline, "minikern.nvmeirqmeasure")) {
        struct fdt fdt;
        if (fdt_open(&fdt, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) == KERR_OK) {
            nvme_measure_intx(&fdt);
        }
    }
    if (cmdline_has(boot_cmdline, "minikern.nvmeirq") ||
        cmdline_has(boot_cmdline, "minikern.nvmeirqprobe")) {
        nvme_irq_probe_only(cmdline_has(boot_cmdline, "minikern.nvmeirqprobe"));
        (void)nvme_use_interrupts_if_present(dtb);
    }
    /* **The umask reader, before anything can create a file.** The
     * filesystem must not learn what a process is — `uspace` depends on it
     * for `execve` — so the composition root hands it a function. Without
     * this the field would be a setting nothing consults, which is the
     * failure this tree has recorded seven times. */
    xfsmount_set_umask_source(uspace_current_umask);
    bring_up_filesystem();
    {
        /* **Did the device's interrupt ever reach the kernel?** The one
         * question that separates "the wait is wrong" from "the source,
         * the mask or the enable is wrong", and it cannot be answered from
         * outside. */
        uint64_t dispatched = 0, unclaimed = 0, woken = 0, timed_out = 0;
        dispatched = irq_count();
        unclaimed = irq_spurious();
        irqwait_counts(&woken, &timed_out);
        klog(KLOG_DEBUG, "irq: %llu dispatched, %llu unclaimed; waits %llu woken, %llu timed out\n",
             (unsigned long long)dispatched, (unsigned long long)unclaimed,
             (unsigned long long)woken, (unsigned long long)timed_out);
    }
    put_the_boot_log_on_disk();
    /* **Rotation needs two boots to show itself**, and a RAM disk is
     * re-staged on every one — so there is no way to watch it except to
     * run the real path again. Three times, because the interesting case is
     * not "0 became 1" but a chain of moves not treading on each other. */
    if (cmdline_has(boot_cmdline, "minikern.klogrotate")) {
        put_the_boot_log_on_disk();
        put_the_boot_log_on_disk();
    }
    run_disk_benchmark();
    bring_up_gpio(dtb);
    if (cmdline_has(boot_cmdline, "minikern.gmac")) {
        probe_the_ethernet(dtb);
    }
    if (cmdline_has(boot_cmdline, "minikern.gmacup")) {
        bring_up_the_ethernet(dtb);
    }
    kmain_after_devices(hart, dtb);
}

/* Jumps into `kmain_high` at its high address, on this hart's boot stack at
 * its high alias. Does not return. */
static void enter_the_kernels_own_half(uint64_t hart, uint64_t dtb) {
    uint64_t shift = mmu_high_offset();
    void *fn = (void *)((uint64_t)(uintptr_t)&kmain_high + shift);
    uint64_t sp;
    if (!mmu_enabled()) {
        /* Translation never came up, so there is no high mapping to jump
         * to and the boot carries on where it is. Saying so, because a
         * kernel that silently stayed low would look identical. */
        kprintf("mmu: translation is off, so the kernel stays at its load address\n");
        kmain_high(hart, dtb);
        return;
    }
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    mmu_enter_high(fn, hart, dtb, sp + shift);
}

void kmain(uint64_t hart, uint64_t dtb) {
    /* The hart id into `tp`, first thing: every per-hart array is indexed
     * through it, and the kernel lock asks who holds it. The firmware passed
     * it in `a0` because there is no register a supervisor can ask. */
    smp_set_this_hart(hart);
    /* Floating point, before anything might need to save it: `sstatus.FS`
     * gates those instructions in the kernel too, so a kernel that saves a
     * thread's floating-point state has to enable it for itself first. */
    fp_enable();
    /* The kernel lock, before anything can trap. Wiring it up later — after
     * a program had already run — left every syscall taking the "no lock
     * configured" path while the scheduler still released one, and the
     * ticket lock's counters drifted apart until the next hart waited for a
     * ticket that had been served. A port that is installed late is a port
     * that was not installed. */
    trap_set_lock(klock_enter, klock_exit);
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
    /* **The log's clock, as soon as the timer can answer.** Every line
     * before this reads `[00:00:00.000]`, which is the truth about them:
     * they happened before the kernel could tell how long it had been
     * alive. */
    klog_time_source(timer_ns);
    bring_up_translation(&info);
    /* **From here on, at the kernel's own address.** `mmu_init` mapped the
     * image both where it is linked and where it was loaded, so the jump is
     * safe and the identity half is still there for anything that has not
     * been re-registered yet — which is what lets this be verified on its
     * own, before that half is taken away.
     *
     * It does not return: the rest of the boot runs on the other side. */
    /* **Does not return**: the rest of the boot is `kmain_high`, running at
     * the kernel's own address. Nothing may be added below this line. */
    enter_the_kernels_own_half(hart, dtb);
}

/* The boot from the point where the devices are up — shared, because
 * `kmain_high` is where it is reached from and `kmain` only gets here when
 * translation never came up at all. */
static void kmain_after_devices(uint64_t hart, uint64_t dtb) {
    bring_up_other_harts(dtb, hart);
    /* **After the harts are up**, because half of what it measures is about
     * them: how many are schedulable, and what an interrupt between two of
     * them costs. */
    if (cmdline_has(boot_cmdline, "minikern.buses")) {
        report_the_buses(dtb);
    }
    if (cmdline_has(boot_cmdline, "minikern.busprobe")) {
        bring_up_the_buses(dtb);
    }
    if (cmdline_has(boot_cmdline, "minikern.usb")) {
        bring_up_usb();
    }
    /* **On by default**, unlike the probes above: the operator fitted a
     * radiator and a fan to keep iterations fast and wants to see what the
     * die is doing, so a temperature nobody asked for is still the answer to
     * the next question. It costs one register read per `/proc` open. */
    {
        /* **The epoch, before anything timestamps anything.** The board has
         * no real-time clock; the harness passes this workstation's time on
         * the command line and the monotonic counter carries it. A boot
         * without it says so, because a wrong date that looks right is how a
         * quota's grace period expires fifty years early. */
        wallclock_set_epoch(cmdline_number(boot_cmdline, "minikern.time", 0u));
        if (wallclock_is_real()) {
            kprintf("time: the epoch is %llu, from the command line\n",
                    (unsigned long long)wallclock_seconds());
        } else {
            kprintf("time: **no epoch given**; timestamps are seconds since boot\n");
        }
    }
    bring_up_thermal(dtb);
    if (cmdline_has(boot_cmdline, "minikern.probe")) {
        struct fdt probe_tree;
        if (fdt_open(&probe_tree, (const void *)(uintptr_t)dtb, FDT_MAX_BYTES) == KERR_OK) {
            probe_report(&probe_tree,
                         (uint32_t)cmdline_number(boot_cmdline, "minikern.cpudiv", 0u));
        }
    }
    bring_up_network(dtb);
    wait_for_ticks();
    run_user_programs();
    (void)selftest_run(boot_cmdline);
    /* **After everything, because that is when the range means something.**
     *
     * The first attempt put this in `report_detectors`, which runs fourteen
     * hundred lines before `bring_up_thermal` binds the sensor — so it
     * reported nothing, correctly, and looked like a broken sampler. The
     * boot's last line is the one place every reading has already been
     * taken. */
    report_the_thermal_range();
    kprintf("minikern: boot complete\n");
    if (halt_requested()) {
        kprintf("minikern: powering off\n");
        sbi_shutdown();
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
