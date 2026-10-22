/* The four functions a C compiler may emit calls to even in freestanding code
 * (struct copies and initialisers). Kernel-only: a host build has libc's. */
#include <stddef.h>

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n-- > 0) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        return memcpy(dst, src, n);
    }
    while (n-- > 0) {
        d[n] = s[n];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (; n > 0; n--, x++, y++) {
        if (*x != *y) {
            return *x - *y;
        }
    }
    return 0;
}
