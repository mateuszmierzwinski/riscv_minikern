/* The trap frame's layout, shared by the entry code and the C that reads it.
 *
 * Offsets rather than a struct, because a .S file goes through the
 * preprocessor and not a C parser. `struct trapframe` in trap.h must match
 * these, and the static assertions there are what keep them honest. */
#ifndef MINIKERN_ARCH_RISCV_FRAME_H
#define MINIKERN_ARCH_RISCV_FRAME_H

#define FRAME_X(n) ((n) * 8)
#define FRAME_SEPC (32 * 8)
#define FRAME_SSTATUS (33 * 8)
#define FRAME_SCAUSE (34 * 8)
#define FRAME_STVAL (35 * 8)
#define FRAME_SIZE (36 * 8)

#endif
