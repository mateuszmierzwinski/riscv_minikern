/* pwmdev.h — a PWM controller as `/dev/pwmchipN`, the character device
 * Linux 6.13 added (`include/uapi/linux/pwm.h`): a channel is requested,
 * given a waveform — period and duty in nanoseconds — and freed. It needs
 * no writable sysfs, which this kernel's synthetic files are not.
 *
 * Rounding is the hardware's: `PWM_IOCTL_ROUNDWF` says what a waveform
 * would become, `SETROUNDEDWF` applies that, `SETEXACTWF` refuses anything
 * that would not be exact. A period of 0 turns the channel off. A duty
 * offset (phase) is refused: this PWM has none. Root only, 0600.
 */
#ifndef MINIKERN_KERNEL_PWMDEV_H
#define MINIKERN_KERNEL_PWMDEV_H

#include <stdint.h>

/* The controller, through the composition root: `round` turns a wanted
 * period and duty (ns) into what the counter can do, `apply` sets a
 * channel to a rounded waveform (period 0 = off). */
struct pwm_port {
    uint32_t channels;
    void (*round)(void *self, uint64_t *period_ns, uint64_t *duty_ns);
    int (*apply)(void *self, uint32_t channel, uint64_t period_ns, uint64_t duty_ns);
    void *self;
};

int pwmdev_install(uint32_t number, const struct pwm_port *port);

#endif
