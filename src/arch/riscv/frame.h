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

/* The kernel's own `tp` for the hart this thread last ran on.
 *
 * `tp` carries the hart id **inside the kernel**, which is how per-hart
 * state is indexed and how the kernel lock knows who holds it. But `tp` is
 * also the **user thread pointer**: a Go runtime keeps its own value there,
 * so after a trap from user mode the register holds the program's value, not
 * the kernel's. A kernel that read the hart id out of it would index its
 * per-hart arrays with whatever the thread happened to be using.
 *
 * So the kernel's value is kept in the frame, written on every return to
 * user mode and read back on every trap from it. The frame lives at a fixed
 * address for a given thread (`sscratch` is its stack top), so the value
 * survives from one trap to the next — and a thread that migrates to
 * another hart gets the new hart's id written on its way back out.
 *
 * 38 words rather than 37 to keep the frame 16-byte aligned, which the ABI
 * requires of any stack pointer. */
#define FRAME_KTP (36 * 8)
#define FRAME_SIZE (38 * 8)

#endif
