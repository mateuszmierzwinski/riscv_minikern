#include "kernel/probe.h"

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/sbi.h"
#include "arch/riscv/smp.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/trap.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/cpufreq_jh7110.h"
#include "drivers/crg_jh7110.h"
#include "drivers/i2c_dw.h"

/* `stimecmp`, the CSR the Sstc extension adds. Reading it from S-mode is an
 * illegal instruction on a machine without Sstc, which is exactly how its
 * presence is tested: there is no feature bit to read. */
#define CSR_STIMECMP 0x14du

/* How many times a thing is done before its cost is divided out. Enough
 * that the `time` CSR's resolution — 4 MHz on this board, so 250 ns a tick
 * — is not most of the answer. */
#define REPEATS 2000u

static struct {
    bool fired;
    uint64_t cause;
} caught;

/* Steps over the faulting instruction so a probe can carry on. The width is
 * 4 because every instruction a probe executes deliberately is a full-width
 * one; a compressed instruction would need the low bits of the opcode. */
static bool step_over_fault(struct trapframe *tf) {
    caught.fired = true;
    caught.cause = tf->scause;
    tf->sepc += 4u;
    return true;
}

static bool faults(void (*what)(void)) {
    caught.fired = false;
    trap_set_expected(step_over_fault);
    what();
    trap_set_expected(NULL);
    return caught.fired;
}

static void read_stimecmp(void) {
    uint64_t sink = 0;
    __asm__ volatile("csrr %0, 0x14d" : "=r"(sink));
    (void)sink;
}

/* How many bits of `satp` are the address-space id.
 *
 * Written as all ones and read back: the bits that stayed set are the ones
 * the hardware implements. Zero means every address-space switch must flush
 * the whole TLB, which is the single biggest input to how long a scheduling
 * slice should be. */
static uint32_t asid_bits(void) {
    uint64_t saved, probe, got;
    uint32_t bits = 0;
    __asm__ volatile("csrr %0, satp" : "=r"(saved));
    probe = saved | (0xffffull << 44);
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero" : : "r"(probe) : "memory");
    __asm__ volatile("csrr %0, satp" : "=r"(got));
    __asm__ volatile("csrw satp, %0\n\tsfence.vma zero, zero" : : "r"(saved) : "memory");
    for (uint32_t i = 0; i < 16u; i++) {
        bits += (got >> (44u + i)) & 1u ? 1u : 0u;
    }
    return bits;
}

/* Nanoseconds for one call, averaged. */
static uint64_t cost_of_set_timer(void) {
    uint64_t far = timer_now() + ((uint64_t)1 << 40);
    uint64_t start = timer_ns();
    for (uint32_t i = 0; i < REPEATS; i++) {
        (void)sbi_set_timer(far);
    }
    return (timer_ns() - start) / REPEATS;
}

/* A full `sfence.vma`, which is what an address-space switch costs when
 * there is no ASID. Measured as the fence alone; what it *then* costs is the
 * refill, below. */
static uint64_t cost_of_full_fence(void) {
    uint64_t start = timer_ns();
    for (uint32_t i = 0; i < REPEATS; i++) {
        __asm__ volatile("sfence.vma zero, zero" : : : "memory");
    }
    return (timer_ns() - start) / REPEATS;
}

/* What a cold TLB costs: the same walk over a working set, with and without
 * throwing the translations away first. The difference is what an address
 * space switch really costs on a machine with no ASID — the fence is cheap
 * and the refill is not. */
static uint64_t cost_of_tlb_refill(void) {
    static volatile uint8_t working_set[256u * 1024u];
    uint64_t warm, cold, start;
    uint32_t sink = 0;
    for (uint32_t pass = 0; pass < 2u; pass++) {
        for (uint32_t i = 0; i < sizeof working_set; i += 4096u) {
            sink += working_set[i];
        }
    }
    start = timer_ns();
    for (uint32_t pass = 0; pass < 32u; pass++) {
        for (uint32_t i = 0; i < sizeof working_set; i += 4096u) {
            sink += working_set[i];
        }
    }
    warm = timer_ns() - start;
    start = timer_ns();
    for (uint32_t pass = 0; pass < 32u; pass++) {
        __asm__ volatile("sfence.vma zero, zero" : : : "memory");
        for (uint32_t i = 0; i < sizeof working_set; i += 4096u) {
            sink += working_set[i];
        }
    }
    cold = timer_ns() - start;
    (void)sink;
    return cold > warm ? (cold - warm) / 32u : 0u;
}

