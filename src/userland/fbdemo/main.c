/* A picture on /dev/fb0 for a person to look at: the check a log cannot do.
 *
 * Eight colour bars across the top half (white, yellow, cyan, green,
 * magenta, red, blue, black — the order every test card uses, so a channel
 * swapped in the pixel format shows as wrong colours in a known place), a
 * grey ramp below them (a missing bit shows as banding), and a white
 * square moving across the bottom for `seconds` (argument 1, default 30):
 * if it moves, the display is following what programs draw, not a frame
 * captured once.
 *
 * Exits 0 after the time, 1 if the framebuffer cannot be mapped.
 */
#define _POSIX_C_SOURCE 200809L /* nanosleep: the toolchain compiles strict C17 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct fb_var_screeninfo {
    uint32_t xres, yres, rest[38];
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

static const uint32_t bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                                 0xff00ff, 0xff0000, 0x0000ff, 0x000000};

struct screen {
    uint8_t *pixels;
    uint32_t width, height, stride;
};

static void fill(const struct screen *s, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                 uint32_t colour) {
    for (uint32_t y = y0; y < y0 + h && y < s->height; y++) {
        uint32_t *row = (uint32_t *)(s->pixels + (size_t)y * s->stride);
        for (uint32_t x = x0; x < x0 + w && x < s->width; x++) {
            row[x] = colour;
        }
    }
}

static void draw_card(const struct screen *s) {
    uint32_t bar = s->width / 8u;
    for (uint32_t i = 0; i < 8u; i++) {
        fill(s, i * bar, 0, bar, s->height / 2u, bars[i]);
    }
    for (uint32_t x = 0; x < s->width; x++) {
        uint32_t level = x * 255u / (s->width - 1u);
        fill(s, x, s->height / 2u, 1, s->height / 4u, level << 16 | level << 8 | level);
    }
    fill(s, 0, s->height * 3u / 4u, s->width, s->height / 4u, 0x202020);
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* The square crosses the bottom band once every ~4 seconds. */
static void animate(const struct screen *s, int seconds) {
    uint32_t side = s->height / 8u;
    uint32_t y = s->height * 3u / 4u + (s->height / 4u - side) / 2u;
    uint32_t x = 0;
    for (int frame = 0; frame < seconds * 25; frame++) {
        fill(s, x, y, side, side, 0x202020);
        x = (x + s->width / 100u) % (s->width - side);
        fill(s, x, y, side, side, 0xffffff);
        sleep_ms(40);
    }
}

int main(int argc, char **argv) {
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    struct screen s;
    int seconds = argc > 1 ? atoi(argv[1]) : 30;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &var) != 0 ||
        ioctl(fd, FBIOGET_FSCREENINFO, &fix) != 0) {
        printf("fbdemo: /dev/fb0 is not there\n");
        return 1;
    }
    s.pixels = mmap(NULL, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s.pixels == MAP_FAILED) {
        printf("fbdemo: /dev/fb0 could not be mapped\n");
        return 1;
    }
    s.width = var.xres;
    s.height = var.yres;
    s.stride = fix.line_length;
    printf("fbdemo: %ux%u test card, moving square for %d s\n", s.width, s.height, seconds);
    draw_card(&s);
    animate(&s, seconds);
    printf("fbdemo: done\n");
    return 0;
}
