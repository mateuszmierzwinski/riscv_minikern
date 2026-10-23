#include "drivers/gpio_jh7110.h"

#include <stddef.h>

#include "arch/riscv/mmu.h"
#include "core/console.h"
#include "core/errno.h"
#include "drivers/crg_jh7110.h"

/* The output-enable byte's values: 0 drives the pin, 1 leaves it an input.
 * Active low, which is worth naming rather than leaving as a bare 0. */
#define DOEN_OUTPUT 0u
#define DOEN_INPUT 1u

/* The output-value byte selects which signal drives the pin. For plain GPIO
 * those are two constants rather than a routed signal. */
#define DOUT_LOW 0u
#define DOUT_HIGH 1u

#define CONTROLLERS 2u

static struct {
    volatile uint8_t *regs;
    phys_t base;
    uint32_t pins;
    char path[48];
    bool up;
} controllers[CONTROLLERS];

static const struct {
    const char *compatible;
    uint32_t pins;
} kinds[CONTROLLERS] = {
    {"starfive,jh7110-sys-pinctrl", JH7110_SYS_GPIOS},
    {"starfive,jh7110-aon-pinctrl", JH7110_AON_GPIOS},
};

static void remember_path(uint32_t i, const char *path) {
    uint32_t at = 0;
    while (path[at] != '\0' && at + 1u < sizeof controllers[i].path) {
        controllers[i].path[at] = path[at];
        at++;
    }
    controllers[i].path[at] = '\0';
}

/* Switches a controller on using **the clock and reset its own node names**.
 *
 * It used to use a constant — `JH7110_SYSCLK_IOMUX_APB` — for both
 * controllers, and that is the system generator's numbering. The always-on
 * pin controller is clocked by the *always-on* generator, whose numbering is
 * its own, so the constant selected whatever happens to sit at that index
 * there: on a real board the enable bit would not stay set and controller 1
 * refused to start (`rc -5`). The number was never this driver's to know —
 * the tree carries it, in the node's `clocks` and `resets`, and a board that
 * wires its pin controller differently is then right by construction.
 *
 * This is the same correction the SD controller needed, one level down: a
 * pin controller is not one block, and the second one is not the first with
 * a different address. */
static bool switch_on(const struct fdt *fdt, uint32_t which, const char *path) {
    int rc = crg_enable_device(fdt, path);
    if (rc == KERR_OK) {
        return true;
    }
    if (rc == KERR_NOENT) {
        /* The node names no clock this kernel can switch — on a board whose
         * firmware has already started it, that is the ordinary case and not
         * a reason to refuse the controller. */
        kprintf("gpio: controller %u names no clock here; taking it as already on\n", which);
        return true;
    }
    kprintf("gpio: controller %u would not switch on (rc %d)\n", which, rc);
    return false;
}

uint32_t gpio_jh7110_init(const struct fdt *fdt) {
    uint32_t found = 0;
    for (uint32_t i = 0; i < CONTROLLERS; i++) {
        char path[48];
        phys_t base = 0;
        uint64_t size = 0;
        if (fdt_find_compatible(fdt, kinds[i].compatible, path, sizeof path) != KERR_OK ||
            fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK ||
            mmu_map_device(base, size) != KERR_OK) {
            continue;
        }
        controllers[i].regs = (volatile uint8_t *)(uintptr_t)base;
        controllers[i].base = base;
        controllers[i].pins = kinds[i].pins;
        remember_path(i, path);
        /* **Switch the block on before believing any of its registers.**
         * The device tree declares a clock and a reset for this node, and a
         * driver that skips them is the one the ARM thermal sensor was: every
         * write ignored, every read meaningless, and the layer above
         * reporting success. Both are confirmed by reading back, so a
         * failure here is a refusal rather than a silent half-start
         * (drivers/crg_jh7110.h). */
        controllers[i].up = switch_on(fdt, i, path);
        found += controllers[i].up ? 1u : 0u;
    }
    return found;
}

/* Checks the controller and the pin together, because every entry point
 * needs both and a pin number out of range must be refused rather than
 * wrapped onto a different pin. */
