/* The JH7110's own temperature sensor: `starfive,jh7110-temp` at
 * 0x120e0000, which is where `minikern.buses` found it on the board.
 *
 * It is a small block — a reset, a power-down, a run bit and a twelve-bit
 * reading — and the only part worth being careful about is the conversion,
 * because a sensor that returns a plausible-looking wrong number is worse
 * than one that returns nothing. The raw value is linear in temperature and
 * the SoC's documented relation is
 *
 *     T(C) = 237.5 * dout / 4096 - 81.1
 *
 * so a reading is reported in **millidegrees**, which is both what Linux's
 * thermal interface uses and precise enough that the arithmetic can stay in
 * integers: `dout * 237500 / 4096 - 81100`.
 *
 * **The registers are read back and printed on the first bring-up rather
 * than trusted.** Three times in this project a register map taken from
 * documentation has been wrong for this board — the SD controller was
 * documented as Cadence and is DesignWare, the GMAC's generation had to be
 * read from a version register, the watchdog's published offsets put the
 * load where the control is. A dump costs one line and settles it.
 */
#ifndef DRIVERS_TEMP_JH7110_H
#define DRIVERS_TEMP_JH7110_H

#include "core/fdt.h"
#include "core/types.h"

struct temp_jh7110 {
    volatile uint32_t *regs;
    bool up;
    /* **The extremes, because an instantaneous reading misses the point.**
     *
     * What a radiator and a fan are judged on is the peak under load, and a
     * reader that samples every few seconds sees neither the spike nor the
     * recovery — the die moves faster than anything polling it. These are
     * updated by every read, including the ones the housekeeping beat makes
     * when nobody is asking, so the peak between two readers' visits is
     * still there when the second one arrives.
     *
     * `TEMP_UNREADABLE` means no valid reading has been taken yet; a sensor
     * that never came up reports no peak rather than a plausible one. */
    int32_t lowest_mdeg;
    int32_t highest_mdeg;
    uint64_t readings;
};

/* Binds the sensor at `path`, clocks it and starts it converting. */
int temp_jh7110_init(struct temp_jh7110 *t, const struct fdt *fdt, const char *path);

/* The current temperature in millidegrees Celsius, or `TEMP_UNREADABLE`.
 *
 * A reading outside what silicon can physically be is **refused rather than
 * reported**: this project has already paid for a governor that acted on an
 * impossible -264 C, and a thermal number nobody sanity-checks is a number
 * that will eventually be acted on. */
#define TEMP_UNREADABLE (-1000000)
int32_t temp_jh7110_millidegrees(const struct temp_jh7110 *t);

/* The raw twelve-bit conversion, for reporting beside the temperature: it is
 * what says whether a wrong answer came from the sensor or from the
 * arithmetic. */
uint32_t temp_jh7110_raw(const struct temp_jh7110 *t);

/* The first few registers, for establishing the layout on a new board. */
void temp_jh7110_dump(const struct temp_jh7110 *t);

/* The extremes seen since boot, and how many valid readings they are drawn
 * from. Either extreme is `TEMP_UNREADABLE` when nothing has been read yet.
 * The count is what says whether a peak is worth believing: two readings and
 * a maximum is not a maximum. */
void temp_jh7110_extremes(const struct temp_jh7110 *t, int32_t *lowest, int32_t *highest,
                          uint64_t *readings);

/* Takes a reading purely to keep the extremes current, for calling from a
 * periodic hook. Separate from `millidegrees` so that a caller which only
 * wants the side effect does not read as one that wanted the value and
 * dropped it. */
void temp_jh7110_sample(struct temp_jh7110 *t);

#endif