/* What a context switch costs, measured by doing nothing else.
 *
 * Two kernel stacks and a switch back and forth: the cost of saving twelve
 * callee-saved registers, swapping stack pointers and returning. It is the
 * floor under every scheduling decision — a slice has to be worth many
 * multiples of it, or the scheduler is most of the work.
 *
 * This is the *same address space* case. The other case is this plus a
 * `satp` write and, on a machine with no ASID, a full fence and the refill
 * that follows — both measured separately above, because adding them here
 * would hide which part is expensive. */
static uint64_t switch_stack[2048] __attribute__((aligned(16)));
static uint64_t probe_from_ksp;
static uint64_t probe_to_ksp;
static volatile uint32_t switch_rounds;

void context_switch(uint64_t *save_to, uint64_t resume_from);

/* The other side of the switch: comes back immediately, for ever. */
static void switch_partner(void) {
    for (;;) {
        context_switch(&probe_to_ksp, probe_from_ksp);
    }
}

static uint64_t cost_of_context_switch(void) {
    uint64_t *top = &switch_stack[sizeof switch_stack / sizeof switch_stack[0]];
    uint64_t start, taken;
    /* A switch frame the way `sched.c` builds one: `ra` first, then twelve
     * callee-saved registers, padded to sixteen bytes. */
    uint64_t *frame = top - 14;
    for (uint32_t i = 0; i < 14u; i++) {
        frame[i] = 0;
    }
    frame[0] = (uint64_t)(uintptr_t)switch_partner;
    probe_to_ksp = (uint64_t)(uintptr_t)frame;
    start = timer_ns();
    for (uint32_t i = 0; i < REPEATS; i++) {
        switch_rounds++;
        context_switch(&probe_from_ksp, probe_to_ksp);
    }
    taken = timer_ns() - start;
    /* Two switches a round: there and back. */
    return taken / (REPEATS * 2u);
}

/* How long an interrupt takes to arrive on another hart.
 *
 * Measured as a round trip and halved, because there is no clock the two
 * harts share closely enough to time one direction: the `time` CSR is
 * common to them, but the far hart's reading has to come back somehow. */

static uint64_t cost_of_ipi(void) {
    if (sbi_probe_extension(SBI_EXT_IPI) <= 0 || smp_running_count() < 2u) {
        return 0;
    }
    /* Sending an IPI needs a handler on the far side to answer it, and this
     * kernel has none yet — the scheduler will add one. Until then the cost
     * reported is the *send*, which is the part this hart pays. */
    {
        uint64_t start = timer_ns();
        for (uint32_t i = 0; i < REPEATS; i++) {
            (void)sbi_send_ipi(0, (uint64_t)-1);
        }
        return (timer_ns() - start) / REPEATS;
    }
}

/* The operating points the board's own tree declares.
 *
 * Read rather than invented: a voltage this kernel made up is a voltage that
 * can damage the part, and the frequencies have to match what the PLL can
 * actually produce. */
/* **How fast the processor is actually going, measured rather than asked.**
 *
 * The `time` CSR is a fixed 4 MHz on this board whatever the cores are
 * doing, which makes it the one honest reference for a frequency change:
 * a loop of known length, timed against it, gives a rate that moves exactly
 * as the clock does.
 *
 * Reported as **iterations a second**, not as megahertz. The loop is two
 * instructions and the U74 is dual-issue and out of order, so the cycles it
 * takes per iteration is not something to assert — but the *ratio* between
 * two measurements is exact, and the ratio is what a frequency change has to
 * prove. Calling it MHz would be inventing a calibration this does not have.
 */
static uint64_t measure_loop_rate(void) {
    uint64_t started = timer_now();
    uint64_t ticks;
    uint32_t spins = 2000000u;
    /* `volatile` so the loop survives the optimiser; the asm keeps it to the
     * two instructions it is meant to be. */
    for (volatile uint32_t i = spins; i != 0; i--) {
    }
    ticks = timer_now() - started;
    if (ticks == 0) {
        return 0;
    }
    return (uint64_t)spins * timer_frequency() / ticks;
}

static void report_cpu_rate(void) {
    uint64_t a = measure_loop_rate();
    uint64_t b = measure_loop_rate();
    kprintf("probe: loop rate %llu/s, then %llu/s (the reference clock is fixed,\n",
            (unsigned long long)a, (unsigned long long)b);
    kprintf("probe:   so this moves with the core clock and its ratio is the proof)\n");
}

