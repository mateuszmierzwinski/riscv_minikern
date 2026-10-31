/* The first musl program: proof the C library starts and talks to the
 * kernel — stdio through `writev`, `malloc` through `brk`/`mmap`, and the
 * soft-float `long double` path of `strtold`.
 *
 * Exits 0 when every check passed; otherwise the bitmask of the failed
 * ones, so the board log names what broke:
 *   1  printf/snprintf formatted wrong
 *   2  malloc of 1 MiB returned NULL or did not hold a pattern
 *   4  strtold("1.5") was not 1.5
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int formats(void) {
    char text[64];
    snprintf(text, sizeof text, "%d-%s-%.2f", 42, "musl", 2.5);
    return strcmp(text, "42-musl-2.50") == 0 ? 0 : 1;
}

static int allocates(void) {
    size_t bytes = 1u << 20;
    unsigned char *p = malloc(bytes);
    if (p == NULL) {
        return 2;
    }
    for (size_t i = 0; i < bytes; i++) {
        p[i] = (unsigned char)(i * 7u);
    }
    for (size_t i = 0; i < bytes; i++) {
        if (p[i] != (unsigned char)(i * 7u)) {
            free(p);
            return 2;
        }
    }
    free(p);
    return 0;
}

static int parses_long_double(void) {
    return strtold("1.5", NULL) == 1.5L ? 0 : 4;
}

int main(void) {
    int failed = formats() | allocates() | parses_long_double();
    printf("musl_hello: %s (failed mask %d)\n", failed == 0 ? "all checks passed" : "FAILED",
           failed);
    return failed;
}
