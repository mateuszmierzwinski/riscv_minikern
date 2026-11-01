#include "kernel/watchdog.h"

#include <stdbool.h>
#include <stddef.h>

#include "arch/riscv/timer.h"
#include "core/console.h"
#include "core/errno.h"
#include "kernel/fdobjects.h"
#include "kernel/file.h"
#include "kernel/uspace.h"

#define WATCHDOG_PATH "/dev/watchdog"
#define FEED_EVERY_NS 1000000000ull /* once a second: half the shortest timeout allowed */
#define TIMEOUT_MIN_S 2u
#define TIMEOUT_MAX_S 170u /* a 32-bit count at the board's 24 MHz */
#define MAGIC_CLOSE 'V'
#define WDIOC_KEEPALIVE 0x80045705u
#define WDIOC_SETTIMEOUT 0xc0045706u
#define WDIOC_GETTIMEOUT 0x80045707u
#define SCAN_CHUNK 64u

/* The one watchdog. Touched only under the kernel lock: the beat hook runs
 * there, and so does every file operation. */
static struct {
    struct watchdog_port port;
    uint32_t seconds;
    bool kernel_feeds;  /* false while a process holds /dev/watchdog */
    bool held;
    bool magic;         /* the holder wrote 'V' since its last feed */
    uint64_t last_feed_ns, starve_at_ns;
    uint64_t feeds;
} wd;

static void feed(void) {
    if (wd.port.feed(wd.port.self, wd.seconds) == KERR_OK) {
        wd.feeds++;
        wd.last_feed_ns = timer_ns();
    }
}

/* The kernel's feeding, once a second, until a starve test says stop. */
static void feed_from_beat(void) {
    uint64_t now = timer_ns();
    if (!wd.kernel_feeds || now - wd.last_feed_ns < FEED_EVERY_NS) {
        return;
    }
    if (wd.starve_at_ns != 0 && now >= wd.starve_at_ns) {
        kprintf("wdt: no longer fed by the kernel (minikern.wdtstarve), %u s to the reset\n",
                wd.seconds);
        wd.kernel_feeds = false;
        return;
    }
    feed();
}

/* Opening takes the feeding over; a second holder is refused. */
static int32_t wd_open(struct file *f) {
    if (wd.held) {
        return KERR_BUSY;
    }
    f->priv = &wd; /* the holder's mark: a refused open's file never gets it */
    wd.held = true;
    wd.magic = false;
    wd.kernel_feeds = false;
    feed();
    return KERR_OK;
}

/* Whether the user buffer holds a 'V', read a chunk at a time. */
static bool holds_magic(const void *buf, uint32_t n) {
    uint8_t chunk[SCAN_CHUNK];
    for (uint32_t at = 0; at < n; at += SCAN_CHUNK) {
        uint32_t take = n - at < SCAN_CHUNK ? n - at : SCAN_CHUNK;
        if (uspace_copy_in(chunk, (virt_t)(uintptr_t)buf + at, take) != KERR_OK) {
            return false;
        }
        for (uint32_t i = 0; i < take; i++) {
            if (chunk[i] == MAGIC_CLOSE) {
                return true;
            }
        }
    }
    return false;
}

/* Any write feeds; a 'V' in it arms the magic close. */
static int32_t wd_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    wd.magic = holds_magic(buf, n);
    feed();
    return (int32_t)n;
}

/* `WDIOC_SETTIMEOUT`: a new timeout within what the counter can hold,
 * re-armed at once and written back, as Linux writes back what it set. */
static int32_t set_timeout(virt_t arg) {
    uint32_t want = 0;
    if (uspace_copy_in(&want, arg, sizeof want) != KERR_OK) {
        return KERR_FAULT;
    }
    if (want < TIMEOUT_MIN_S || want > TIMEOUT_MAX_S) {
        return KERR_INVAL;
    }
    wd.seconds = want;
    if (wd.port.arm(wd.port.self, want) != KERR_OK) {
        return KERR_IO;
    }
    feed();
    return uspace_copy_out(arg, &want, sizeof want) == KERR_OK ? 0 : KERR_FAULT;
}

static int32_t wd_ioctl(struct file *f, uint32_t request, virt_t arg) {
    (void)f;
    switch (request) {
    case WDIOC_KEEPALIVE:
        feed();
        return 0;
    case WDIOC_GETTIMEOUT:
        return uspace_copy_out(arg, &wd.seconds, sizeof wd.seconds) == KERR_OK ? 0 : KERR_FAULT;
    case WDIOC_SETTIMEOUT:
        return set_timeout(arg);
    default:
        return KERR_NOTTY;
    }
}

/* The holder goes: with the magic close the kernel feeds again, without it
 * nothing does — the daemon died, and the reset is the point. */
static void wd_release(struct file *f) {
    /* **Only the holder's file.** A refused second open is built and put
     * back, which runs this too — and once cleared `held` and announced a
     * close without 'V' while the real holder still fed it. */
    if (f->priv != &wd) {
        return;
    }
    wd.held = false;
    if (wd.magic) {
        wd.kernel_feeds = true;
        feed();
        return;
    }
    kprintf("wdt: %s closed without '%c': not fed, %u s to the reset\n", WATCHDOG_PATH,
            MAGIC_CLOSE, wd.seconds);
}

static const struct file_ops watchdog_ops = {
    .open = wd_open,
    .write = wd_write,
    .ioctl = wd_ioctl,
    .release = wd_release,
};

int watchdog_start(const struct watchdog_port *port, uint32_t seconds, uint32_t starve_after_s) {
    static const struct device_owner root_only = {0600u, 0u, 0u};
    int rc;
    if (seconds < TIMEOUT_MIN_S || seconds > TIMEOUT_MAX_S) {
        return KERR_INVAL;
    }
    wd.port = *port;
    wd.seconds = seconds;
    rc = wd.port.arm(wd.port.self, seconds);
    rc = rc != KERR_OK ? rc : fdobjects_add_device(WATCHDOG_PATH, &watchdog_ops, NULL, &root_only);
    if (rc != KERR_OK) {
        return rc;
    }
    (void)synthfs_add_device(WATCHDOG_PATH, &root_only);
    wd.kernel_feeds = true;
    feed();
    wd.starve_at_ns = starve_after_s != 0 ? timer_ns() + (uint64_t)starve_after_s * 1000000000ull : 0;
    return timer_add_housekeeping_hook(feed_from_beat);
}

void watchdog_stop(void) {
    wd.kernel_feeds = false;
    if (wd.port.stop != NULL && wd.port.stop(wd.port.self) == KERR_OK) {
        kprintf("wdt: stopped for the power-off\n");
    }
}

uint64_t watchdog_feeds(void) {
    return wd.feeds;
}
