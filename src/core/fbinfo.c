#include "core/fbinfo.h"

#include "core/errno.h"

#define PAGE_BYTES 4096u

/* One decimal side of a mode, `*at` advanced past it. 0 when there are no
 * digits or the number outgrows FB_MAX_SIDE — the caller refuses 0. */
static uint32_t parse_side(const char *text, size_t len, size_t *at) {
    uint32_t value = 0;
    size_t start = *at;
    while (*at < len && text[*at] >= '0' && text[*at] <= '9') {
        value = value * 10u + (uint32_t)(text[*at] - '0');
        if (value > FB_MAX_SIDE) {
            return 0;
        }
        (*at)++;
    }
    return *at == start ? 0 : value;
}

static bool side_in_range(uint32_t side) {
    return side >= FB_MIN_SIDE && side <= FB_MAX_SIDE;
}

int fbinfo_parse_mode(const char *text, size_t len, struct fb_mode *out) {
    size_t at = 0;
    uint32_t width;
    uint32_t height;
    if (text == NULL || out == NULL) {
        return KERR_INVAL;
    }
    width = parse_side(text, len, &at);
    if (at >= len || text[at] != 'x') {
        return KERR_INVAL;
    }
    at++;
    height = parse_side(text, len, &at);
    if (at != len || !side_in_range(width) || !side_in_range(height)) {
        return KERR_INVAL;
    }
    out->width = width;
    out->height = height;
    out->stride = (width * FB_BYTES_PER_PIXEL + FB_STRIDE_ALIGN - 1u) & ~(FB_STRIDE_ALIGN - 1u);
    return KERR_OK;
}

uint64_t fbinfo_bytes(const struct fb_mode *m) {
    uint64_t bytes = (uint64_t)m->stride * m->height;
    return (bytes + PAGE_BYTES - 1u) & ~(uint64_t)(PAGE_BYTES - 1u);
}

static struct fb_bitfield channel(uint32_t offset, uint32_t length) {
    struct fb_bitfield b = {.offset = offset, .length = length, .msb_right = 0};
    return b;
}

void fbinfo_describe_var(const struct fb_mode *m, struct fb_var_screeninfo *v) {
    *v = (struct fb_var_screeninfo){0};
    v->xres = m->width;
    v->yres = m->height;
    v->xres_virtual = m->width;
    v->yres_virtual = m->height;
    v->bits_per_pixel = FB_BYTES_PER_PIXEL * 8u;
    v->red = channel(16u, 8u);
    v->green = channel(8u, 8u);
    v->blue = channel(0u, 8u);
    v->transp = channel(24u, 0u); /* X, not A: the top byte is not alpha */
}

#define FB_TYPE_PACKED_PIXELS 0u
#define FB_VISUAL_TRUECOLOR 2u

void fbinfo_describe_fix(const struct fb_mode *m, const char *id, struct fb_fix_screeninfo *f) {
    *f = (struct fb_fix_screeninfo){0};
    for (uint32_t i = 0; id != NULL && id[i] != '\0' && i + 1u < sizeof f->id; i++) {
        f->id[i] = id[i];
    }
    f->smem_len = (uint32_t)fbinfo_bytes(m);
    f->type = FB_TYPE_PACKED_PIXELS;
    f->visual = FB_VISUAL_TRUECOLOR;
    f->line_length = m->stride;
}

static bool same_channel(struct fb_bitfield a, struct fb_bitfield b) {
    return a.offset == b.offset && a.length == b.length && a.msb_right == b.msb_right;
}

bool fbinfo_var_matches(const struct fb_mode *m, const struct fb_var_screeninfo *v) {
    struct fb_var_screeninfo want;
    fbinfo_describe_var(m, &want);
    return v->xres == want.xres && v->yres == want.yres && v->xres_virtual == want.xres_virtual &&
           v->yres_virtual == want.yres_virtual && v->xoffset == 0 && v->yoffset == 0 &&
           v->bits_per_pixel == want.bits_per_pixel && same_channel(v->red, want.red) &&
           same_channel(v->green, want.green) && same_channel(v->blue, want.blue);
}
