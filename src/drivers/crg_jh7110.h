/* crg_jh7110 — the clock and reset generators.
 *
 * **This exists because of a specific, expensive lesson.** On the board the
 * ARM kernel targeted, the thermal-sensor driver was complete and correct and
 * had never been switched on: nothing ungated its bus clock, released its
 * reset, or started its module clock. On silicon that block would have been
 * held in reset — every write ignored, every reading refused — while the
 * governor above it reported temperatures and appeared healthy. A
 * peripheral's clocks and resets are part of its contract, and a driver that
 * does not handle them is not finished.
 *
 * So every JH7110 peripheral driver in this tree goes through here first.
 *
 * The layout, which is pleasantly regular:
 *
 *   a **clock** is one 32-bit register at `base + 4 * index`, enabled by
 *   bit 31 — so turning one on is a read, an or, and a write;
 *   a **reset** is one bit in a bank: `assert + 4 * (id / 32)` holds it, and
 *   **setting the bit asserts** (holds the block in reset) while clearing it
 *   releases. A separate status bank reads back 1 for a released line, which
 *   is how a release is *confirmed* rather than assumed.
 *
 * There are several generators — one for the system peripherals, one in the
 * always-on domain, others for storage and video — each a separate node in
 * the device tree with its own base. They are found by `compatible`, never
 * by address.
 */
#ifndef MINIKERN_DRIVERS_CRG_JH7110_H
#define MINIKERN_DRIVERS_CRG_JH7110_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* Which generator. The two this kernel needs: the system one, which owns
 * most peripherals, and the always-on one. */
enum crg_domain {
    CRG_SYS = 0,
    CRG_AON = 1,
    CRG_DOMAINS,
};

/* Clock indices this kernel uses, as the generator numbers them. */
#define JH7110_SYSCLK_IOMUX_APB 112u

/* Reset ids, likewise. */
#define JH7110_SYSRST_IOMUX_APB 31u

/* ── the pure part: where a clock's and a reset's bits live ──────────────── */

/* A clock's control register: one word per index. */
static inline uint32_t crg_clock_offset(uint32_t index) {
    return index * 4u;
}

/* Which word of a reset bank holds an id, and which bit of it. A bank is
 * 32 resets wide, so these are the only two facts a caller needs — and
 * getting either wrong writes into a *different* peripheral's reset, which
 * is the kind of mistake that takes a board down in a way nothing explains. */
static inline uint32_t crg_reset_bank(uint32_t id) {
    return (id / 32u) * 4u;
}

static inline uint32_t crg_reset_bit(uint32_t id) {
    return id % 32u;
}

/* The bit that enables a clock. */
#define CRG_CLOCK_ENABLE_BIT 31u

/* Finds the generators in the device tree and maps them. Returns how many
 * were found; 0 means this is not a JH7110. */
uint32_t crg_jh7110_init(const struct fdt *fdt);

/* Turns a clock on, and reads it back to confirm.
 *
 * KERR_NOENT when that generator was not found, KERR_IO when the enable bit
 * does not stay set — which is what happens when the *parent* clock is off,
 * and is worth distinguishing from "I wrote it and assumed". */
int crg_enable_clock(enum crg_domain domain, uint32_t index);

/* Releases a reset and **confirms it** through the status register, which
 * reads 1 for a released line. KERR_TIMEDOUT when it does not come out of
 * reset: a block held in reset accepts writes and ignores them, so assuming
 * is how a driver comes to look like it works. */
int crg_release_reset(enum crg_domain domain, uint32_t id);

/* Where a generator was found. */
phys_t crg_base(enum crg_domain domain);
bool crg_present(enum crg_domain domain);

#endif
