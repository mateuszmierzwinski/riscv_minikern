/* gpio — naming a pin, and nothing else.
 *
 * A pin is a board fact: which one switches the CPU's supply is in the
 * schematic of the board in front of you, and it differs between boards
 * that share this chip. So it is **configuration**, not a constant compiled
 * in — and this is the part of that which can be tested, because turning
 * "PA10" into a port and a pin needs no hardware at all.
 */
#ifndef MINIKERN_CORE_GPIO_H
#define MINIKERN_CORE_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#define GPIO_PORTS 8u    /* PA..PH: the main pin controller */
#define GPIO_PINS  32u

/* **PL is a port too, and it is the one that matters here.** On every H3
 * board that has a switchable CPU supply, the pin that switches it is on
 * PL — the Orange Pi One's is PL6 — and PL lives in a *different* pin
 * controller (the "R_PIO" block in the always-on domain), not in PA..PH.
 * This parser accepted only PA..PH, so the one pin the feature exists for
 * was rejected as a typo on every board that has it. */
#define GPIO_PORT_L 11u  /* 'L' - 'A', so the letter arithmetic still holds */

struct gpio_pin {
    uint8_t port; /* 0 is PA; GPIO_PORT_L is PL, in the other controller */
    uint8_t pin;
};

/* True when this pin is in the always-on controller rather than the main
 * one, which decides the base address and the clock gate it needs. */
static inline bool gpio_is_r_port(struct gpio_pin pin) {
    return pin.port == GPIO_PORT_L;
}

/* Parses a name of the form "PA10" or "pa10" — the P is optional, the
 * letter is the port and the number is the pin. **PA..PH and PL** are
 * accepted; PL because that is where a board's CPU-supply switch is.
 * False for anything else, including a port letter this chip has no
 * controller for, a pin past 31, and trailing rubbish: a mis-typed pin
 * would drive the wrong line, and guessing which one was meant is not
 * something a kernel should do. */
bool gpio_parse(const char *name, uint32_t len, struct gpio_pin *out);

#endif
