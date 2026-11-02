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
    uint64_t bytes;  /* the frame's pages: what read, write and lseek see */
    struct fb_mode mode;
} fb;

/* The frame's pages and the overlay area after them, in one allocation. */
static uint64_t allocated_bytes(void) {
    return fb.bytes + FB_OVERLAY_BYTES;
}

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

/* `read` and `write`: `pread` and `pwrite` at the descriptor's position,
 * which they move — what `cat /dev/fb0 > shot` and `dd` use. */
static int32_t fb_read(struct file *f, void *buf, uint32_t n) {
    int32_t got = fb_pread(f, buf, n, f->pos);
    f->pos += got > 0 ? (uint64_t)got : 0u;
    return got;
}

static int32_t fb_write(struct file *f, const void *buf, uint32_t n) {
    int32_t put = fb_pwrite(f, buf, n, f->pos);
    f->pos += put > 0 ? (uint64_t)put : 0u;
    return put;
}

/* The length `lseek(SEEK_END)` measures from: the whole framebuffer. */
static uint64_t fb_size(struct file *f) {
    (void)f;
    return fb.bytes;
}

/* The frame behind a page of the framebuffer, for `MAP_SHARED`. */
static int32_t fb_map_page(struct file *f, uint64_t offset, phys_t *phys) {
    (void)f;
    if ((offset & (PAGE_SIZE - 1u)) != 0) {
        return KERR_INVAL;
    }
    if (offset < fb.bytes) {
        *phys = fb.base + offset;
        return KERR_OK;
    }
    if (offset >= FB_OVERLAY_MMAP_OFFSET && offset - FB_OVERLAY_MMAP_OFFSET < FB_OVERLAY_BYTES) {
        *phys = fb.base + fb.bytes + (offset - FB_OVERLAY_MMAP_OFFSET);
        return KERR_OK;
    }
    return KERR_INVAL; /* past the frame, short of the overlay area: nothing */
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

/* The rows programs have said they changed since the last flush
 * (`MINIKERN_FBIO_DIRTY`): [first, end), and whether any program has used
 * the ioctl at all — until one does, the whole frame is flushed as before. */
static struct {
    uint32_t users; /* open descriptors that have marked: dirty mode while > 0 */
    bool pending;
    uint32_t first, end;
} dirty;

static int32_t mark_dirty(struct file *f, virt_t arg) {
    struct fb_dirty d;
    uint32_t first, end;
    if (uspace_copy_in(&d, arg, sizeof d) != KERR_OK) {
        return KERR_FAULT;
    }
    if (f->priv != &dirty) {
        f->priv = &dirty; /* this descriptor marks: counted until it closes */
        dirty.users++;
    }
    if (!fbinfo_dirty_rows(&fb.mode, &d, &first, &end)) {
        return KERR_OK; /* nothing on screen changed */
    }
    dirty.first = dirty.pending && dirty.first < first ? dirty.first : first;
    dirty.end = dirty.pending && dirty.end > end ? dirty.end : end;
    dirty.pending = true;
    return KERR_OK;
}

bool fbdev_take_dirty(uint64_t *offset, uint64_t *bytes) {
    if (dirty.users == 0) {
        return false;
    }
    *offset = dirty.pending ? (uint64_t)dirty.first * fb.mode.stride : 0u;
    *bytes = dirty.pending ? (uint64_t)(dirty.end - dirty.first) * fb.mode.stride : 0u;
    dirty.pending = false;
    return true;
}

/* Who shows the overlay, and which descriptor put one up: when that one
 * closes the overlay goes, as a dirty-marking descriptor's marks do — a
 * plane left showing a dead program's memory is a stale picture at best. */
static struct {
    const struct fbdev_overlay_port *port;
    struct file *owner;
} overlay;

void fbdev_set_overlay_port(const struct fbdev_overlay_port *port) {
    overlay.port = port;
}

static void hide_overlay(void) {
    if (overlay.owner != NULL) {
        overlay.port->hide();
        overlay.owner = NULL;
    }
}

static int32_t set_overlay(struct file *f, virt_t arg) {
    struct fb_overlay o;
    int rc;
    if (uspace_copy_in(&o, arg, sizeof o) != KERR_OK) {
        return KERR_FAULT;
    }
    if (overlay.port == NULL) {
        return KERR_NODEV;
    }
    if (o.width == 0u) {
        hide_overlay();
        return KERR_OK;
    }
    if (!fbinfo_overlay_ok(&fb.mode, &o)) {
        return KERR_INVAL;
    }
    rc = overlay.port->show(fb.base + fb.bytes + o.offset, &o);
    overlay.owner = rc == KERR_OK ? f : overlay.owner;
    return rc;
}

/* A descriptor that marked goes: when it was the last, whole frames are
 * flushed again — a later program that draws without marking must not be
 * left unflushed because an earlier one marked. */
static void fb_release(struct file *f) {
    if (f->priv == &dirty && dirty.users > 0) {
        dirty.users--;
    }
    if (overlay.owner == f) {
        hide_overlay();
    }
}

static int32_t fb_ioctl(struct file *f, uint32_t request, virt_t arg) {
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
    case MINIKERN_FBIO_DIRTY:
        return mark_dirty(f, arg);
    case MINIKERN_FBIO_OVERLAY:
        return set_overlay(f, arg);
    default:
        return KERR_NOTTY;
    }
}

static const struct file_ops fb_ops = {
    .read = fb_read,
    .write = fb_write,
    .size = fb_size,
    .pread = fb_pread,
    .pwrite = fb_pwrite,
    .ioctl = fb_ioctl,
    .release = fb_release,
    .map_page = fb_map_page,
};

/* Takes the frames and the overlay area in one run and clears them: a
 * screen starts black, and memory a previous owner left is not this
 * device's to show. */
static int allocate_the_frames(uint64_t bytes) {
    uint64_t all = bytes + FB_OVERLAY_BYTES;
    int rc = mem_alloc_pages((uint32_t)(all / PAGE_SIZE), 1u, &fb.base);
    if (rc != KERR_OK) {
        return rc;
    }
    for (uint64_t i = 0; i < all; i++) {
        pixels()[i] = 0;
    }
    fb.bytes = bytes;
    return KERR_OK;
}

int fbdev_install(const struct fb_mode *mode, const struct device_owner *owner) {
    int rc;
    if (fb.installed) {
        return KERR_BUSY;
    }
    rc = allocate_the_frames(fbinfo_bytes(mode));
    if (rc != KERR_OK) {
        return rc;
    }
    fb.mode = *mode;
    rc = fdobjects_add_device(FBDEV_PATH, &fb_ops, NULL, owner);
    if (rc != KERR_OK) {
        (void)mem_free_pages(fb.base, (uint32_t)(allocated_bytes() / PAGE_SIZE));
        return rc;
    }
    (void)synthfs_add_device(FBDEV_PATH, owner);
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
