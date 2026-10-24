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
    uint64_t arena = ((uint64_t)(uintptr_t)__kernel_end + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
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
    if (volumes_automount() == 0) {
        return;
    }
    list_the_volumes();
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
    want = smp_start_others((phys_t)(uintptr_t)_secondary_start, satp);
    /* Bounded, because a hart the firmware accepted and that then never
     * arrives must cost one core rather than the whole boot. */
    for (waited = 0; harts_arrived < want && waited < 50000000u; waited++) {
    }
    kprintf("smp: %u of %u started harts reached the kernel, %u running in total\n",
            harts_arrived, want, smp_running_count());
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

static void poll_net_from_tick(void) {
    virtio_net_poll(knet_interface());
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
    rc = virtio_net_init(&fdt, &port, mac);
    if (rc != KERR_OK) {
        kprintf("net: no network device in the tree (rc %d)\n", rc);
        return;
    }
    (void)knet_init(mac, port);
    {
        static const struct irq_handler handler = {.handle = on_net_interrupt, .self = NULL};
        (void)irq_register(virtio_net_irq(), &handler);
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
extern const char user_spin_start[], user_spin_end[];
extern const char user_nice_start[], user_nice_end[];
extern const char user_pin_start[], user_pin_end[];
extern const char user_rt_start[], user_rt_end[];
extern const char user_hostile_start[], user_hostile_end[];
extern const char user_wait_start[], user_wait_end[];
extern const char user_pgrp_start[], user_pgrp_end[];
extern const char user_term_start[], user_term_end[];
extern const char user_uid_start[], user_uid_end[];
extern const char user_rlimit_start[], user_rlimit_end[];
extern const char user_mremap_start[], user_mremap_end[];
extern const char user_mapflags_start[], user_mapflags_end[];
extern const char user_endsib_start[], user_endsib_end[];
extern const char user_huge_start[], user_huge_end[];
extern const char user_filemap_start[], user_filemap_end[];
extern const char user_tstp_start[], user_tstp_end[];
extern const char user_swap_start[], user_swap_end[];
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
#define INIT_MAX_BYTES (4u * 1024u * 1024u)

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
    if (size != len) {
        kfree(bytes);
        return KERR_IO;
    }
    for (uint32_t i = 0; i < len; i++) {
        if (bytes[i] != (uint8_t)want[i]) {
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
            rc = got[i] == chunk[i] ? rc : KERR_IO;
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
 * Three things are then asserted, and the second is the one that matters.
 * The filesystem **mounts** — a power cut must not make a disk unreadable.
 * It is **read-only** — its metadata is behind a log saying something newer
 * happened, so a write would build on a superseded state. And a **read
 * still works**, which is what the whole arrangement is for. */
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
    if (xfsmount_writable()) {
        return KERR_IO; /* readable is right; writable is the bug */
    }
    return read_back("d1", text, sizeof text - 1u);
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
    ok = ok && wrote_and_checked("allocate past one group", fill_past_one_group());
    ok = ok && wrote_and_checked("one group alone must refuse", one_group_refuses());
    /* Last, because it leaves the filesystem read-only: anything after it
     * would be testing a mount that refuses writes by design. */
    ok = ok && wrote_and_checked("a dirty log mounts read-only", dirty_then_remount());
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
        kprintf("cat: %s cannot be read (rc %d)\n", path, rc);
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

static void run_user_programs(void) {
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
    bring_up_translation(&info);
    adopt_the_real_uart(dtb);
    bring_up_memory(&info);
    bring_up_interrupts(dtb, hart);
    bring_up_clocks(dtb);
    bring_up_storage(dtb);
    bring_up_filesystem();
    run_disk_benchmark();
    bring_up_gpio(dtb);
    bring_up_other_harts(dtb, hart);
    /* **After the harts are up**, because half of what it measures is about
     * them: how many are schedulable, and what an interrupt between two of
     * them costs. */
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
    kprintf("minikern: boot complete\n");
    if (halt_requested()) {
        kprintf("minikern: powering off\n");
        sbi_shutdown();
    }
    for (;;) {
        __asm__ volatile("wfi");
    }
}
