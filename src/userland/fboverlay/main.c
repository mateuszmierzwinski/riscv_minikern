/* The display's overlay plane from a program (`MINIKERN_FBIO_OVERLAY`,
 * ROADMAP §10 acceleration): map /dev/fb0's overlay area, draw a
 * 128×128 four-colour card there, and have the controller show it at
 * 64,64, scaled to 256×256 — no copy, no CPU scaling.
 *
 * Whether the controller fetches it is the kernel's to report (with
 * `minikern.overlaycheck`); this program checks the ioctl's contract. The
 * exit status is a bitmask of the checks that **passed**:
 *   1   the card shown: 0
 *   2   a picture reaching past the overlay area is refused: EINVAL
 *   4   a request at a bad pointer is refused: EFAULT
 *   8   taken down again (width 0): 0
 * 15 is all four. It then shows the card once more and exits without
 * taking it down: closing the descriptor must do that (the kernel logs it).
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_EXIT_GROUP 94
#define SYS_NANOSLEEP 101
#define SYS_MMAP 222
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define PROT_RW 3L
#define MAP_SHARED 1L
#define EINVAL 22
#define EFAULT 14
#define MINIKERN_FBIO_OVERLAY 0x40204641L
#define OVERLAY_AREA (4u * 1024u * 1024u)
#define OVERLAY_MMAP_OFFSET 0x40000000L /* where /dev/fb0 maps its overlay area */
#define CARD 128u

struct fb_overlay {
    uint32_t offset, stride, width, height;
    uint32_t x, y, out_width, out_height;
};

static long syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7) : "memory");
    return a0;
}

/* Red, green, blue and white quadrants, opaque. */
static void draw_card(uint32_t *px) {
    static const uint32_t colours[4] = {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu};
    for (uint32_t y = 0; y < CARD; y++) {
        for (uint32_t x = 0; x < CARD; x++) {
            px[y * CARD + x] = colours[(y >= CARD / 2u ? 2u : 0u) + (x >= CARD / 2u ? 1u : 0u)];
        }
    }
}

static long overlay(long fd, const struct fb_overlay *o) {
    return syscall6(SYS_IOCTL, fd, MINIKERN_FBIO_OVERLAY, (long)o, 0, 0, 0);
}

void user_main(void) {
    long fd = syscall6(SYS_OPENAT, AT_FDCWD, (long)"/dev/fb0", O_RDWR, 0, 0, 0);
    uint32_t passed = 0;
    long base = fd >= 0 ? syscall6(SYS_MMAP, 0, OVERLAY_AREA, PROT_RW, MAP_SHARED, fd, OVERLAY_MMAP_OFFSET) : -1;
    struct fb_overlay card = {0, CARD * 4u, CARD, CARD, 64u, 64u, 2u * CARD, 2u * CARD};
    struct fb_overlay past = card, off = {0};
    long ts[2] = {0, 500000000L};
    if (base < 0 && base > -4096) {
        (void)syscall6(SYS_EXIT_GROUP, 64, 0, 0, 0, 0, 0); /* no mapping: nothing to show */
    }
    draw_card((uint32_t *)base);
    past.offset = OVERLAY_AREA - CARD * 4u * (CARD - 1u);
    passed |= overlay(fd, &card) == 0 ? 1u : 0u;
    passed |= overlay(fd, &past) == -EINVAL ? 2u : 0u;
    passed |= overlay(fd, (const struct fb_overlay *)8L) == -EFAULT ? 4u : 0u;
    (void)syscall6(SYS_NANOSLEEP, (long)ts, 0, 0, 0, 0, 0);
    passed |= overlay(fd, &off) == 0 ? 8u : 0u;
    (void)overlay(fd, &card);
    (void)syscall6(SYS_EXIT_GROUP, (long)passed, 0, 0, 0, 0, 0);
}
