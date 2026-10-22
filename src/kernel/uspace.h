/* uspace — user mode: an address space of its own, and the way into it.
 *
 * This is the step everything else in the kernel exists for. A process has
 * its own Sv39 table with the kernel's half shared and its own half private,
 * and entering it is `sret` with `sstatus.SPP` cleared — one instruction,
 * where the architecture this migrated from needed a mode switch, a banked
 * register set and a stack per mode.
 *
 * **The kernel's half must be mapped in every address space.** When a trap
 * takes a user process into the kernel, translation does not change — the
 * same `satp` is still loaded — so the handler's code and stack have to be
 * addressable from where the process was. That is why the kernel is mapped
 * `PTE_G` (global, not flushed per address space) and why a process table
 * is built by copying the kernel's top-level entries rather than starting
 * empty.
 *
 * Nothing here loads an ELF yet: the first user program is a page of
 * instructions the kernel places, which is enough to prove the mode switch,
 * the trap path out of user mode and the syscall convention. An ELF loader
 * on top of a path that has never been entered would be two untested things
 * at once.
 */
#ifndef MINIKERN_KERNEL_USPACE_H
#define MINIKERN_KERNEL_USPACE_H

#include <stdbool.h>
#include <stdint.h>

#include "arch/riscv/pgtable.h"
#include "core/types.h"

/* Where a user program and its stack live. Low addresses, well inside the
 * bottom half, and nowhere near the kernel's. */
#define USER_TEXT 0x10000ull
#define USER_STACK_TOP 0x80000ull
#define USER_STACK_PAGES 4u

/* The top of user space: everything a program may address. Well below the
 * sign-extension hole, and far below anything the kernel maps. */
#define USER_TOP 0x40000000ull

/* The kernel registers that must survive a trip through user mode: the
 * return address, the stack pointer, and s0..s11 — exactly what a C caller
 * expects a call to preserve. */
#define USPACE_CONTEXT_WORDS 14u

/* One process: its address space, where it starts, and the kernel context
 * to come back to when it stops. */
struct uproc {
    struct pgtable pt;
    virt_t entry;
    virt_t stack_top;
    uint64_t context[USPACE_CONTEXT_WORDS];
    bool built;
};

/* Builds an address space with the kernel's mappings shared, `bytes` of
 * program copied in at USER_TEXT, and a stack. KERR_NOMEM when pages run
 * out. For the hand-written test programs: no headers, no start state. */
int uspace_build(struct uproc *p, const void *program, uint32_t bytes);

/* Builds an address space from an **ELF image**: every loadable segment at
 * the address its header names, with the permissions its flags ask for, a
 * stack, and the start state a C runtime expects — `argc`, `argv`, the
 * environment and an auxiliary vector.
 *
 * `argv[0]` and the path are what the program will see; the image itself is
 * not kept, so the caller may free it the moment this returns.
 *
 * KERR_NOEXEC for something that is not a static RISC-V 64 executable,
 * KERR_INVAL for headers that contradict the file, KERR_NOMEM when pages
 * run out. */
int uspace_build_elf(struct uproc *p, const uint8_t *image, uint32_t bytes, const char *path);

/* Releases the process's pages and its table. */
void uspace_destroy(struct uproc *p);

/* Enters the process. Returns when it exits — with the status it asked to
 * exit with, or a negative error when it faulted. */
int64_t uspace_run(struct uproc *p);

/* How many syscalls the running process has made, for a test to assert
 * against rather than trusting a single line of output. */
uint64_t uspace_syscalls(void);

#endif
