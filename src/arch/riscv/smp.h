/* smp — the other harts.
 *
 * Starting a core here is one firmware call: `sbi_hart_start(hart, address,
 * opaque)` enters that hart **in S-mode**, with translation off, at a
 * physical address. The specification says S-mode, so the entire class of
 * bug that made the ARM bring-up hard — PSCI starting a secondary in the
 * mode the *primary* was entered in, so a core that skipped the HYP→SVC
 * drop wrote the hypervisor's MMU registers and died on its next
 * instruction fetch, while PSCI reported success — cannot happen.
 *
 * **The hart numbering is the thing to get right.** The boot hart is not
 * necessarily 0 (QEMU's `virt` picks one by lottery, and `sifive_u` boots on
 * hart 1), and on the JH7110 hart 0 is an **S7 monitor core** with four U74
 * application cores at 1–4. A kernel that assumes it is hart 0, or that the
 * application cores are `0..n-1`, is wrong on the target board — so the
 * harts come from the device tree's `cpus` node, with each one's
 * `riscv,isa` and `mmu-type` read, and a hart that cannot run this kernel
 * is skipped rather than started and lost.
 */
#ifndef MINIKERN_ARCH_RISCV_SMP_H
#define MINIKERN_ARCH_RISCV_SMP_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

#define SMP_MAX_HARTS 8u

/* One hart, as the device tree describes it. */
struct smp_hart {
    uint64_t id;
    bool has_sv39;    /* an `mmu-type` this kernel can use */
    bool startable;   /* not the boot hart, and the firmware agrees it exists */
    bool running;     /* it reached the kernel and said so */
    char path[64];
};

/* Reads the `cpus` node. Reports how many harts were found, with the boot
 * hart marked. KERR_NOENT when there is no `cpus` node at all. */
int smp_enumerate(const struct fdt *fdt, uint64_t boot_hart);

/* How many harts there are, and one of them. */
uint32_t smp_hart_count(void);

/* How many times a hart could not find itself in the table and was given
 * index 0 — which means it shared another hart's per-hart state. Any value
 * but zero is a bug worth stopping for. */
uint32_t smp_hart_misses(void);
const struct smp_hart *smp_hart(uint32_t index);

/* How many are running kernel code, the boot hart included. */
uint32_t smp_running_count(void);

/* Starts every startable hart at `entry` (a **physical** address, because a
 * hart begins with translation off), handing each `opaque` in `a1`. Reports
 * how many the firmware accepted — a hart that refuses to start is logged
 * and skipped, not fatal: a machine with fewer cores still works. */
uint32_t smp_start_others(phys_t entry, uint64_t opaque);

/* Called by a secondary once it is on the kernel's own footing. Marks it
 * running. */
void smp_secondary_arrived(uint64_t hart);

/* This hart's position in the enumeration, which is what a per-hart array
 * is indexed by — the id is not, because the ids are not 0..n-1 (the
 * JH7110's are an S7 at 0 and four U74s at 1-4, and a tree may list them in
 * any order). Returns 0 for a hart that was never enumerated, which is the
 * boot hart's slot before `smp_enumerate` has run. */
uint32_t smp_hart_index(void);

/* The id of the hart this is running on, which is kept in `sscratch`'s
 * sibling — a per-hart register — rather than worked out, because there is
 * no instruction that asks "which hart am I" from S-mode. */
uint64_t smp_this_hart(void);
void smp_set_this_hart(uint64_t hart);

#endif
