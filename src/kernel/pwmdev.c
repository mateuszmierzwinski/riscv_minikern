#include "kernel/pwmdev.h"

#include <stdbool.h>
#include <stddef.h>

#include "core/errno.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/uspace.h"
#include "lib/kfmt.h"

#define CHANNELS_MAX 8u
#define PWM_IOCTL_REQUEST 0x7501u
#define PWM_IOCTL_FREE 0x7502u
#define PWM_IOCTL_ROUNDWF 0xc0207503u
#define PWM_IOCTL_GETWF 0xc0207504u
#define PWM_IOCTL_SETROUNDEDWF 0x40207505u
#define PWM_IOCTL_SETEXACTWF 0x40207506u

/* `struct pwmchip_waveform`: hwpwm, padding, then three u64 lengths. */
struct waveform {
    uint32_t hwpwm;
    uint32_t pad;
    uint64_t period_ns, duty_ns, offset_ns;
};

static struct {
    struct pwm_port port;
    struct waveform set[CHANNELS_MAX];
    const struct file *held[CHANNELS_MAX]; /* which descriptor requested each channel */
    char path[20];
    bool up;
} chip;

/* A waveform from the user, checked against the channels and the shapes
 * the hardware has: no phase offset, duty within the period. */
static int32_t take_waveform(virt_t arg, struct waveform *w) {
    if (uspace_copy_in(w, arg, sizeof *w) != KERR_OK) {
        return KERR_FAULT;
    }
    if (w->hwpwm >= chip.port.channels || w->offset_ns != 0 || w->duty_ns > w->period_ns) {
        return KERR_INVAL;
    }
    return KERR_OK;
}

/* Sets a channel the caller holds; `exact` refuses a waveform that would
 * round to something else. */
static int32_t set_waveform(const struct file *f, virt_t arg, bool exact) {
    struct waveform w, rounded;
    int32_t rc = take_waveform(arg, &w);
    if (rc != KERR_OK) {
        return rc;
    }
    if (chip.held[w.hwpwm] != f) {
        return KERR_BUSY; /* not requested by this descriptor */
    }
    rounded = w;
    chip.port.round(chip.port.self, &rounded.period_ns, &rounded.duty_ns);
    if (exact && (rounded.period_ns != w.period_ns || rounded.duty_ns != w.duty_ns)) {
        return KERR_DOM;
    }
    rc = chip.port.apply(chip.port.self, w.hwpwm, rounded.period_ns, rounded.duty_ns);
    if (rc == KERR_OK) {
        chip.set[w.hwpwm] = rounded;
    }
    return rc;
}

/* ROUNDWF and GETWF: a waveform written back — what it would become, or
 * what the channel has. */
static int32_t answer_waveform(virt_t arg, bool current) {
    struct waveform w;
    int32_t rc = take_waveform(arg, &w);
    if (rc != KERR_OK && !(current && rc == KERR_INVAL)) {
        return rc;
    }
    if (current) {
        if (w.hwpwm >= chip.port.channels) {
            return KERR_INVAL;
        }
        w = chip.set[w.hwpwm];
    } else {
        chip.port.round(chip.port.self, &w.period_ns, &w.duty_ns);
    }
    return uspace_copy_out(arg, &w, sizeof w) == KERR_OK ? 0 : KERR_FAULT;
}

/* REQUEST and FREE: a channel belongs to one descriptor at a time. */
static int32_t request_or_free(const struct file *f, uint32_t request, uint64_t channel) {
    if (channel >= chip.port.channels) {
        return KERR_INVAL;
    }
    if (request == PWM_IOCTL_REQUEST) {
        if (chip.held[channel] != NULL && chip.held[channel] != f) {
            return KERR_BUSY;
        }
        chip.held[channel] = f;
        return 0;
    }
    if (chip.held[channel] != f) {
        return KERR_INVAL;
    }
    chip.held[channel] = NULL;
    return 0;
}

static int32_t pwm_ioctl(struct file *f, uint32_t request, virt_t arg) {
    switch (request) {
    case PWM_IOCTL_REQUEST:
    case PWM_IOCTL_FREE:
        return request_or_free(f, request, arg);
    case PWM_IOCTL_ROUNDWF:
        return answer_waveform(arg, false);
    case PWM_IOCTL_GETWF:
        return answer_waveform(arg, true);
    case PWM_IOCTL_SETROUNDEDWF:
        return set_waveform(f, arg, false);
    case PWM_IOCTL_SETEXACTWF:
        return set_waveform(f, arg, true);
    default:
        return KERR_NOTTY;
    }
}

/* A descriptor that goes frees what it requested; the waveforms stay, as
 * on Linux, where a freed channel keeps running until someone changes it. */
static void pwm_release(struct file *f) {
    for (uint32_t c = 0; c < CHANNELS_MAX; c++) {
        chip.held[c] = chip.held[c] == f ? NULL : chip.held[c];
    }
}

static const struct file_ops pwm_ops = {
    .ioctl = pwm_ioctl,
    .release = pwm_release,
};

int pwmdev_install(uint32_t number, const struct pwm_port *port) {
    static const struct device_owner root_only = {0600u, 0u, 0u};
    int rc;
    if (chip.up || port->channels == 0 || port->channels > CHANNELS_MAX) {
        return KERR_INVAL;
    }
    chip.port = *port;
    (void)kfmt_snprintf(chip.path, sizeof chip.path, "/dev/pwmchip%u", number);
    rc = fdobjects_add_device(chip.path, &pwm_ops, NULL, &root_only);
    if (rc != KERR_OK) {
        return rc;
    }
    (void)synthfs_add_device(chip.path, &root_only);
    chip.up = true;
    return KERR_OK;
}
