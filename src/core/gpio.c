#include "core/gpio.h"

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* The port letter's index, or -1. PA..PH are the main controller; **PL** is
 * the always-on one, and is accepted because that is where a board's
 * CPU-supply switch is (gpio.h). The letters between H and L have no
 * controller on this chip and are refused. */
static int port_of(char c) {
    char up = c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c;
    if (up == 'L') {
        return (int)GPIO_PORT_L;
    }
    return up >= 'A' && up < (char)('A' + GPIO_PORTS) ? up - 'A' : -1;
}

bool gpio_parse(const char *name, uint32_t len, struct gpio_pin *out) {
    uint32_t at = 0;
    uint32_t pin = 0;
    int port;
    if (len > 0 && (name[0] == 'P' || name[0] == 'p') && len > 2) {
        at = 1; /* the optional P */
    }
    if (at >= len) {
        return false;
    }
    port = port_of(name[at]);
    if (port < 0 || at + 1 >= len) {
        return false;
    }
    for (at++; at < len; at++) {
        if (!is_digit(name[at])) {
            return false;
        }
        pin = pin * 10u + (uint32_t)(name[at] - '0');
        if (pin >= GPIO_PINS) {
            return false;
        }
    }
    out->port = (uint8_t)port;
    out->pin = (uint8_t)pin;
    return true;
}
