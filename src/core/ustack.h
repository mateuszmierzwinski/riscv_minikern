/* ustack — the process start state on the user stack, Linux's layout
 * (an ABI fact the C runtime reads before main):
 *
 *   sp -> argc, argv[0..argc-1], NULL, envp[0..], NULL, auxv pairs, AT_NULL
 *         ... then the strings and AT_RANDOM's 16 bytes, up to the top.
 *
 * Built into a kernel buffer that stands for the top of the user stack,
 * then copied out; pointers inside it are user addresses. Pure and bounded,
 * so host-tested. */
#ifndef MINIKERN_CORE_USTACK_H
#define MINIKERN_CORE_USTACK_H

#include <stdint.h>

/* Auxiliary vector types (Linux include/uapi/linux/auxvec.h values). */
#define AT_NULL     0u
#define AT_PHDR     3u
#define AT_PHENT    4u
#define AT_PHNUM    5u
#define AT_PAGESZ   6u
#define AT_BASE     7u
#define AT_FLAGS    8u
#define AT_ENTRY    9u
#define AT_UID      11u
#define AT_EUID     12u
#define AT_GID      13u
#define AT_EGID     14u
#define AT_PLATFORM 15u
#define AT_HWCAP    16u
#define AT_CLKTCK   17u
#define AT_SECURE   23u
#define AT_RANDOM   25u
#define AT_HWCAP2   26u
#define AT_EXECFN   31u

#define USTACK_MAX_AUXV 24u
#define USTACK_MAX_ARGS 32u  /* argv and envp entries each */

struct auxv_pair {
    uint32_t type;
    uint32_t value;
};

struct ustack_args {
    const char *const *argv;
    uint32_t argc;
    const char *const *envp;
    uint32_t envc;
    const struct auxv_pair *auxv;  /* AT_RANDOM, AT_PLATFORM, AT_EXECFN are added here */
    uint32_t nauxv;
    uint8_t random[16];
    const char *platform;          /* "v7l" */
};

/* Lays the start state out in buf, which stands for the user addresses
 * [top - len, top). Returns KERR_OK with *sp (16-byte aligned) and the
 * number of bytes used from the top in *used, or KERR_RANGE when it does
 * not fit or there are too many arguments or auxv entries. */
int ustack_build(uint8_t *buf, uint32_t len, uint32_t top, const struct ustack_args *a, uint32_t *sp,
                 uint32_t *used);

#endif
