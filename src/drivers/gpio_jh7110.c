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

/* The clock and the reset each controller needs, and which generator owns
 * them. The `sys` pin controller is clocked and reset by the system
 * generator; the always-on one by the always-on generator — which is the
 * same "a pin controller is not one block" point one level down. */
static bool switch_on(uint32_t which) {
    enum crg_domain domain = which == 0 ? CRG_SYS : CRG_AON;
    int rc;
    if (!crg_present(domain)) {
        kprintf("gpio: no clock generator for controller %u; not touching it\n", which);
        return false;
    }
    rc = crg_enable_clock(domain, JH7110_SYSCLK_IOMUX_APB);
    if (rc != KERR_OK) {
        kprintf("gpio: controller %u's clock would not enable (rc %d)\n", which, rc);
        return false;
    }
    rc = crg_release_reset(domain, JH7110_SYSRST_IOMUX_APB);
    if (rc != KERR_OK) {
        kprintf("gpio: controller %u stayed in reset (rc %d)\n", which, rc);
        return false;
    }
    return true;
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
        controllers[i].up = switch_on(i);
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

int gpio_jh7110_set_output(uint32_t controller, uint32_t pin, bool high) {
    int rc = pin_ok(controller, pin);
    volatile uint8_t *regs;
    if (rc != KERR_OK) {
        return rc;
    }
    regs = controllers[controller].regs;
    /* The value before the enable: a pin switched to output while its value
     * byte still holds the last thing written drives that for as long as it
     * takes to get to the next instruction. On a board that glitch is on a
     * wire. */
    regs[gpio_jh7110_dout_offset(pin)] = high ? DOUT_HIGH : DOUT_LOW;
    regs[gpio_jh7110_doen_offset(pin)] = DOEN_OUTPUT;
    return KERR_OK;
}

int gpio_jh7110_set_input(uint32_t controller, uint32_t pin) {
    int rc = pin_ok(controller, pin);
    if (rc != KERR_OK) {
        return rc;
    }
    controllers[controller].regs[gpio_jh7110_doen_offset(pin)] = DOEN_INPUT;
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
