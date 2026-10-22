#include "core/elf.h"

#include <stdbool.h>

#include "core/errno.h"

/* Field offsets and values are the ELF32 ABI (System V gABI, ARM EABI). */
#define EH_SIZE      52u
#define PH_SIZE      32u
#define ET_EXEC      2u
#define EM_ARM       40u
#define PT_LOAD      1u
#define PT_INTERP    3u
#define PT_PHDR      6u
#define PF_X         1u
#define PF_W         2u
#define PF_R         4u
#define ELF_MAX_PHNUM 64u

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static bool fits(uint32_t off, uint32_t n, size_t len) {
    return off <= len && n <= len - off;
}

/* The identification bytes and the fixed header fields. */
static bool is_arm_exec(const uint8_t *d, size_t len) {
    return len >= EH_SIZE && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' && d[3] == 'F' && d[4] == 1 &&
           d[5] == 1 && d[6] == 1 && le16(d + 16) == ET_EXEC && le16(d + 18) == EM_ARM;
}

static uint32_t prot_of(uint32_t flags) {
    return ((flags & PF_R) ? 1u : 0u) | ((flags & PF_W) ? 2u : 0u) | ((flags & PF_X) ? 4u : 0u);
}

/* Records one PT_LOAD after checking it against the file and user_top. */
static int add_load(const uint8_t *ph, size_t len, uint32_t user_top, struct elf_image *out) {
    struct elf_load l = {.offset = le32(ph + 4), .vaddr = le32(ph + 8), .filesz = le32(ph + 16),
                         .memsz = le32(ph + 20), .prot = prot_of(le32(ph + 24))};
    if (out->nloads == ELF_MAX_LOADS || l.filesz > l.memsz || !fits(l.offset, l.filesz, len) ||
        l.memsz > user_top || l.vaddr > user_top - l.memsz || l.vaddr < 0x1000u ||
        (l.offset % 4096u) != (l.vaddr % 4096u)) {
        return KERR_INVAL;
    }
    out->loads[out->nloads++] = l;
    return KERR_OK;
}

/* AT_PHDR: PT_PHDR's address if present, else where the PT_LOAD that
 * covers the program headers' file offset puts them. 0 when neither. */
static uint32_t phdr_address(const struct elf_image *img, uint32_t phoff, uint32_t phdr_seg) {
    if (phdr_seg != 0) {
        return phdr_seg;
    }
    for (uint32_t i = 0; i < img->nloads; i++) {
        const struct elf_load *l = &img->loads[i];
        if (phoff >= l->offset && phoff - l->offset < l->filesz) {
            return l->vaddr + (phoff - l->offset);
        }
    }
    return 0;
}

/* Walks the program headers; PT_INTERP means a dynamic binary: refused. */
static int read_phdrs(const uint8_t *d, size_t len, uint32_t user_top, struct elf_image *out, uint32_t *phdr_seg) {
    uint32_t phoff = le32(d + 28);
    for (uint32_t i = 0; i < out->phnum; i++) {
        const uint8_t *ph = d + phoff + i * PH_SIZE;
        uint32_t type = le32(ph);
        int rc = KERR_OK;
        if (type == PT_INTERP) {
            return KERR_NOEXEC;
        }
        if (type == PT_PHDR) {
            *phdr_seg = le32(ph + 8);
        }
        if (type == PT_LOAD && le32(ph + 20) > 0) {
            rc = add_load(ph, len, user_top, out);
        }
        if (rc != KERR_OK) {
            return rc;
        }
    }
    return KERR_OK;
}

int elf_parse(const uint8_t *d, size_t len, uint32_t user_top, struct elf_image *out) {
    struct elf_image img = {0};
    uint32_t phoff, phdr_seg = 0;
    int rc;
    if (d == NULL || !is_arm_exec(d, len)) {
        return KERR_NOEXEC;
    }
    phoff = le32(d + 28);
    img = (struct elf_image){.entry = le32(d + 24), .phent = le16(d + 42), .phnum = le16(d + 44)};
    if (img.phent != PH_SIZE || img.phnum == 0 || img.phnum > ELF_MAX_PHNUM ||
        !fits(phoff, img.phnum * PH_SIZE, len)) {
        return KERR_INVAL;
    }
    rc = read_phdrs(d, len, user_top, &img, &phdr_seg);
    if (rc != KERR_OK || img.nloads == 0 || img.entry >= user_top) {
        return rc != KERR_OK ? rc : KERR_INVAL;
    }
    img.phdr_vaddr = phdr_address(&img, phoff, phdr_seg);
    *out = img;
    return KERR_OK;
}
