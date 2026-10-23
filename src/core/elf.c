#include "core/elf.h"

#include <stdbool.h>

#include "core/errno.h"

/* Field offsets and values are the ELF64 gABI. */
#define EH_SIZE 64u
#define PH_SIZE 56u
#define EM_RISCV 243u
#define ET_EXEC 2u
#define PT_LOAD 1u
#define PT_INTERP 3u
#define PT_PHDR 6u
#define PF_X 0x1u
#define PF_W 0x2u
#define PF_R 0x4u
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u

/* Header offsets. */
#define EH_TYPE 16u
#define EH_MACHINE 18u
#define EH_ENTRY 24u
#define EH_PHOFF 32u
#define EH_PHENTSIZE 54u
#define EH_PHNUM 56u

/* Program-header offsets. **p_flags is at 4 here**, where ELF32 keeps it at
 * 24 — the single most error-prone difference between the two formats. */
#define PH_TYPE 0u
#define PH_FLAGS 4u
#define PH_OFFSET 8u
#define PH_VADDR 16u
#define PH_FILESZ 32u
#define PH_MEMSZ 40u

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t le64(const uint8_t *p) {
    return (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
}

/* True when [off, off+n) lies inside len, without overflowing on the way. */
static bool fits(uint64_t off, uint64_t n, size_t len) {
    return off <= len && n <= (uint64_t)len - off;
}

/* The magic, the class, the byte order, the type and the machine. All five,
 * because a file that is an ELF for something else is a more likely mistake
 * than a file that is not an ELF at all. */
static bool is_ours(const uint8_t *d) {
    return d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F' && d[4] == ELFCLASS64 &&
           d[5] == ELFDATA2LSB && d[6] == 1 && le16(d + EH_TYPE) == ET_EXEC &&
           le16(d + EH_MACHINE) == EM_RISCV;
}

static uint32_t prot_of(uint32_t flags) {
    return ((flags & PF_R) != 0 ? PROT_READ : 0u) | ((flags & PF_W) != 0 ? PROT_WRITE : 0u) |
           ((flags & PF_X) != 0 ? PROT_EXEC : 0u);
}

/* Records one PT_LOAD, after checking it against the file and the address
 * space. A segment whose memory size is smaller than its file size, or that
 * reaches into the kernel's half, is a refusal. */
static int add_load(const uint8_t *ph, size_t len, virt_t user_top, struct elf_image *out) {
    struct elf_load l = {.offset = le64(ph + PH_OFFSET),
                         .vaddr = le64(ph + PH_VADDR),
                         .filesz = le64(ph + PH_FILESZ),
                         .memsz = le64(ph + PH_MEMSZ),
                         .prot = prot_of(le32(ph + PH_FLAGS))};
    if (out->nloads == ELF_MAX_LOADS) {
        return KERR_INVAL;
    }
    if (l.memsz < l.filesz || !fits(l.offset, l.filesz, len)) {
        return KERR_INVAL;
    }
    if (l.vaddr >= user_top || l.memsz > user_top - l.vaddr) {
        return KERR_INVAL;
    }
    out->loads[out->nloads++] = l;
    return KERR_OK;
}

/* Where the program headers land in memory, which is what AT_PHDR must
 * report. A linker usually covers them with the first loadable segment, and
 * the address is then that segment's start plus the header table's file
 * offset — but only when the table really is inside it, which is checked
 * rather than assumed. */
static virt_t phdr_address(const struct elf_image *img, uint64_t phoff, uint64_t table_bytes) {
    for (uint32_t i = 0; i < img->nloads; i++) {
        const struct elf_load *l = &img->loads[i];
        if (phoff >= l->offset && phoff + table_bytes <= l->offset + l->filesz) {
            return l->vaddr + (phoff - l->offset);
        }
    }
    return 0;
}

int elf_parse(const uint8_t *data, size_t len, virt_t user_top, struct elf_image *out) {
    uint64_t phoff;
    uint32_t phent, phnum;
    /* **The pointers first, before the length.**
     *
     * This used to begin `len < EH_SIZE || !is_ours(data)`, which reads the
     * file whenever the length is plausible — so a NULL `data` with any
     * length over 64 dereferenced it. In a kernel a fault is a panic, so
     * that is the machine stopping inside a parser's entry condition, from
     * one caller's mistake.
     *
     * No caller passes NULL today, which is the argument for leaving it out
     * and is not good enough: this function's whole job is to be total over
     * input it does not control, and its callers are the paths that grow.
     * Found by the Rule 25 suite on its first run. */
    if (data == NULL || out == NULL) {
        return KERR_INVAL;
    }
    if (len < EH_SIZE || !is_ours(data)) {
        return KERR_NOEXEC;
    }
    phoff = le64(data + EH_PHOFF);
    phent = le16(data + EH_PHENTSIZE);
    phnum = le16(data + EH_PHNUM);
    if (phent != PH_SIZE || phnum == 0 || !fits(phoff, (uint64_t)phent * phnum, len)) {
        return KERR_INVAL;
    }
    *out = (struct elf_image){.entry = le64(data + EH_ENTRY), .phent = phent, .phnum = phnum};
    for (uint32_t i = 0; i < phnum; i++) {
        const uint8_t *ph = data + phoff + (uint64_t)i * phent;
        uint32_t type = le32(ph + PH_TYPE);
        if (type == PT_INTERP) {
            return KERR_NOEXEC; /* dynamically linked: there is no loader here */
        }
        if (type == PT_LOAD) {
            int rc = add_load(ph, len, user_top, out);
            if (rc != KERR_OK) {
                return rc;
            }
        }
    }
    if (out->nloads == 0 || out->entry >= user_top) {
        return KERR_INVAL;
    }
    out->phdr_vaddr = phdr_address(out, phoff, (uint64_t)phent * phnum);
    return KERR_OK;
}
