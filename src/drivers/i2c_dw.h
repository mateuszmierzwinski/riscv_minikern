/* i2c_dw — the Synopsys DesignWare I²C master, polled.
 *
 * The VisionFive 2 has **seven** of these (`snps,designware-i2c`), and the
 * one that matters for frequency scaling is `i2c@12050000`: the AXP15060
 * PMIC sits on it at address 0x36, and that PMIC holds the core voltage.
 *
 * ## Why this driver can be trusted before it is trusted
 *
 * A DesignWare I²C block publishes its own identity in two read-only
 * registers at the top of its window: `IC_COMP_TYPE` reads `0x44570140` and
 * `IC_COMP_VERSION` reads an ASCII version. That makes the usual bring-up
 * risk — "are these the right register offsets?" — a question the hardware
 * answers, before a single byte goes out on a wire. `i2c_dw_init` checks it
 * and refuses the controller if it does not match, because the alternative
 * is writing a transfer command into whatever block actually lives there.
 *
 * This project has had the same lesson twice from registers that accepted
 * nonsense quietly: a UART whose registers were four bytes apart and a pin
 * controller that took 32-bit accesses only. Both were invisible under an
 * emulator and both produced *plausible* behaviour. An identity register is
 * the cheapest possible defence and this block has one.
 *
 * ## Scope, deliberately small
 *
 * Polled, no interrupts, one transfer at a time, 7-bit addresses, standard
 * mode. It exists to read and write single 8-bit registers in a PMIC, which
 * is two transfers of two bytes, and nothing here tries to be a general I²C
 * subsystem. The bus is 100 kHz, so a transfer is tens of microseconds and
 * polling it costs less than an interrupt would.
 */
#ifndef MINIKERN_DRIVERS_I2C_DW_H
#define MINIKERN_DRIVERS_I2C_DW_H

#include <stdbool.h>
#include <stdint.h>

#include "core/fdt.h"
#include "core/types.h"

/* A controller this driver has brought up. Opaque by convention: callers
 * hold one and pass it back. */
struct i2c_dw {
    phys_t base;
    bool up;
};

/* Brings up the controller at `path` in the device tree: enables its clock
 * and reset through the generator, maps it, **checks its identity
 * registers**, and configures it for 100 kHz standard mode.
 *
 * KERR_NOENT if the node is not there, KERR_IO if the identity does not
 * match — which means the offsets are wrong or that is not a DesignWare
 * block, and either way nothing should be written to it. */
int i2c_dw_init(struct i2c_dw *bus, const struct fdt *fdt, const char *path);

/* What the identity registers said, for the log. Zero before `init`. */
uint32_t i2c_dw_component_type(const struct i2c_dw *bus);
uint32_t i2c_dw_component_version(const struct i2c_dw *bus);

/* Reads one 8-bit register from a 7-bit address: writes the register number,
 * then reads one byte, as every PMIC and sensor of this shape expects.
 *
 * KERR_IO on a bus error or a device that does not answer, KERR_TIMEDOUT if
 * the controller never reports the transfer finished. The distinction
 * matters: "nothing is there" and "the controller is wedged" need different
 * responses from a caller. */
int i2c_dw_read_u8(struct i2c_dw *bus, uint8_t addr, uint8_t reg, uint8_t *out);

/* Writes one 8-bit register. Same errors.
 *
 * **No read-back here.** A register that does not take the value is a fact
 * about the *device*, not the bus, and only the caller knows whether reading
 * it back is meaningful — some PMIC registers are write-only and some change
 * what they report. The regulator driver above this does the check. */
int i2c_dw_write_u8(struct i2c_dw *bus, uint8_t addr, uint8_t reg, uint8_t value);

#endif
