/* elf — validates a static ARM executable and describes what to load.
 *
 * Accepts exactly what Go and TinyGo produce for linux/arm: ELF32,
 * little-endian, EM_ARM, ET_EXEC, statically linked (no PT_INTERP). Every
 * header field that steers loading is checked against the file's length
 * and the user address limit, because the file comes from a filesystem
 * image we did not build. The image itself is not copied. */
#ifndef MINIKERN_CORE_ELF_H
#define MINIKERN_CORE_ELF_H

#include <stddef.h>
#include <stdint.h>

#define ELF_MAX_LOADS 8u

/* One PT_LOAD: file bytes [offset, offset+filesz) land at vaddr; the rest
 * up to memsz is zero. prot is PROT_* from p_flags. */
struct elf_load {
    uint32_t vaddr;
    uint32_t memsz;
    uint32_t offset;
    uint32_t filesz;
    uint32_t prot;
};

struct elf_image {
    uint32_t entry;
    uint32_t phdr_vaddr;  /* AT_PHDR: where the program headers are in memory */
    uint32_t phent;       /* AT_PHENT */
    uint32_t phnum;       /* AT_PHNUM */
    uint32_t nloads;
    struct elf_load loads[ELF_MAX_LOADS];
};

/* Parses the len-byte file at data. Returns KERR_OK, KERR_NOEXEC for
 * something that is not an ELF executable for us, or KERR_INVAL when the
 * headers contradict the file or the address space (user_top). */
int elf_parse(const uint8_t *data, size_t len, uint32_t user_top, struct elf_image *out);

#endif
