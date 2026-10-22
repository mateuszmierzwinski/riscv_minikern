/* elf — validates a static RISC-V 64 executable and describes what to load.
 *
 * Accepts exactly what Go produces for `linux/riscv64`: ELF64,
 * little-endian, `EM_RISCV`, `ET_EXEC`, statically linked (no `PT_INTERP`).
 * Every header field that steers loading is checked against the file's
 * length and the top of user space, because the file comes from a
 * filesystem this kernel did not build — a program header that claims a
 * segment runs past the end of the file, or into the kernel's half, is a
 * refusal and not a surprise later.
 *
 * It is a rewrite rather than a widening of the ARM version. ELF64 is not
 * ELF32 with bigger fields: the header's layout differs, and the program
 * header's **flags move from the end to the front** — in ELF32 they follow
 * the sizes, in ELF64 they sit immediately after the type. Patching the
 * offsets would have read a segment's permissions out of its alignment.
 *
 * Pure: it reads the image and copies nothing.
 */
#ifndef MINIKERN_CORE_ELF_H
#define MINIKERN_CORE_ELF_H

#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

#define ELF_MAX_LOADS 8u

/* One PT_LOAD: file bytes [offset, offset+filesz) land at vaddr, and the
 * rest up to memsz is zero — which is how a .bss arrives. */
struct elf_load {
    virt_t vaddr;
    uint64_t memsz;
    uint64_t offset;
    uint64_t filesz;
    uint32_t prot;  /* PROT_READ/WRITE/EXEC, from p_flags */
};

struct elf_image {
    virt_t entry;
    virt_t phdr_vaddr;  /* AT_PHDR: where the program headers land in memory */
    uint32_t phent;     /* AT_PHENT */
    uint32_t phnum;     /* AT_PHNUM */
    uint32_t nloads;
    struct elf_load loads[ELF_MAX_LOADS];
};

#define PROT_READ 0x1u
#define PROT_WRITE 0x2u
#define PROT_EXEC 0x4u

/* Parses the len-byte file at data. KERR_OK, KERR_NOEXEC for something that
 * is not a static RISC-V 64 executable, or KERR_INVAL when the headers
 * contradict the file or the address space (`user_top`). */
int elf_parse(const uint8_t *data, size_t len, virt_t user_top, struct elf_image *out);

#endif
