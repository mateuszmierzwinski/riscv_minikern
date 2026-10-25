#include "drivers/pwm_jh7110.h"

#include "core/errno.h"
#include "drivers/crg_jh7110.h"
#include "arch/riscv/mmu.h"

/* Each channel has four 32-bit registers, in this order. The OpenCores PTC
 * names them CNTR, HRC, LRC and CTRL; `HRC` is where the output goes high
 * and `LRC` is where the counter reloads, so the period is LRC and the duty
 * is the distance between them. */
#define CHANNEL_STRIDE 0x10u
#define REG_CNTR 0x0u
#define REG_HRC 0x4u
#define REG_LRC 0x8u
#define REG_CTRL 0xcu

#define CTRL_EN (1u << 0)    /* the counter runs */
#define CTRL_ECLK (1u << 1)  /* 0 selects the internal clock, which is wanted */
#define CTRL_OE (1u << 3)    /* the output pin is driven */
#define CTRL_SINGLE (1u << 5) /* one shot; 0 is continuous */

static uint32_t rd(const struct pwm_jh7110 *pwm, uint32_t channel, uint32_t reg) {
    return pwm->regs[(channel * CHANNEL_STRIDE + reg) / 4u];
}

static void wr(struct pwm_jh7110 *pwm, uint32_t channel, uint32_t reg, uint32_t v) {
    pwm->regs[(channel * CHANNEL_STRIDE + reg) / 4u] = v;
}

int pwm_jh7110_init(struct pwm_jh7110 *pwm, const struct fdt *fdt, const char *path) {
    phys_t base = 0;
    uint64_t size = 0;
    if (pwm == NULL) {
        return KERR_INVAL;
    }
    pwm->up = false;
    if (fdt_device_reg(fdt, path, 0, &base, &size) != KERR_OK) {
        return KERR_NOENT;
    }
    (void)crg_enable_device(fdt, path);
    pwm->regs = mmu_map_device(base, size);
    if (pwm->regs == NULL) {
        return KERR_NOMEM;
    }
    pwm->up = true;
    return KERR_OK;
}

int pwm_jh7110_start(struct pwm_jh7110 *pwm, uint32_t channel, uint32_t period_ticks,
                     uint32_t high_ticks) {
    if (!pwm->up) {
        return KERR_NODEV;
    }
    if (channel >= PWM_CHANNELS || period_ticks == 0) {
        return KERR_INVAL;
    }
    /* **Clamped, not passed through.** A high time at or past the reload
     * point means the counter never reaches it, and the output stays at
     * whatever it was — so full scale would read as "stuck" rather than as
     * "always on". One tick below the period is the honest encoding. */
    if (high_ticks >= period_ticks) {
        high_ticks = period_ticks - 1u;
    }
    /* Stopped while reprogrammed: writing the reload value of a running
     * counter can leave it above the new period, and it then counts the
     * whole way round before the first correct cycle. */
    wr(pwm, channel, REG_CTRL, 0);
    wr(pwm, channel, REG_CNTR, 0);
    wr(pwm, channel, REG_HRC, high_ticks);
    wr(pwm, channel, REG_LRC, period_ticks);
    wr(pwm, channel, REG_CTRL, CTRL_EN | CTRL_OE);
    return KERR_OK;
}

int pwm_jh7110_stop(struct pwm_jh7110 *pwm, uint32_t channel) {
    if (!pwm->up) {
        return KERR_NODEV;
    }
    if (channel >= PWM_CHANNELS) {
        return KERR_INVAL;
    }
    wr(pwm, channel, REG_CTRL, 0);
    return KERR_OK;
}

uint32_t pwm_jh7110_counter(const struct pwm_jh7110 *pwm, uint32_t channel) {
    if (!pwm->up || channel >= PWM_CHANNELS) {
        return 0;
    }
    return rd(pwm, channel, REG_CNTR);
}
