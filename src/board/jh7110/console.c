#include "board/jh7110/console.h"

#include <stddef.h>

#include "arch/riscv/sbi.h"

static void sbi_write(void *self, const char *data, size_t len) {
    (void)self;
    for (size_t i = 0; i < len; i++) {
        /* A bare newline is what the kernel emits; a terminal wants both
         * characters, and the firmware does not add them. */
        if (data[i] == '\n') {
            sbi_console_putchar('\r');
        }
        sbi_console_putchar(data[i]);
    }
}

struct console_port jh7110_sbi_console(void) {
    struct console_port p = {.write = sbi_write, .self = NULL};
    return p;
}
