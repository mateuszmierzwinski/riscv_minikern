/* fbinfo — a framebuffer's geometry, and how Linux's fbdev ABI describes it.
 *
 * `/dev/fb0` answers `FBIOGET_VSCREENINFO` and `FBIOGET_FSCREENINFO` with two
 * structs whose layout is user-visible ABI: a program built against Linux's
 * headers reads fields at fixed offsets, so the structs here are written out
 * field by field and their sizes pinned (160 and 80 bytes on LP64). Nothing
 * is copied from Linux; the layout is the interface, as for `stat`.
 *
 * One pixel format: 32 bits, XRGB8888 — blue in the low byte, red at bit 16,
 * the top byte unused. It is what a display controller scans out natively
 * and what every software renderer can write without conversion.
 *
 * Pure, so it is host-tested.
 */
#ifndef MINIKERN_CORE_FBINFO_H
#define MINIKERN_CORE_FBINFO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

#define FB_BYTES_PER_PIXEL 4u
#define FB_MIN_SIDE 16u
#define FB_MAX_SIDE 4096u
/* A line starts on a 64-byte boundary: the cache line, and the burst a
 * display controller fetches in. Every common width is already a multiple. */
#define FB_STRIDE_ALIGN 64u

/* The `ioctl` requests this kernel answers on a framebuffer. */
#define FBIOGET_VSCREENINFO 0x4600u
#define FBIOPUT_VSCREENINFO 0x4601u
#define FBIOGET_FSCREENINFO 0x4602u
#define FBIOPAN_DISPLAY 0x4606u
#define FBIOBLANK 0x4611u

struct fb_mode {
    uint32_t width;
    uint32_t height;
    uint32_t stride; /* bytes from one line's start to the next */
};

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height_mm;
    uint32_t width_mm;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

_Static_assert(sizeof(struct fb_var_screeninfo) == 160, "fb_var_screeninfo is ABI");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 80, "fb_fix_screeninfo is ABI");
_Static_assert(offsetof(struct fb_fix_screeninfo, line_length) == 48, "line_length is ABI");
_Static_assert(offsetof(struct fb_fix_screeninfo, mmio_start) == 56, "mmio_start is ABI");

/* Reads "<width>x<height>" (decimal, as on the command line) into a mode
 * with its stride. KERR_INVAL for anything else, or a side outside
 * [FB_MIN_SIDE, FB_MAX_SIDE] — a value from the command line is untrusted
 * input, and a typo must not size an allocation. */
int fbinfo_parse_mode(const char *text, size_t len, struct fb_mode *out);

/* The bytes the whole framebuffer occupies, rounded up to whole pages: what
 * `mmap` may map and what the device allocates. */
uint64_t fbinfo_bytes(const struct fb_mode *m);

/* The variable screen information for `m`: resolution, no panning area
 * beyond the visible one, XRGB8888. Timing fields are zero — a virtual
 * framebuffer has no timing, and a display driver that has one fills them. */
void fbinfo_describe_var(const struct fb_mode *m, struct fb_var_screeninfo *v);

/* The fixed screen information. `smem_start` is **zero on purpose**: it is a
 * physical address, which user space cannot use (it maps the device through
 * `mmap` of the descriptor) and should not learn. */
void fbinfo_describe_fix(const struct fb_mode *m, const char *id, struct fb_fix_screeninfo *f);

/* Whether a `FBIOPUT_VSCREENINFO` asks for exactly the geometry and format
 * the framebuffer already has — the only request this kernel can honour, as
 * it cannot change a mode. Anything else is refused rather than accepted and
 * ignored. */
bool fbinfo_var_matches(const struct fb_mode *m, const struct fb_var_screeninfo *v);

#endif
