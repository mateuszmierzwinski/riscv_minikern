/* ustack — the state a program starts in.
 *
 * A Linux process does not begin with empty registers: it begins with a
 * stack that already holds its arguments, its environment and a table of
 * facts about the machine, and a C runtime reads all of it before `main`.
 * Getting this wrong produces a program that starts and then does something
 * inexplicable — on the architecture this migrated from, the very first bug
 * found in user mode was here, and it had nothing to do with the loader.
 *
 * The layout, growing **down** from the top of the stack, with the stack
 * pointer left at `argc`:
 *
 *     sp ->  argc                      (one 64-bit word)
 *            argv[0..argc-1], NULL
 *            envp[0..envc-1], NULL
 *            auxv pairs, terminated by AT_NULL
 *            ... the strings themselves, and AT_RANDOM's bytes
 *
 * Everything is a 64-bit word here, where the ARM version used 32 — which
 * is most of why this is a separate implementation rather than a widening:
 * a pointer-sized word is the unit the whole structure is counted in.
 *
 * Pure: it fills a buffer the caller owns, and reports where the stack
 * pointer should be. Nothing here knows what a page is.
 */
#ifndef MINIKERN_CORE_USTACK_H
#define MINIKERN_CORE_USTACK_H

#include <stdint.h>

#include "core/types.h"

/* The auxiliary-vector tags this kernel supplies. */
#define AT_NULL 0u
#define AT_PHDR 3u
#define AT_PHENT 4u
#define AT_PHNUM 5u
#define AT_PAGESZ 6u
#define AT_BASE 7u
#define AT_FLAGS 8u
#define AT_ENTRY 9u
#define AT_UID 11u
#define AT_EUID 12u
#define AT_GID 13u
#define AT_EGID 14u
#define AT_HWCAP 16u
#define AT_CLKTCK 17u
#define AT_RANDOM 25u
#define AT_HWCAP2 26u
#define AT_EXECFN 31u

#define USTACK_MAX_AUXV 20u

struct auxv_pair {
    uint64_t tag;
    uint64_t value;
};

/* What the program should find when it looks. */
struct ustack_args {
    const char *const *argv;
    uint32_t argc;
    const char *const *envp;
    uint32_t envc;
    const struct auxv_pair *auxv;  /* AT_RANDOM and AT_EXECFN are added here */
    uint32_t nauxv;
    uint8_t random[16];            /* AT_RANDOM: sixteen bytes a runtime seeds from */
    const char *execfn;            /* AT_EXECFN: the path it was run from */
};

/* Builds the block into `buf`, which will be mapped so that its last byte
 * is at `top - 1`. Reports the stack pointer the program must start with,
 * and how many bytes were used.
 *
 * KERR_NOMEM when it does not fit, rather than a truncated structure: a
 * runtime that reads one word past the end of a half-built auxiliary vector
 * does something undefined, and finding out which is not worth the
 * convenience. */
int ustack_build(uint8_t *buf, uint32_t len, virt_t top, const struct ustack_args *a, virt_t *sp,
                 uint32_t *used);

#endif
