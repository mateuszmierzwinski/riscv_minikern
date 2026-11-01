/* The EDID reader, fuzzed: 128 bytes from whatever answers on the DDC
 * line. Inputs are cut or padded to exactly one block in a buffer of that
 * size, so a read past it is a redzone hit. */
#include <stdlib.h>
#include <string.h>

#include "core/display_mode.h"
#include "fuzz.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint8_t *block = calloc(1, EDID_BLOCK_BYTES);
    struct edid_info info;
    if (block == NULL) {
        return 0;
    }
    memcpy(block, data, size < EDID_BLOCK_BYTES ? size : EDID_BLOCK_BYTES);
    (void)display_edid_parse(block, &info);
    free(block);
    return 0;
}