static int pin_ok(uint32_t controller, uint32_t pin) {
    if (controller >= CONTROLLERS || !controllers[controller].up) {
        return KERR_NOENT;
    }
    return pin < controllers[controller].pins ? KERR_OK : KERR_INVAL;
}

/* Writes one pin's 8-bit field as a **32-bit read-modify-write**.
 *
 * The field is a byte and the register is a word, and those are not the same
 * thing: this block takes 32-bit accesses, so a byte store to the right
 * address is not a narrower version of the same write — it is a write the
 * hardware need not honour at all. The driver did exactly that, and the
 * result on a board was that one header pin of four could be driven and the
 * other three could not, with no error anywhere: the stores were accepted
 * and discarded.
 *
 * It is the same mistake this project already paid for on the JH7110's UART,
 * whose `reg-io-width` is 4 — and the second time, it was not a register's
 * *spacing* that was wrong but the *width of the access*. Four pins share a
 * word, so the read-modify-write must also preserve its neighbours: writing
 * the word wholesale would reconfigure three other pins, which on a board is
 * three other wires. */
static void write_pin_field(uint32_t controller, uint32_t byte_offset, uint8_t value) {
    volatile uint32_t *word =
        (volatile uint32_t *)(controllers[controller].regs + (byte_offset & ~3u));
    uint32_t shift = 8u * (byte_offset & 3u);
    uint32_t now = *word;
    *word = (now & ~(0xffu << shift)) | ((uint32_t)value << shift);
}

int gpio_jh7110_set_output(uint32_t controller, uint32_t pin, bool high) {
    int rc = pin_ok(controller, pin);
    if (rc != KERR_OK) {
        return rc;
    }
    /* The value before the enable: a pin switched to output while its value
     * byte still holds the last thing written drives that for as long as it
     * takes to get to the next instruction. On a board that glitch is on a
     * wire. */
    write_pin_field(controller, gpio_jh7110_dout_offset(pin), high ? DOUT_HIGH : DOUT_LOW);
    write_pin_field(controller, gpio_jh7110_doen_offset(pin), DOEN_OUTPUT);
    return KERR_OK;
}

int gpio_jh7110_set_input(uint32_t controller, uint32_t pin) {
    int rc = pin_ok(controller, pin);
    if (rc != KERR_OK) {
        return rc;
    }
    write_pin_field(controller, gpio_jh7110_doen_offset(pin), DOEN_INPUT);
    return KERR_OK;
}

int gpio_jh7110_peek(uint32_t controller, uint32_t byte_offset, uint32_t *word) {
    if (controller >= CONTROLLERS || !controllers[controller].up || (byte_offset & 3u) != 0) {
        return KERR_INVAL;
    }
    *word = *(volatile uint32_t *)(controllers[controller].regs + byte_offset);
    return KERR_OK;
}

int gpio_jh7110_get_padcfg(uint32_t controller, uint32_t pin, uint32_t *value) {
    int rc = pin_ok(controller, pin);
    if (rc != KERR_OK) {
        return rc;
    }
    *value = *(volatile uint32_t *)(controllers[controller].regs +
                                    gpio_jh7110_padcfg_offset(pin));
    return KERR_OK;
}

int gpio_jh7110_set_padcfg(uint32_t controller, uint32_t pin, uint32_t value) {
    int rc = pin_ok(controller, pin);
    if (rc != KERR_OK) {
        return rc;
    }
    *(volatile uint32_t *)(controllers[controller].regs + gpio_jh7110_padcfg_offset(pin)) = value;
    return KERR_OK;
}

int gpio_jh7110_read(uint32_t controller, uint32_t pin, bool *high) {
    int rc = pin_ok(controller, pin);
    volatile uint32_t *word;
    if (rc != KERR_OK) {
        return rc;
    }
    word = (volatile uint32_t *)(controllers[controller].regs + gpio_jh7110_input_offset(pin));
    *high = ((*word >> gpio_jh7110_input_bit(pin)) & 1u) != 0;
    return KERR_OK;
}

phys_t gpio_jh7110_base(uint32_t controller) {
    return controller < CONTROLLERS ? controllers[controller].base : 0;
}

const char *gpio_jh7110_path(uint32_t controller) {
    return controller < CONTROLLERS && controllers[controller].up ? controllers[controller].path
                                                                  : "";
}
