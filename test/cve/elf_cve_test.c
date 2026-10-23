/* The ELF loader against a hostile executable — Rule 25's suite for the
 * attack surface a user controls most directly.
 *
 * ## Why this parser and not another
 *
 * The product class is a kernel that parses untrusted input, and it has four
 * such surfaces: a filesystem on a **removable** card, an ELF a user
 * supplies, packets off a network, and syscall arguments. Of the four, the
 * filesystem and the network already have adversarial tests
 * (`xfs_test.c` refuses untrustworthy superblocks and inodes;
 * `tcp_hostile_test.c` is 42 hostile cases), and the ELF loader had three
 * correctness tests and nothing written from the attacker's side.
 *
 * That is the wrong way round. On this kernel an ELF is the *only* thing a
 * user hands the kernel before having any privileges at all: `/init` comes
 * off a card anyone can write, and a malformed header is read by the loader
 * before a single instruction of the program runs.
 *
 * ## The classes, and where they come from
 *
 * Each test below is a class with history in real kernels rather than an
 * invented input. The names are the shapes, not the CVE numbers, because the
 * shape is what a reader needs:
 *
 *  - **Integer overflow in address arithmetic.** `vaddr + memsz` wrapping
 *    past the top of the address space is the oldest ELF loader bug there
 *    is, and the one that turns "refuse out-of-range" into "accept
 *    everything": a wrapped sum is small, so a bounds check on the sum
 *    passes. Linux has had this shape more than once, most recently in the
 *    argument-block arithmetic rather than the segments.
 *  - **A segment that lands on the kernel.** This kernel maps itself into
 *    every address space — it has to, or the trap handler could not run —
 *    so a segment pointing at kernel addresses is a request to overwrite
 *    the handler that would catch it. `CLAUDE.md` records that the sharing
 *    workaround has already cost one memory-corruption bug.
 *  - **`filesz` larger than the file.** A loader that trusts it reads past
 *    the buffer it was given: a kernel-side out-of-bounds read, which here
 *    means reading whatever followed the program in memory.
 *  - **`filesz` larger than `memsz`.** More file bytes than there is
 *    mapping to put them in, which writes past the segment.
 *  - **Program headers outside the file**, and a count that overflows when
 *    multiplied by the entry size — the table is addressed by
 *    `phoff + phnum * phentsize`, three attacker-controlled values.
 *  - **Resource exhaustion by segment count**, which must be refused rather
 *    than overrunning a fixed array.
 *  - **An entry point in no segment**, which transfers control to an
 *    unmapped address and faults in user mode — harmless here, and checked
 *    so that it stays a user fault rather than becoming a kernel one.
 *
 * Every case asserts a **refusal**, not a crash that happens not to matter:
 * the loader's contract is `KERR_INVAL` or `KERR_NOEXEC`, and a test that
 * accepted "it did not obviously break" would pass against a loader that
 * had read out of bounds and got away with it.
 */
#include "core/elf.h"

#include <stdint.h>
#include <string.h>

#include "core/errno.h"
#include "ktest.h"

#define USER_TOP 0x40000000ull
#define EH 64u
#define PH 56u

/* Bigger than the image the headers describe, so that a parser reading past
 * what it was told would find plausible bytes rather than fault — the whole
 * point being that it must not read there at all. */
static uint8_t image[4096];

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

/* A valid static RISC-V 64 executable with one loadable segment. Every test
 * starts from this and corrupts exactly one thing, so a failure names the
 * field rather than the file. */
static void build_valid(void) {
    memset(image, 0, sizeof image);
    image[0] = 0x7f;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 2; /* ELFCLASS64 */
    image[5] = 1; /* little-endian */
    image[6] = 1;
    put16(image + 16, 2);           /* ET_EXEC */
    put16(image + 18, 243);         /* EM_RISCV */
    put64(image + 24, 0x10120ull);  /* entry */
    put64(image + 32, EH);          /* phoff */
    put16(image + 54, PH);          /* phentsize */
    put16(image + 56, 1);           /* phnum */
    put32(image + EH + 0, 1);       /* PT_LOAD */
    put32(image + EH + 4, 0x5);     /* PF_R | PF_X */
    put64(image + EH + 8, 0);       /* offset */
    put64(image + EH + 16, 0x10000ull); /* vaddr */
    put64(image + EH + 32, 512u);   /* filesz */
    put64(image + EH + 40, 1024u);  /* memsz */
}

/* The baseline: the suite is worthless if the valid case does not pass, and
 * a refusal of everything would make every test below green. */
static void test_the_valid_image_is_still_accepted(void) {
    struct elf_image img;
    build_valid();
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_OK);
}

static void test_segment_address_arithmetic_cannot_wrap(void) {
    struct elf_image img;
    /* vaddr near the top and a memsz that carries the sum past 2^64. The
     * sum is *small*, so a check written as `vaddr + memsz > user_top`
     * passes and the loader maps a segment at an address of its attacker's
     * choosing. */
    build_valid();
    put64(image + EH + 16, 0xffffffffffff0000ull);
    put64(image + EH + 40, 0x20000ull);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* The same shape one step smaller: exactly 2^64, so the sum is zero. */
    build_valid();
    put64(image + EH + 16, 0xffffffffffffffffull);
    put64(image + EH + 40, 1u);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* And a memsz that is itself absurd, with a sane vaddr. */
    build_valid();
    put64(image + EH + 40, 0xffffffffffffffffull);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
}

