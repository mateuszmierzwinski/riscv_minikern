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
 * Its **clock and reset are handled** (drivers/crg_jh7110.h), and both are
 * confirmed by reading back rather than assumed: a block held in reset
 * accepts every write and ignores all of them, which is how the ARM thermal
 * sensor came to look like it worked. A controller whose clock will not
 * enable or whose reset will not release is **not marked up**, so nothing
 * above it can drive a pin through a block that is not listening.
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

int gpio_jh7110_peek(uint32_t controller, uint32_t byte_offset, uint32_t *word);

/* A pin's pad-configuration word: where the pad's own electrical behaviour
 * lives, as distinct from which signal drives it.
 *
 * The block begins just past the two input-state words, one 32-bit word per
 * pin, and this was **measured on a board rather than taken from a
 * datasheet**: dumping it showed pin 60 reading `0x09` and pins 61, 63 and
 * 36 reading `0x01`, and pin 60 was the only one of the four that could
 * actually be driven. One bit separates a pin that works from a pin whose
 * writes go nowhere.
 *
 * Writing it is scoped to one named pin for a reason: a pin controller is
 * where a wrong write reaches a pad that is wired to something, so this
 * takes a pin number and not an offset. */
#define JH7110_PADCFG_BASE 0x120u

static inline uint32_t gpio_jh7110_padcfg_offset(uint32_t pin) {
    return JH7110_PADCFG_BASE + pin * 4u;
}

int gpio_jh7110_set_padcfg(uint32_t controller, uint32_t pin, uint32_t value);
int gpio_jh7110_get_padcfg(uint32_t controller, uint32_t pin, uint32_t *value);

#endif
