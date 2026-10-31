#include "kernel/fbdev.h"

#include <stdbool.h>

#include "arch/riscv/mmu.h"
#include "arch/riscv/pgtable.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/mem.h"
#include "kernel/synthfs.h"
#include "kernel/uspace.h"

/* What `FBIOGET_FSCREENINFO` names the device, as Linux drivers name theirs. */
#define FBDEV_ID "minikern vfb"

/* The one framebuffer. Written once at install, before anything can open
 * it, and read-only after — so it needs no lock of its own. */
static struct {
    bool installed;
    phys_t base;
    uint64_t bytes;
    struct fb_mode mode;
} fb;

/* The pixels, through the kernel's direct map. */
static uint8_t *pixels(void) {
    return mmu_phys_to_virt(fb.base);
}

/* How much of a transfer of `n` at `off` lies inside the framebuffer. */
static uint32_t inside(uint32_t n, uint64_t off) {
    if (off >= fb.bytes) {
        return 0;
    }
    return fb.bytes - off < n ? (uint32_t)(fb.bytes - off) : n;
}

/* `pread`: the pixels themselves — what a screenshot or a test reads to
 * check a mapping drew where it meant to. 0 at the end, like a file. */
static int32_t fb_pread(struct file *f, void *buf, uint32_t n, uint64_t off) {
    uint32_t take = inside(n, off);
    (void)f;
    if (take == 0) {
        return 0;
    }
    return uspace_copy_out((virt_t)(uintptr_t)buf, pixels() + off, take) == KERR_OK
               ? (int32_t)take
               : KERR_FAULT;
}

/* `pwrite`: pixels in, for a program that would rather not map. Past the
 * end is `ENOSPC`, as Linux answers a write beyond a framebuffer. */
static int32_t fb_pwrite(struct file *f, const void *buf, uint32_t n, uint64_t off) {
    uint32_t take = inside(n, off);
    if ((f->flags & O_ACCMODE) == O_RDONLY) {
        return KERR_BADF;
    }
    if (take == 0) {
        return n == 0 ? 0 : KERR_NOSPC;
    }
    return uspace_copy_in(pixels() + off, (virt_t)(uintptr_t)buf, take) == KERR_OK
               ? (int32_t)take
               : KERR_FAULT;
}

/* The frame behind a page of the framebuffer, for `MAP_SHARED`. */
static int32_t fb_map_page(struct file *f, uint64_t offset, phys_t *phys) {
    (void)f;
    if ((offset & (PAGE_SIZE - 1u)) != 0 || offset >= fb.bytes) {
        return KERR_INVAL;
    }
    *phys = fb.base + offset;
    return KERR_OK;
}

static int32_t get_var(virt_t arg) {
    struct fb_var_screeninfo v;
    fbinfo_describe_var(&fb.mode, &v);
    return uspace_copy_out(arg, &v, (uint32_t)sizeof v);
}

static int32_t get_fix(virt_t arg) {
    struct fb_fix_screeninfo x;
    fbinfo_describe_fix(&fb.mode, FBDEV_ID, &x);
    return uspace_copy_out(arg, &x, (uint32_t)sizeof x);
}

/* `FBIOPUT_VSCREENINFO`: honoured when it asks for the mode there is, and
 * answered with it, as Linux writes the result back; any other mode is
 * refused — a virtual framebuffer cannot change shape under its mappings. */
static int32_t put_var(virt_t arg) {
    struct fb_var_screeninfo v;
    int rc = uspace_copy_in(&v, arg, (uint32_t)sizeof v);
    if (rc != KERR_OK) {
        return rc;
    }
    return fbinfo_var_matches(&fb.mode, &v) ? get_var(arg) : KERR_INVAL;
}

/* `FBIOPAN_DISPLAY`: the visible area is the whole buffer, so the only pan
 * there is is to the origin. */
static int32_t pan(virt_t arg) {
    struct fb_var_screeninfo v;
    int rc = uspace_copy_in(&v, arg, (uint32_t)sizeof v);
    if (rc != KERR_OK) {
        return rc;
    }
    return v.xoffset == 0 && v.yoffset == 0 ? KERR_OK : KERR_INVAL;
}

/* `FBIOBLANK`'s levels, 0 (unblank) to 4 (powerdown). With nothing to
 * blank, a valid level succeeds and does nothing — and is vacuous, not
 * ignored: there is no light to switch off. */
#define FB_BLANK_POWERDOWN 4u

static int32_t fb_ioctl(struct file *f, uint32_t request, virt_t arg) {
    (void)f;
    switch (request) {
    case FBIOGET_VSCREENINFO:
        return get_var(arg);
    case FBIOGET_FSCREENINFO:
        return get_fix(arg);
    case FBIOPUT_VSCREENINFO:
        return put_var(arg);
    case FBIOPAN_DISPLAY:
        return pan(arg);
    case FBIOBLANK:
        return arg <= FB_BLANK_POWERDOWN ? KERR_OK : KERR_INVAL;
    default:
        return KERR_NOTTY;
    }
}

static const struct file_ops fb_ops = {
    .pread = fb_pread,
    .pwrite = fb_pwrite,
    .ioctl = fb_ioctl,
    .map_page = fb_map_page,
};

/* Takes the frames in one run and clears them: a screen starts black, and
 * memory a previous owner left is not this device's to show. */
static int allocate_the_frames(uint64_t bytes) {
    int rc = mem_alloc_pages((uint32_t)(bytes / PAGE_SIZE), 1u, &fb.base);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint64_t i = 0; i < bytes; i++) {
        pixels()[i] = 0;
    }
    fb.bytes = bytes;
    return KERR_OK;
}

int fbdev_install(const struct fb_mode *mode) {
    int rc;
    if (fb.installed) {
        return KERR_BUSY;
    }
    rc = allocate_the_frames(fbinfo_bytes(mode));
    if (rc != KERR_OK) {
        return rc;
    }
    fb.mode = *mode;
    rc = fdobjects_add_device(FBDEV_PATH, &fb_ops, NULL);
    if (rc != KERR_OK) {
        (void)mem_free_pages(fb.base, (uint32_t)(fb.bytes / PAGE_SIZE));
        return rc;
    }
    (void)synthfs_add_device(FBDEV_PATH);
    fb.installed = true;
    kprintf("fb: %s %ux%u, stride %u, %llu KiB\n", FBDEV_PATH, mode->width, mode->height,
            mode->stride, (unsigned long long)(fb.bytes / 1024u));
    return KERR_OK;
}

int fbdev_memory(phys_t *base, struct fb_mode *mode) {
    if (!fb.installed) {
        return KERR_NODEV;
    }
    *base = fb.base;
    *mode = fb.mode;
    return KERR_OK;
}
