#include "kernel/probe.h"

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/sbi.h"
#include "arch/riscv/smp.h"
#include "arch/riscv/timer.h"
#include "arch/riscv/trap.h"
#include "core/console.h"
#include "core/errno.h"

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

void probe_report(const struct fdt *fdt) {
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
    kprintf("probe: NOT measured yet: PMIC voltage ramp and PLL relock need an I2C and a\n");
    kprintf("probe:   clock driver, which do not exist — so no DVFS timing is reported\n");
    kprintf("probe: ---------------------------------\n\n");
}