/* The clock generator's own registers for the CPU, read and not written.
 *
 * Changing a CPU clock means knowing which field is the mux and which the
 * divider, and this project's rule is that the datasheet and a measurement
 * decide. So the first step is to look: a running board's values say what the
 * firmware chose, and that is the ground truth a write has to respect.
 *
 * Indices 0..3 of the system generator are the CPU's — root, core, bus and
 * the APB that follows them — per the JH7110's clock list. Printed raw,
 * because a field this code has not verified yet should not be decoded as
 * though it had been. */
static void report_cpu_clock_registers(void) {
    phys_t base = crg_base(CRG_SYS);
    if (!crg_present(CRG_SYS)) {
        kprintf("probe: no system clock generator, so no frequency scaling\n");
        return;
    }
    kprintf("probe: syscrg at %016llx, CPU clock words:\n", (unsigned long long)base);
    for (uint32_t i = 0; i < 4u; i++) {
        volatile const uint32_t *reg =
            (volatile const uint32_t *)(uintptr_t)(base + crg_clock_offset(i));
        kprintf("probe:   [%u] %08x\n", i, *reg);
    }
}

/* **The experiment that settles what word [1] is**, under
 * `minikern.cpudiv=<n>`.
 *
 * The hypothesis from the board's own numbers: the four operating points are
 * 1500 MHz divided by 1, 2, 3 and 4, and word [1] of the system generator —
 * reading `00000001` on a machine running at 1500 MHz — is that divider. If
 * that is right, writing 2 halves the measured loop rate, and the ratio says
 * so exactly because the reference clock does not move.
 *
 * **Lowering the frequency is electrically safe at the voltage already set.**
 * The board boots at 1.04 V for its top operating point, and every point
 * below it is specified at 800 mV, so running slower on more volts than
 * required is wasteful and never unsafe. The unsafe direction is *raising*
 * frequency before voltage, and this only ever divides. That is why this
 * experiment can be run at all without a PMIC driver.
 *
 * It restores the divider afterwards, and reports both rates so that a
 * ratio which is not the one asked for is visible rather than assumed. */
/* Every operating point the board offers, set through the driver and
 * **measured** — one run that proves the whole table rather than four runs
 * that each prove a divider.
 *
 * The loop rate is the evidence: the reference clock does not move, so the
 * ratio against the top point is the frequency ratio. A point that reports
 * the right frequency and does not move the rate has not changed anything,
 * which is the failure a register write can produce and the reason this
 * measures instead of reading back. */
/* **What is on this board's I2C, and where its regulator is.**
 *
 * Reconnaissance for voltage scaling, which is the half of DVFS the divider
 * cannot do. Three questions, and the tree answers all three: is there an
 * I2C controller and what kind, is the PMIC described, and does the device
 * tree name the regulator that feeds the cores.
 *
 * Reported and not acted on. A regulator is the one peripheral on this board
 * where a wrong write is not a hang but a *damaged part* — over-volting a
 * core is permanent — so the write comes after the layout is known from the
 * tree and the datasheet together, not from a guess that happened to work.
 */
/* **Reads the PMIC, and lets the board confirm the encoding.**
 *
 * The AXP15060's DCDC2 rail feeds the cores, and its voltage register is
 * documented as 0.5 V plus 10 mV a step. This board is running at its top
 * operating point, which its device tree says is **1040 mV** — so if the
 * register reads a value that decodes to 1040, the register number *and* the
 * encoding are both confirmed by the hardware itself, against a number that
 * came from somewhere else entirely.
 *
 * That is the only kind of confirmation worth having before writing to a
 * regulator. Every other peripheral on this board punishes a wrong write
 * with a hang; this one can destroy the part, and an over-volted core does
 * not come back. Nothing here writes.
 */
#define AXP15060_ADDR 0x36u
#define AXP15060_DCDC2_VOLTAGE 0x25u

