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

/* **Not every clock in this generator has a gate.**
 *
 * A JH7110 clock register is one of several kinds. A *gate* keeps its
 * enable in bit 31. An *inverter* has no gate at all: bit 30 selects the
 * polarity of a clock that is simply always on, and bit 31 is not
 * implemented. There are also muxes (bits 27:24) and dividers (bits 23:0),
 * and one register may combine them.
 *
 * This matters because `crg_enable_clock` writes bit 31 and reads it back —
 * which is the right check for a gate and a **false negative** for an
 * inverter, whose bit 31 can never stick. Both of the board's Ethernet
 * controllers were refused for exactly that reason: each names an inverted
 * transmit clock, GMAC0's at AON index 6 and GMAC1's at SYS index 106, and
 * both read `0x4000_0000` — polarity set, no divider, no gate. */
#define CRG_CLOCK_INVERT_BIT 30u
#define CRG_CLOCK_MUX_MASK   0x0f000000u
#define CRG_CLOCK_DIV_MASK   0x00ffffffu

/* Finds the generators in the device tree and maps them. Returns how many
 * were found; 0 means this is not a JH7110. */
uint32_t crg_jh7110_init(const struct fdt *fdt);

/* Turns a clock on, and reads it back to confirm.
 *
 * KERR_NOENT when that generator was not found, KERR_IO when the enable bit
 * does not stay set — which is what happens when the *parent* clock is off,
 * and is worth distinguishing from "I wrote it and assumed". */
/* Enables a clock.
 *
 * `KERR_OK` when its gate is on, `KERR_NOTSUP` when the clock has **no
 * gate** — an inverter, which is already running and whose bit 31 can never
 * be set — and `KERR_IO` when the bit refuses for any other reason, which
 * is the diagnosis that caught the always-on pin controller being switched
 * on through the wrong generator.
 *
 * The three answers are worth keeping apart. Collapsing the middle one into
 * `KERR_IO` refused two working clocks; collapsing it into `KERR_OK` would
 * throw away the check that found a real bug. */
int crg_enable_clock(enum crg_domain domain, uint32_t index);

/* Releases a reset and **confirms it** through the status register, which
 * reads 1 for a released line. KERR_TIMEDOUT when it does not come out of
 * reset: a block held in reset accepts writes and ignores them, so assuming
 * is how a driver comes to look like it works. */
int crg_release_reset(enum crg_domain domain, uint32_t id);

/* Switches on everything a device node asks for: each clock in its `clocks`
 * property and each reset in its `resets`.
 *
 * A node names them as (phandle, index) pairs, and the phandle says which
 * generator — so this resolves the phandle rather than assuming the system
 * one, because a device on the always-on generator would otherwise have its
 * neighbour's clock switched on instead.
 *
 * It is how a driver switches its own controller on without knowing any
 * clock numbers: the tree already holds them. KERR_NOENT when the node has
 * no clocks, and whatever the enable or the release reported when one of
 * them fails — both of which are confirmed by reading back, so a failure
 * here is a block that really is not running. */
int crg_enable_device(const struct fdt *fdt, const char *path);

/* Where a generator was found. */
phys_t crg_base(enum crg_domain domain);

/* **Where this kernel reaches that domain's registers.** `crg_base` is the
 * address the hardware decodes, which stopped being usable as a pointer
 * when the kernel left the identity map. NULL if the domain is not up. */
volatile uint32_t *crg_regs(enum crg_domain domain);
bool crg_present(enum crg_domain domain);

#endif
