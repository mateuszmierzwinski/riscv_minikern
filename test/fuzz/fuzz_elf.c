/* The ELF loader's parser, fuzzed. A program's image is the most direct
 * input a user hands the kernel; `elf_parse` must refuse or describe it
 * without reading outside it, and a description it gives must stay inside
 * the user range it was given. */
#include <stdlib.h>

#include "core/elf.h"
#include "core/errno.h"
#include "fuzz.h"

#define USER_TOP 0x40000000ull

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    struct elf_image img;
    if (elf_parse(data, size, USER_TOP, &img) == KERR_OK && img.entry >= USER_TOP) {
        abort(); /* an accepted image may not start outside the user range */
    }
    return 0;
}