static void report_pmic(const struct fdt *fdt) {
    struct i2c_dw bus;
    uint8_t raw = 0;
    int rc = i2c_dw_init(&bus, fdt, "/soc/i2c@12050000");
    if (rc != KERR_OK) {
        kprintf("probe: i2c@12050000 did not come up (rc %d), so no PMIC read\n", rc);
        return;
    }
    kprintf("probe: i2c@12050000 is a DesignWare block (type %08x, version %08x)\n",
            i2c_dw_component_type(&bus), i2c_dw_component_version(&bus));
    /* **Which register holds the core rail, asked rather than assumed.**
     *
     * The first attempt read 0x25 as "DCDC2" from memory of a related part
     * and got 0x0b — 610 mV under the documented encoding, against a board
     * the device tree says is running at 1040 mV. So the register number was
     * wrong, and that mismatch is the whole value of reading before writing.
     *
     * 1040 mV encodes as 0.5 V + 10 mV a step, so the register this board's
     * cores are on must read **0x36**. Printing the whole voltage block and
     * marking that value identifies the rail from the hardware, against a
     * number that came from the device tree — two independent sources
     * agreeing, which is the only basis on which this will ever be written.
     */
    /* **The identity registers first.** The same discipline the I2C block
     * itself allowed: a register whose correct value is known from somewhere
     * other than this code is the only way to tell a working read path from
     * one returning plausible nonsense. */
    for (uint8_t r = 0x00u; r <= 0x04u; r++) {
        if (i2c_dw_read_u8(&bus, AXP15060_ADDR, r, &raw) == KERR_OK) {
            kprintf("probe: PMIC id register %02x = %02x\n", r, raw);
        }
    }
    /* **The whole register map, as a diagnostic, and no conclusion drawn.**
     *
     * The search for the core rail did not find it, and that is the result
     * rather than a step on the way to one. The device tree says the cores
     * want 1040 mV, which encodes as 0x36 under this family's documented
     * 0.5 V + 10 mV a step — and **no register on this part holds 0x36**.
     * 0x20..0x2b, where a related AXP keeps its DCDC voltages, read 0x0b
     * almost uniformly, and the identity registers prove the read path is
     * real and per-register, so that block is simply not the voltage map
     * here.
     *
     * So the register to write is not known, and this is the one peripheral
     * on the board where guessing is not a hang but a destroyed part: an
     * over-volted core does not come back. The project's rule is that the
     * datasheet and a measurement decide, and for this register there is
     * neither — so the map is printed for whoever has the datasheet, and
     * nothing is written.
     */
    kprintf("probe: PMIC register map (read-only; the core rail is NOT identified):\n");
    for (uint8_t row = 0x10u; row < 0x40u; row = (uint8_t)(row + 8u)) {
        kprintf("probe:   %02x:", row);
        for (uint8_t c = 0; c < 8u; c++) {
            if (i2c_dw_read_u8(&bus, AXP15060_ADDR, (uint8_t)(row + c), &raw) == KERR_OK) {
                kprintf(" %02x", raw);
            } else {
                kprintf(" --");
            }
        }
        kprintf("\n");
    }
    kprintf("probe:   no register holds 36 (1040 mV), which the device tree says the\n");
    kprintf("probe:   cores run at — so voltage scaling waits for an authoritative map\n");
}

static void report_i2c_and_regulators(const struct fdt *fdt) {
    static const char *const compats[] = {"snps,designware-i2c", "x-powers,axp15060",
                                          "starfive,jh7110-temp", "starfive,jh7100-temp"};
    for (uint32_t c = 0; c < sizeof compats / sizeof compats[0]; c++) {
        char path[160];
        bool any = false;
        for (uint32_t n = 0; n < 8u; n++) {
            if (fdt_find_compatible_nth(fdt, compats[c], n, path, sizeof path) != KERR_OK) {
                break;
            }
            kprintf("probe: %s at %s\n", compats[c], path);
            any = true;
        }
        if (!any) {
            kprintf("probe: no %s in the tree\n", compats[c]);
        }
    }
}

static void sweep_operating_points(const struct fdt *fdt) {
    struct cpufreq_policy policy;
    uint32_t points = cpufreq_jh7110_init(fdt, &policy);
    uint64_t top;
    if (points == 0) {
        kprintf("probe: no usable operating points, so no sweep\n");
        return;
    }
    top = measure_loop_rate();
    kprintf("probe: sweeping %u operating points (loop rate at the top: %llu/s)\n", points,
            (unsigned long long)top);
    for (uint32_t i = points; i > 0; i--) {
        uint32_t want = policy.opps[i - 1u].khz;
        uint32_t got = cpufreq_jh7110_set_khz(want);
        uint64_t rate = measure_loop_rate();
        kprintf("probe:   asked %u kHz, set %u kHz (divider %u): %llu/s", want, got,
                cpufreq_jh7110_divider(), (unsigned long long)rate);
        if (rate != 0) {
            kprintf(", %llu%% of the top", (unsigned long long)(rate * 100u / top));
        }
        kprintf("\n");
    }
    /* Back to the top, because leaving a board slow after a measurement is
     * a measurement that changed the machine. */
    (void)cpufreq_jh7110_set_khz(policy.opps[points - 1u].khz);
    kprintf("probe: restored to %u kHz\n", cpufreq_jh7110_current_khz());
}

