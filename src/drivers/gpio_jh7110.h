/* gpio_jh7110 — the board's pins.
 *
 * **There are two pin controllers, and that is the first thing to get
 * right.** The JH7110 has a `sys` controller with 64 GPIOs and an
 * always-on `aon` controller with four more, at different addresses, with
 * their own clocks. The ARM kernel this migrated from learned that lesson
 * expensively: the one pin anybody cared about there lived in a second
 * controller its pin parser did not know existed, so the feature was
 * unusable on every board that had it and the failure was silent. So this
 * enumerates controllers from the device tree rather than assuming one.
 *
 * The register layout is **unusual and worth stating plainly**: a pin's
 * output enable and output value are each a *byte* in a table, not a bit in
 * a word. 64 pins occupy 64 bytes at `DOEN`, then 64 more at `DOUT`. Inputs
 * are the conventional shape — one bit per pin across two words. Writing a
 * bit where a byte belongs lands on a neighbouring pin, which on a board
 * wired to real hardware is how a GPIO driver sets something on fire.
 *
 * The arithmetic that maps a pin to a register and an offset is therefore
 * **pure and host-tested** (`gpio_jh7110_doen_offset` and friends), because
 * it is the part that can be wrong in a way no boot log would show.
 *
 * **What is not verified, and cannot be here:** QEMU models no pin
 * controller of any kind, so every register access below has been written
 * from an implementation that runs on this silicon and has never run on it
 * from this kernel. The offsets are cross-checked; the behaviour is not.
 * Nor are the block's **clocks and resets** handled yet — the device tree
 * declares them, and the lesson from the ARM thermal sensor is exactly that
 * a complete driver which was never switched on reads nothing and looks
 * like it works. In practice the firmware enables this block because it
 * uses GPIO itself, which is an assumption and is recorded as one
 * (DEVELOPMENT_PROGRESS.md P6.1).
 */
#ifndef MINIKERN_DRIVERS_GPIO_JH7110_H
#define MINIKERN_DRIVERS_GPIO_JH7110_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* How many pins each controller has. */
#define JH7110_SYS_GPIOS 64u
#define JH7110_AON_GPIOS 4u

/* ── the pure part: where a pin's bits live ──────────────────────────────── */

/* A pin's output-enable byte, within the controller's registers. One byte
 * per pin, 64 of them, starting at the base of the block. */
static inline uint32_t gpio_jh7110_doen_offset(uint32_t pin) {
    return 0x000u + pin;
}

/* A pin's output-value byte: the next table along. */
static inline uint32_t gpio_jh7110_dout_offset(uint32_t pin) {
    return 0x040u + pin;
}

/* The word holding a pin's input bit, and which bit it is — the
 * conventional layout, unlike the two above. */
static inline uint32_t gpio_jh7110_input_offset(uint32_t pin) {
    return 0x118u + (pin / 32u) * 4u;
}

static inline uint32_t gpio_jh7110_input_bit(uint32_t pin) {
    return pin % 32u;
}

/* ── the live part ───────────────────────────────────────────────────────── */

/* Finds the pin controllers in the device tree and maps them. Returns how
 * many were found: 0 means this is not a JH7110, which is the ordinary case
 * under an emulator. */
uint32_t gpio_jh7110_init(const struct fdt *fdt);

/* Drives a pin, or reads one. `controller` is 0 for `sys` and 1 for `aon`.
 *
 * KERR_NOENT when that controller was not found, KERR_INVAL for a pin it
 * does not have — refused rather than wrapped, because a pin number out of
 * range that silently becomes a different pin is the worst outcome
 * available. */
int gpio_jh7110_set_output(uint32_t controller, uint32_t pin, bool high);
int gpio_jh7110_set_input(uint32_t controller, uint32_t pin);
int gpio_jh7110_read(uint32_t controller, uint32_t pin, bool *high);

/* Where a controller was found, for the boot log. */
phys_t gpio_jh7110_base(uint32_t controller);
const char *gpio_jh7110_path(uint32_t controller);

#endif
