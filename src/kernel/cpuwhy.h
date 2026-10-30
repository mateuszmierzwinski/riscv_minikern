/* cpuwhy.h — where the processors' time goes, printed on the console every
 * ten seconds while the machine runs (`minikern.cpuwhy`).
 *
 * `/proc/stat` says a hart is busy; this says busy *doing what*: user time
 * by process, kernel time split into holding and waiting for the kernel
 * lock, and the syscalls and traps that brought each hart in. All figures
 * are the change since the previous report, so a steady machine reads the
 * same every time and a spike stands out. */
#ifndef MINIKERN_KERNEL_CPUWHY_H
#define MINIKERN_KERNEL_CPUWHY_H

#include <stdint.h>

/* Starts reporting from the housekeeping beat. `khz` gives the cycle
 * counter's rate, which turns the lock's cycle counts into time. */
void cpuwhy_install(uint32_t (*khz)(void));

#endif
