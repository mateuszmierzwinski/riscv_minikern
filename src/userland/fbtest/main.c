/* The virtual framebuffer, from a program's side (ROADMAP §10).
 *
 * Written as any Linux fbdev client is — open `/dev/fb0`, ask its geometry,
 * `mmap` it shared — against musl, with the fbdev structs declared here
 * because there are no kernel headers: their layout is the ABI under test.
 *
 * Each check is something a kernel could get wrong while drawing still
 * "worked": a mapping that is private (the device never sees the pixels), a
 * `fork` that makes the shared mapping copy-on-write, a second mapping of
 * different frames, a refusal that is accepted instead.
 *
 * The exit status is a bitmask of the checks that **passed** (255 = all):
 *   1   geometry: 32 bpp, line_length and smem_len cover the visible area
 *   2   pixels drawn through the mapping are what `pread` reads back
 *   4   pixels written with `pwrite` appear in the mapping
 *   8   after `fork`, the child's drawing is visible to the parent
 *   16  a second open and mapping shows the same pixels
 *   32  refusals: shared write on a read-only open, MAP_SHARED anonymous,
 *       a mapping past the end, a mode change
 *   64  `mprotect` after `fork` keeps a private page private
 *   128 the pixels outlive `munmap`
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

struct fb_bitfield {
    uint32_t offset, length, msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset, bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags, pixclock, left_margin, right_margin,
        upper_margin, lower_margin, hsync_len, vsync_len, sync, vmode, rotate, colorspace,
        reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

struct screen {
    int fd;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    uint8_t *pixels;
};

/* The pattern a pixel should hold: neither zero nor uniform, so a read of
 * the wrong frame or row cannot pass. */
static uint32_t pattern(uint32_t x, uint32_t y, uint32_t salt) {
    return 0xff000000u | ((x * 7u + salt) & 0xffu) << 16 | ((y * 13u) & 0xffu) << 8 |
           ((x ^ y ^ salt) & 0xffu);
}

static uint32_t *row(const struct screen *s, uint32_t y) {
    return (uint32_t *)(s->pixels + (size_t)y * s->fix.line_length);
}

static void draw(const struct screen *s, uint32_t salt) {
    for (uint32_t y = 0; y < s->var.yres; y++) {
        for (uint32_t x = 0; x < s->var.xres; x++) {
            row(s, y)[x] = pattern(x, y, salt);
        }
    }
}