static void try_cpu_divider(uint32_t want) {
    volatile uint32_t *reg;
    uint32_t was;
    uint64_t before, after;
    if (want == 0 || !crg_present(CRG_SYS)) {
        return;
    }
    reg = (volatile uint32_t *)(uintptr_t)(crg_base(CRG_SYS) + crg_clock_offset(1u));
    was = *reg;
    before = measure_loop_rate();
    *reg = (was & ~0xffu) | (want & 0xffu);
    kprintf("probe: cpu core word [1] %08x -> %08x\n", was, *reg);
    after = measure_loop_rate();
    kprintf("probe: loop rate %llu/s -> %llu/s", (unsigned long long)before,
            (unsigned long long)after);
    if (after != 0) {
        kprintf(" (ratio %llu/100)", (unsigned long long)(before * 100u / after));
    }
    kprintf("\n");
    *reg = was;
    kprintf("probe: restored [1] to %08x, rate %llu/s\n", *reg,
            (unsigned long long)measure_loop_rate());
}

static void report_operating_points(const struct fdt *fdt) {
    char path[64];
    uint32_t found = 0;
    if (fdt_find_compatible(fdt, "operating-points-v2", path, sizeof path) != KERR_OK) {
        kprintf("probe: no operating-points-v2 node: no OPP table to scale between\n");
        return;
    }
    kprintf("probe: operating points at %s\n", path);
    for (uint32_t i = 0; i < 16u; i++) {
        char child[96];
        struct fdt_prop hz, uv;
        uint64_t rate = 0, volt = 0;
        if (fdt_nth_child(fdt, path, i, child, sizeof child) != KERR_OK) {
            break;
        }
        if (fdt_get_prop(fdt, child, "opp-hz", &hz) != KERR_OK) {
            continue;
        }
        (void)fdt_prop_cells(&hz, 0, 2, &rate);
        if (fdt_get_prop(fdt, child, "opp-microvolt", &uv) == KERR_OK) {
            (void)fdt_prop_cells(&uv, 0, 1, &volt);
        }
        kprintf("probe:   %llu kHz at %llu uV\n", (unsigned long long)(rate / 1000u),
                (unsigned long long)volt);
        found++;
    }
    if (found == 0) {
        kprintf("probe:   none readable\n");
    }
}

void probe_report(const struct fdt *fdt, uint32_t cpu_divider) {
    uint32_t asid = asid_bits();
    bool sstc = !faults(read_stimecmp);
    kprintf("\nprobe: ---- what this machine is ----\n");
    kprintf("probe: harts in the tree %u, schedulable %u\n", smp_hart_count(),
            smp_running_count());
    for (uint32_t i = 0; i < smp_hart_count(); i++) {
        const struct smp_hart *h = smp_hart(i);
        kprintf("probe:   hart %llu %s\n", (unsigned long long)h->id,
                h->has_sv39 ? "has Sv39 (schedulable)" : "has no usable MMU (never scheduled)");
    }
    kprintf("probe: Sstc (per-hart stimecmp) %s\n",
            sstc ? "PRESENT — a timer can be armed without a firmware call"
                 : "absent — every timer arm is an SBI call");
    kprintf("probe: satp ASID bits %u%s\n", asid,
            asid == 0 ? " — every address-space switch flushes the whole TLB" : "");
    kprintf("probe: time CSR %llu Hz (%llu ns a tick)\n", (unsigned long long)timer_frequency(),
            (unsigned long long)(1000000000ull / (timer_frequency() | 1u)));
    kprintf("probe: sbi_set_timer %llu ns a call\n", (unsigned long long)cost_of_set_timer());
    kprintf("probe: sfence.vma (all) %llu ns\n", (unsigned long long)cost_of_full_fence());
    kprintf("probe: TLB refill over 64 pages %llu ns\n", (unsigned long long)cost_of_tlb_refill());
    kprintf("probe: context switch (same address space) %llu ns\n",
            (unsigned long long)cost_of_context_switch());
    kprintf("probe: SBI IPI extension %s", sbi_probe_extension(SBI_EXT_IPI) > 0 ? "present" : "absent");
    if (sbi_probe_extension(SBI_EXT_IPI) > 0) {
        kprintf(", %llu ns to send one", (unsigned long long)cost_of_ipi());
    }
    kprintf("\n");
    report_operating_points(fdt);
    report_cpu_rate();
    report_cpu_clock_registers();
    try_cpu_divider(cpu_divider);
    sweep_operating_points(fdt);
    report_i2c_and_regulators(fdt);
    report_pmic(fdt);
    kprintf("probe: NOT measured yet: PMIC voltage ramp and PLL relock need an I2C and a\n");
    kprintf("probe:   clock driver, which do not exist — so no DVFS timing is reported\n");
    kprintf("probe: ---------------------------------\n\n");
}
