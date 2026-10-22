/* The floating-point registers as thread state.
 *
 * `f0`..`f31` and `fcsr` belong to a thread exactly as the integer registers
 * do, and must be saved on a context switch and on signal delivery. The cost
 * of not doing it is in `fp.S`.
 */
#ifndef MINIKERN_ARCH_RISCV_FP_H
#define MINIKERN_ARCH_RISCV_FP_H

#include <stdint.h>

/* Thirty-two doubles then `fcsr`: the ABI's D-extension layout, which is
 * also what a signal frame's `sc_fpregs` holds. */
#define FP_AREA_BYTES 264u

/* Both take a pointer to `FP_AREA_BYTES` of 8-byte-aligned space. */
void fp_save(void *area);
void fp_restore(const void *area);

/* Lets this hart use floating point at all.
 *
 * `sstatus.FS` gates every floating-point instruction, in the kernel as well
 * as in user mode — so a kernel that means to *save* floating-point state
 * has to enable it for itself first, or the save instruction is the illegal
 * one. Each hart does it for itself, like everything else in `sstatus`. */
void fp_enable(void);

#endif