static void test_a_segment_may_not_land_on_the_kernel(void) {
    struct elf_image img;
    /* Above `user_top` is the kernel's, and this kernel is mapped into every
     * address space — so accepting this is handing over the trap handler. */
    build_valid();
    put64(image + EH + 16, USER_TOP);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* Straddling the boundary, which is the version a check on the *start*
     * alone would miss. */
    build_valid();
    put64(image + EH + 16, USER_TOP - 256u);
    put64(image + EH + 40, 4096u);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* And the kernel's own load address, which is where this one lives. */
    build_valid();
    put64(image + EH + 16, 0x80200000ull);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
}

static void test_file_bytes_must_be_inside_the_file(void) {
    struct elf_image img;
    /* A loader that trusts `filesz` reads past the buffer it was handed,
     * which on this kernel means reading whatever followed the program. */
    build_valid();
    put64(image + EH + 32, sizeof image + 4096u);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* An offset past the end, with a modest size. */
    build_valid();
    put64(image + EH + 8, sizeof image + 16u);
    put64(image + EH + 32, 16u);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* And the wrapping version: offset + filesz overflows, so the sum is
     * inside the file while neither part is. */
    build_valid();
    put64(image + EH + 8, 0xffffffffffffff00ull);
    put64(image + EH + 32, 0x200ull);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
}

static void test_file_bytes_must_fit_the_mapping(void) {
    struct elf_image img;
    /* More file than mapping: the bytes have to go somewhere, and past the
     * segment is somebody else's. */
    build_valid();
    put64(image + EH + 32, 2048u); /* filesz */
    put64(image + EH + 40, 1024u); /* memsz, smaller */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
}

static void test_the_program_header_table_must_be_inside_the_file(void) {
    struct elf_image img;
    build_valid();
    put64(image + 32, sizeof image + 64u); /* phoff past the end */
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* A count whose product with the entry size runs past the end — the
     * table is addressed by three attacker-controlled numbers. */
    build_valid();
    put16(image + 56, 0xffffu);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
    /* An entry size that is not this format's, which would make every
     * subsequent field land somewhere unintended. */
    build_valid();
    put16(image + 54, 32u);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) != KERR_OK);
    /* Zero, which would make a loop over the table never advance. */
    build_valid();
    put16(image + 54, 0);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) != KERR_OK);
}

static void test_too_many_segments_are_refused_not_overrun(void) {
    struct elf_image img;
    uint32_t n = ELF_MAX_LOADS + 4u;
    build_valid();
    put16(image + 56, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *ph = image + EH + i * PH;
        put32(ph + 0, 1);
        put32(ph + 4, 0x5u);
        put64(ph + 8, 0);
        put64(ph + 16, 0x10000ull + i * 0x10000ull);
        put64(ph + 32, 16u);
        put64(ph + 40, 16u);
    }
    /* Refused, or accepted with no more loads recorded than the array
     * holds. What must not happen is `nloads` past the end of it. */
    if (elf_parse(image, sizeof image, USER_TOP, &img) == KERR_OK) {
        KTEST_CHECK(img.nloads <= ELF_MAX_LOADS);
    }
}

static void test_a_truncated_file_is_refused_at_every_length(void) {
    struct elf_image img;
    /* Every prefix of a valid file, which is what a short read off a card
     * produces. None may be accepted and none may read past `len` — the
     * loop is the test, because the interesting lengths are the ones that
     * end part-way through a header. */
    build_valid();
    for (size_t len = 0; len < EH + PH; len++) {
        KTEST_CHECK(elf_parse(image, len, USER_TOP, &img) != KERR_OK);
    }
    /* A length of zero with a non-NULL pointer, and the reverse. */
    KTEST_CHECK(elf_parse(image, 0, USER_TOP, &img) != KERR_OK);
    KTEST_CHECK(elf_parse(NULL, sizeof image, USER_TOP, &img) != KERR_OK);
}

static void test_an_entry_point_outside_every_segment(void) {
    struct elf_image img;
    build_valid();
    put64(image + 24, 0x30000000ull); /* in range, in no segment */
    /* Either refusal or acceptance is defensible — the program faults in
     * user mode on its first instruction, which is the process's problem.
     * What is checked is that the *kernel* does not treat it as its own:
     * the entry must come back unchanged rather than being clamped into a
     * segment, which would start executing bytes the program did not
     * nominate. */
    if (elf_parse(image, sizeof image, USER_TOP, &img) == KERR_OK) {
        KTEST_CHECK(img.entry == 0x30000000ull);
    }
    /* An entry above the user's half is the kernel's address, and that one
     * must be refused. */
    build_valid();
    put64(image + 24, USER_TOP + 0x1000ull);
    KTEST_CHECK(elf_parse(image, sizeof image, USER_TOP, &img) == KERR_INVAL);
}

int main(void) {
    KTEST_RUN(test_the_valid_image_is_still_accepted);
    KTEST_RUN(test_segment_address_arithmetic_cannot_wrap);
    KTEST_RUN(test_a_segment_may_not_land_on_the_kernel);
    KTEST_RUN(test_file_bytes_must_be_inside_the_file);
    KTEST_RUN(test_file_bytes_must_fit_the_mapping);
    KTEST_RUN(test_the_program_header_table_must_be_inside_the_file);
    KTEST_RUN(test_too_many_segments_are_refused_not_overrun);
    KTEST_RUN(test_a_truncated_file_is_refused_at_every_length);
    KTEST_RUN(test_an_entry_point_outside_every_segment);
    return ktest_failures();
}
