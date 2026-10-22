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
#include "drivers/virtio_net.h"
#include "kernel/klock.h"
#include "kernel/net.h"
#include "kernel/random.h"
#include "kernel/sched.h"
#include "kernel/signal.h"
#include "kernel/syscall.h"
#include "kernel/uspace.h"
#include "kernel/xfsmount.h"
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

static void drive_a_pin(void);

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
    (void)timer_add_tick_hook(poll_net_from_tick);
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
static void drive_a_pin(void) {
    size_t len = 0;
    const char *spec = cmdline_value(boot_cmdline, "minikern.gpio", &len);
    uint32_t controller = 0, pin = 0, at = 0;
    bool high = false;
    int rc;
    if (spec == NULL) {
        return;
    }
    while (at < len && spec[at] >= '0' && spec[at] <= '9') {
        controller = controller * 10u + (uint32_t)(spec[at++] - '0');
    }
    at += at < len && spec[at] == ':' ? 1u : 0u;
    while (at < len && spec[at] >= '0' && spec[at] <= '9') {
        pin = pin * 10u + (uint32_t)(spec[at++] - '0');
    }
    kprintf("gpio: driving controller %u pin %u\n", controller, pin);
    for (uint32_t step = 0; step < 2u; step++) {
        bool want = step == 0;
        rc = gpio_jh7110_set_output(controller, pin, want);
        if (rc != KERR_OK) {
            kprintf("gpio: could not drive it %s (rc %d)\n", want ? "high" : "low", rc);
            return;
        }
        rc = gpio_jh7110_read(controller, pin, &high);
        if (rc != KERR_OK) {
            kprintf("gpio: could not read it back (rc %d)\n", rc);
            return;
        }
        kprintf("gpio: drove %s, reads back %s%s\n", want ? "high" : "low", high ? "high" : "low",
                high == want ? "" : " — which does not match, so the write did not reach the pin");
    }
    /* Left as an input afterwards: a pin this kernel was asked to poke at
     * once should not stay driven for the rest of the machine's life. */
    (void)gpio_jh7110_set_input(controller, pin);
    kprintf("gpio: left pin %u as an input\n", pin);
}

/* Runs the user-mode programs, if asked.
 *
 * `minikern.user=1` is the proof that user mode works: a program of its own
 * in an address space of its own, making syscalls, and a second one that
 * faults on purpose so that a **user** fault can be seen to end the process
 * rather than the kernel. */
extern const char user_program_start[], user_program_end[];
extern const char user_faulter_start[], user_faulter_end[];

static void run_one_user_program(const char *name, const char *from, const char *to) {
    struct uproc proc;
    int64_t status;
    int rc = uspace_build(&proc, from, (uint32_t)(to - from));
    if (rc != KERR_OK) {
        kprintf("uspace: could not build %s (rc %d)\n", name, rc);
        return;
    }
    status = uspace_run(&proc);
    kprintf("uspace: %s exited with %lld after %llu syscalls\n", name, (long long)status,
            (unsigned long long)uspace_syscalls());
    uspace_destroy(&proc);
}

/* What each hart actually did.
 *
 * The measurement that distinguishes "four harts are available" from "four
 * harts were used". A hart that never switched to a thread never ran one —
 * and on the architecture this migrated from, the equivalent check was
 * satisfiable by a core that had never left its idle loop, which is why
 * this counts switches rather than asking whether a hart is up. */
static void report_hart_usage(void) {
    {
        uint64_t sleeps = 0, average = 0, worst = 0;
        sched_sleep_report(&sleeps, &average, &worst);
        kprintf("signal: %llu delivered, %llu frames overwritten\n",
                (unsigned long long)signal_delivered(),
                (unsigned long long)signal_clobbered_frames());
        kprintf("sched: %llu sleeps, woken %llu us late on average, %llu us at worst\n",
                (unsigned long long)sleeps, (unsigned long long)average,
                (unsigned long long)worst);
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

static void run_init_from_disk(const char *path) {
    struct uproc proc;
    uint8_t *image = NULL;
    uint32_t size = 0;
    int64_t status;
    int rc = xfsmount_read_file(path, INIT_MAX_BYTES, &image, &size);
    if (rc != KERR_OK) {
        kprintf("init: cannot read %s (rc %d)\n", path, rc);
        return;
    }
    kprintf("init: %s is %u bytes\n", path, size);
    rc = uspace_build_elf(&proc, image, size, path);
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
        return;
    }
    status = uspace_run(&proc);
    kprintf("init: %s exited with %lld after %llu syscalls (%llu stale-translation faults)\n",
            path, (long long)status, (unsigned long long)uspace_syscalls(),
            (unsigned long long)uspace_stale_faults());
    report_hart_usage();
    /* The stack's own counters after a program that used it. A reset or a
     * drop is the difference between "the program's logic is wrong" and
     * "the stack refused something", and they are indistinguishable from
     * the program's error message alone. */
    if (cmdline_has(boot_cmdline, "minikern.netstat")) {
        knet_report();
    }
    uspace_destroy(&proc);
}

static void run_user_programs(void) {
    /* Set before anything runs, not between the programs: a trace that
     * starts half way through is worse than none, because the first half
     * looks quiet. */
    sched_set_trace(cmdline_has(boot_cmdline, "minikern.trace"));
    syscall_set_trace(cmdline_has(boot_cmdline, "minikern.calls") ? 400u : 0u);
    syscall_trace_sleeps(cmdline_has(boot_cmdline, "minikern.sleeps"));
    signal_set_delivery(!cmdline_has(boot_cmdline, "minikern.nosignals"));
    syscall_allow_tgkill(!cmdline_has(boot_cmdline, "minikern.nopreempt"));
    if (cmdline_has(boot_cmdline, "minikern.user")) {
        run_one_user_program("hello", user_program_start, user_program_end);
        run_one_user_program("faulter", user_faulter_start, user_faulter_end);
    }
    if (cmdline_has(boot_cmdline, "minikern.init")) {
        run_init_from_disk("/init");
    }
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
    bring_up_storage(dtb);
    bring_up_filesystem();
    bring_up_gpio(dtb);
    bring_up_other_harts(dtb, hart);
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
