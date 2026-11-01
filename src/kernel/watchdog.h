/* watchdog.h — the board's watchdog, fed by the kernel or by a process.
 *
 * `minikern.watchdog=<seconds>` arms it and the kernel feeds it once a
 * second from the housekeeping beat — so a kernel that wedges on its lock,
 * which stops the beat, is reset rather than left hanging. Off unless asked:
 * an armed watchdog turns every pause in a debugging session into a reboot.
 *
 * `/dev/watchdog` (root, 0600) hands the feeding to a process, as Linux's
 * does: while it is open the process feeds it — any write, or
 * `WDIOC_KEEPALIVE` — and the kernel does not. Closing it after writing
 * `V` (the "magic close") hands the feeding back; closing without it means
 * the process died, so nothing feeds it and the board resets. One holder at
 * a time (`EBUSY`).
 */
#ifndef MINIKERN_KERNEL_WATCHDOG_H
#define MINIKERN_KERNEL_WATCHDOG_H

#include <stdint.h>

/* The hardware, through the composition root. `arm` starts the count with
 * the reset enabled; `feed` starts it again. Both take the timeout in
 * seconds. */
struct watchdog_port {
    int (*arm)(void *self, uint32_t seconds);
    int (*feed)(void *self, uint32_t seconds);
    int (*stop)(void *self);
    void *self;
};

/* Arms the watchdog for `seconds` and starts feeding it, then installs
 * `/dev/watchdog`. `starve_after_s`, when not 0, stops the kernel's feeding
 * that long after the start — the test that the reset is real
 * (`minikern.wdtstarve`). */
int watchdog_start(const struct watchdog_port *port, uint32_t seconds, uint32_t starve_after_s);

/* Disarms it, for an orderly power-off: on this board the power-off can
 * fail (no PMIC power register), leaving a halted kernel that an armed
 * watchdog would reset — a power-off that turns into a reboot. Linux stops
 * its watchdog on shutdown for the same reason. */
void watchdog_stop(void);

/* How many times it has been fed, by anyone. */
uint64_t watchdog_feeds(void);

#endif
