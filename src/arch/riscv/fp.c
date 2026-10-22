#include "arch/riscv/fp.h"

/* `sstatus.FS`: 00 Off, 01 Initial, 10 Clean, 11 Dirty. Anything but Off
 * permits floating-point instructions; Initial is the state a thread starts
 * in and is enough. */
#define SSTATUS_FS_INITIAL (1ull << 13)

void fp_enable(void) {
    __asm__ volatile("csrs sstatus, %0" : : "r"(SSTATUS_FS_INITIAL));
}
