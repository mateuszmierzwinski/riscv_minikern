/* fbdev — `/dev/fb0`, a framebuffer in the kernel's own memory.
 *
 * The first rung of the display stack (ROADMAP §10): pixels a program can
 * draw into, through the interface Linux programs already use — open the
 * device, ask its geometry with `FBIOGET_VSCREENINFO`/`FSCREENINFO`, and
 * `mmap` it `MAP_SHARED`. Every process that maps it maps **the same
 * frames**, so what one draws, another (a VNC server, a compositor) sees.
 *
 * With no display attached it is a virtual framebuffer: memory with a
 * shape. A display driver scans the same frames out (`fbdev_memory`), which
 * is why they are one physically contiguous run, allocated at boot while
 * memory is unfragmented.
 */
#ifndef MINIKERN_KERNEL_FBDEV_H
#define MINIKERN_KERNEL_FBDEV_H

#include <stdint.h>

#include "core/fbinfo.h"
#include "core/types.h"

#define FBDEV_PATH "/dev/fb0"

/* Allocates the framebuffer for `mode`, clears it and makes `/dev/fb0`
 * openable. KERR_NOMEM when the frames cannot be had in one run, KERR_BUSY
 * when there is already one. */
int fbdev_install(const struct fb_mode *mode);

/* Where the pixels are and their shape, for a display driver to scan out.
 * KERR_NODEV when no framebuffer was installed. */
int fbdev_memory(phys_t *base, struct fb_mode *mode);

#endif
