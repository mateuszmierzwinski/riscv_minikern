/* The ELF64 parser, on the host, against hand-built headers.
 *
 * The point is what it **refuses**. A kernel loads a file it did not build,
 * so every header field that steers loading is a claim to be checked: a
 * segment that runs past the end of the file, one that reaches into the
 * kernel's half, a dynamically linked binary with no loader to help it.
 * Each of those, trusted, is a fault somewhere later with no obvious cause.
 */
#include "core/elf.h"

#include <string.h>

#include "core/errno.h"
#include "ktest.h"

#define USER_TOP 0x40000000ull

/* A minimal ELF64 for RISC-V, built field by field so that a test can
 * corrupt exactly one thing. */
#define EH 64u
#define PH 56u

static uint8_t image[1024];

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    for (uint32_t i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void put64(uint8_t *p, uint64_t v) {
    for (uint32_t i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

/* One loadable segment covering the headers and a little code. */
static void build_image(void) {
    memset(image, 0, sizeof image);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2;    /* ELFCLASS64 */
    image[5] = 1;    /* little-endian */
    image[6] = 1;    /* version */
    put16(image + 16, 2);    /* ET_EXEC */
    put16(image + 18, 243);  /* EM_RISCV */
    put64(image + 24, 0x10120ull); /* entry */
    put64(image + 32, EH);         /* phoff */
    put16(image + 54, PH);         /* phentsize */
    put16(image + 56, 1);          /* phnum */
    /* The segment: type PT_LOAD, flags R|X — and the flags are at offset 4,
     * which is where ELF64 keeps them and ELF32 does not. */
    put32(image + EH + 0, 1);
    put32(image + EH + 4, 0x5);          /* PF_R | PF_X */
    put64(image + EH + 8, 0);            /* offset */
    put64(image + EH + 16, 0x10000ull);  /* vaddr */
    put64(image + EH + 32, 512u);        /* filesz */
    put64(image + EH + 40, 1024u);       /* memsz: a .bss beyond the file */
}

static void test_reads_a_static_riscv_executable(void) {
    struct elf_image img;
    build_image();
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_OK);
    KTEST_CHECK(img.entry == 0x10120ull);
    KTEST_CHECK(img.nloads == 1u);
    KTEST_CHECK(img.loads[0].vaddr == 0x10000ull);
    KTEST_CHECK(img.loads[0].filesz == 512u && img.loads[0].memsz == 1024u);
    /* Read and execute, not write — the flags came from offset 4. */
    KTEST_CHECK(img.loads[0].prot == (PROT_READ | PROT_EXEC));
    /* The program headers are inside the segment, so their address is the
     * segment's start plus their file offset. */
    KTEST_CHECK(img.phdr_vaddr == 0x10000ull + EH);
    KTEST_CHECK(img.phent == PH && img.phnum == 1u);
}

/* Anything that is not a static RISC-V 64 executable. Each of these is a
 * more likely mistake than a file that is not an ELF at all: a 32-bit
 * binary, one for another architecture, an object file, a shared library. */
static void test_refuses_what_it_cannot_run(void) {
    struct elf_image img;
    build_image();
    image[4] = 1; /* ELFCLASS32 */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_NOEXEC);
    build_image();
    put16(image + 18, 40); /* EM_ARM — the architecture this came from */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_NOEXEC);
    build_image();
    put16(image + 16, 1); /* ET_REL: an object file */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_NOEXEC);
    build_image();
    image[5] = 2; /* big-endian */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_NOEXEC);
    build_image();
    put32(image + EH + 0, 3); /* PT_INTERP: dynamically linked */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_NOEXEC);
    /* And something that is not an ELF, including a file too short to tell. */
    memset(image, 0, sizeof image);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_NOEXEC);
    build_image();
    KTEST_CHECK(elf_parse(image, 16u, USER_TOP, &img) == KERR_NOEXEC);
}

/* Headers that contradict the file or the address space. These are the ones
 * that matter: trusted, they load a segment from outside the file or into
 * the kernel's half. */
static void test_refuses_headers_that_do_not_add_up(void) {
    struct elf_image img;
    build_image();
    put64(image + EH + 32, 4096u); /* filesz past the end of the image */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put64(image + EH + 8, 1020u);  /* offset + filesz past the end */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put64(image + EH + 40, 256u);  /* memsz smaller than filesz */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put64(image + EH + 16, USER_TOP - 128u); /* a segment reaching past user space */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put64(image + 24, USER_TOP + 8u);        /* an entry point in the kernel's half */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put16(image + 54, 32u);                  /* an ELF32-sized program header */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put64(image + 32, 1000u);                /* a header table past the end */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    build_image();
    put16(image + 56, 0);                    /* no program headers at all */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
}

int main(void) {
    KTEST_RUN(test_reads_a_static_riscv_executable);
    KTEST_RUN(test_refuses_what_it_cannot_run);
    KTEST_RUN(test_refuses_headers_that_do_not_add_up);
    return ktest_failures();
}