static int open_screen(struct screen *s, int flags) {
    s->fd = open("/dev/fb0", flags);
    if (s->fd < 0 || ioctl(s->fd, FBIOGET_VSCREENINFO, &s->var) != 0 ||
        ioctl(s->fd, FBIOGET_FSCREENINFO, &s->fix) != 0) {
        return -1;
    }
    s->pixels = mmap(NULL, s->fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    return s->pixels == MAP_FAILED ? -1 : 0;
}

static int geometry_holds(const struct screen *s) {
    return s->var.bits_per_pixel == 32u && s->var.xres >= 16u && s->var.yres >= 16u &&
           s->fix.line_length >= s->var.xres * 4u &&
           s->fix.smem_len >= s->fix.line_length * s->var.yres && s->var.red.offset == 16u &&
           s->var.blue.offset == 0u;
}

/* Every row of the device, read with `pread`, against the pattern. */
static int device_holds(const struct screen *s, uint32_t salt) {
    static uint32_t line[4096];
    for (uint32_t y = 0; y < s->var.yres; y++) {
        off_t at = (off_t)y * s->fix.line_length;
        if (pread(s->fd, line, s->var.xres * 4u, at) != (ssize_t)(s->var.xres * 4u)) {
            return 0;
        }
        for (uint32_t x = 0; x < s->var.xres; x++) {
            if (line[x] != pattern(x, y, salt)) {
                return 0;
            }
        }
    }
    return 1;
}

static int pwrite_shows_in_mapping(const struct screen *s) {
    uint32_t line[64];
    uint32_t y = s->var.yres / 2u;
    for (uint32_t x = 0; x < 64u; x++) {
        line[x] = 0x00c0ffeeu + x;
    }
    if (pwrite(s->fd, line, sizeof line, (off_t)y * s->fix.line_length) != (ssize_t)sizeof line) {
        return 0;
    }
    return memcmp(row(s, y), line, sizeof line) == 0;
}

static int fork_shares_the_pixels(const struct screen *s) {
    int status = 0;
    pid_t child = fork();
    if (child == 0) {
        draw(s, 77u);
        _exit(0);
    }
    if (child < 0 || waitpid(child, &status, 0) != child || status != 0) {
        return 0;
    }
    return row(s, 3)[5] == pattern(5, 3, 77u) && device_holds(s, 77u);
}

static int second_mapping_agrees(const struct screen *s) {
    struct screen other;
    int same;
    if (open_screen(&other, O_RDWR) != 0) {
        return 0;
    }
    same = memcmp(other.pixels, s->pixels, s->fix.smem_len) == 0;
    row(&other, 1)[1] = 0x00123456u;
    same = same && row(s, 1)[1] == 0x00123456u;
    munmap(other.pixels, other.fix.smem_len);
    close(other.fd);
    return same;
}

static int refuses(const struct screen *s) {
    int ro = open("/dev/fb0", O_RDONLY);
    struct fb_var_screeninfo bigger = s->var;
    int ok = ro >= 0;
    ok = ok && mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0) == MAP_FAILED &&
         errno == EACCES;
    ok = ok && mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0) != MAP_FAILED;
    ok = ok && mmap(NULL, 4096, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED;
    ok = ok && mmap(NULL, s->fix.smem_len + 4096u, PROT_READ, MAP_SHARED, s->fd, 0) == MAP_FAILED;
    bigger.xres += 16u;
    ok = ok && ioctl(s->fd, FBIOPUT_VSCREENINFO, &bigger) != 0 && errno == EINVAL;
    ok = ok && ioctl(s->fd, FBIOPUT_VSCREENINFO, (void *)&s->var) == 0;
    if (ro >= 0) {
        close(ro);
    }
    return ok;
}

/* A private page, shared copy-on-write by `fork`, made writable again by the
 * child with `mprotect`: the child's store must stay the child's. */
static int mprotect_keeps_pages_private(void) {
    volatile uint32_t *page =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int status = 0;
    pid_t child;
    if (page == MAP_FAILED) {
        return 0;
    }
    page[0] = 0x11111111u;
    child = fork();
    if (child == 0) {
        mprotect((void *)page, 4096, PROT_READ);
        mprotect((void *)page, 4096, PROT_READ | PROT_WRITE);
        page[0] = 0x22222222u;
        _exit(page[0] == 0x22222222u ? 0 : 1);
    }
    if (child < 0 || waitpid(child, &status, 0) != child || status != 0) {
        return 0;
    }
    return page[0] == 0x11111111u;
}

static int survives_munmap(struct screen *s) {
    draw(s, 5u);
    if (munmap(s->pixels, s->fix.smem_len) != 0) {
        return 0;
    }
    return device_holds(s, 5u);
}

int main(void) {
    struct screen s;
    int passed = 0;
    if (open_screen(&s, O_RDWR) != 0) {
        printf("fbtest: /dev/fb0 could not be opened and mapped (errno %d)\n", errno);
        return 0;
    }
    printf("fbtest: %ux%u, line %u, %u bytes\n", s.var.xres, s.var.yres, s.fix.line_length,
           s.fix.smem_len);
    passed |= geometry_holds(&s) ? 1 : 0;
    draw(&s, 1u);
    passed |= device_holds(&s, 1u) ? 2 : 0;
    passed |= pwrite_shows_in_mapping(&s) ? 4 : 0;
    passed |= fork_shares_the_pixels(&s) ? 8 : 0;
    passed |= second_mapping_agrees(&s) ? 16 : 0;
    passed |= refuses(&s) ? 32 : 0;
    passed |= mprotect_keeps_pages_private() ? 64 : 0;
    passed |= survives_munmap(&s) ? 128 : 0;
    printf("fbtest: passed mask %d\n", passed);
    return passed;
}
