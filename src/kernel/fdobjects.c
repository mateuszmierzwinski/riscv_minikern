#include "kernel/fdobjects.h"

#include "core/reclock.h"
#include "kernel/random.h"
#include "kernel/perm.h"

#include "core/klog.h"
#include "kernel/net.h"
#include "kernel/procfs.h"

#include <stddef.h>

#include "core/console.h"
#include "core/errno.h"
#include "core/access.h"
#include "core/caps.h"
#include "kernel/mem.h"
#include "kernel/sched.h"
#include "kernel/tty.h"
#include "kernel/uspace.h"
#include "kernel/xfsmount.h"

/* ── the console ────────────────────────────────────────────────────────── */

/* Reading the console is reading the **terminal** now: a line at a time,
 * blocking until one is typed, and refused with `SIGTTIN` for a process
 * that is not in the foreground group.
 *
 * It used to return 0 — end of file — because nothing was wired to the
 * input side, and the comment said so. That answer was true and is now
 * wrong, which is the usual fate of a stub that reports success. */
static int32_t console_file_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    return ktty_read(buf, n);
}

/* Ready when a line is waiting, so `poll` on the console means something.
 * `NULL` meant "always ready both ways", which for the read side was a lie
 * that a runtime acts on by reading and getting nothing. */
static uint32_t console_file_poll(struct file *f) {
    (void)f;
    /* Writable always; readable when a line is waiting. `NULL` here meant
     * "always ready for everything", which for the read side was a lie a
     * runtime acts on by reading and getting nothing. */
    return EPOLLOUT | (ktty_ready() ? EPOLLIN : 0u);
}

/* Linux's `struct termios` as `TCGETS` returns it (asm-generic: four flag
 * words, the line discipline, nineteen control characters). */
struct user_termios {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t line;
    uint8_t cc[19];
};

#define TCGETS 0x5401u
#define TCSETS 0x5402u
#define TCSETSW 0x5403u
#define TCSETSF 0x5404u
#define FIONREAD_REQ 0x541bu

/* `TCGETS`: the terminal's mode as `kernel/tty.c` keeps it. */
static void describe_terminal(struct user_termios *t) {
    struct ktty_mode m;
    ktty_get_mode(&m);
    *t = (struct user_termios){.iflag = m.iflag, .oflag = m.oflag, .cflag = m.cflag,
                               .lflag = m.lflag};
    for (uint32_t i = 0; i < KTTY_NCC; i++) {
        t->cc[i] = m.cc[i];
    }
}

/* `TCSETS*`: the whole mode, adopted or refused (`EINVAL`) as one — a flag
 * the terminal would ignore is never reported as set. */
static int32_t set_terminal(virt_t arg) {
    struct user_termios t;
    struct ktty_mode m;
    if (uspace_copy_in(&t, arg, sizeof t) != KERR_OK) {
        return KERR_FAULT;
    }
    m = (struct ktty_mode){.iflag = t.iflag, .oflag = t.oflag, .cflag = t.cflag, .lflag = t.lflag};
    for (uint32_t i = 0; i < KTTY_NCC; i++) {
        m.cc[i] = t.cc[i];
    }
    return ktty_set_mode(&m);
}

/* The terminal's attribute requests, and how much is waiting to be read. */
static int32_t console_attr_ioctl(uint32_t request, virt_t arg) {
    struct user_termios t;
    uint32_t pending;
    switch (request) {
    case TCGETS:
        describe_terminal(&t);
        return uspace_copy_out(arg, &t, sizeof t) == KERR_OK ? 0 : KERR_FAULT;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return set_terminal(arg);
    case FIONREAD_REQ:
        pending = ktty_pending();
        return uspace_copy_out(arg, &pending, sizeof pending) == KERR_OK ? 0 : KERR_FAULT;
    default:
        return KERR_NOTTY;
    }
}

/* `ioctl`, for the two things a program asks a terminal.
 *
 * A program that cannot get a window size lays itself out for 0x0, and one
 * that cannot find the foreground group cannot do job control at all. The
 * rest of `ioctl` is refused rather than pretended: returning 0 for an
 * unknown request tells a caller its request was honoured. */
static int32_t console_file_ioctl(struct file *f, uint32_t request, virt_t arg) {
    struct user_winsize ws;
    uint32_t pgid;
    (void)f;
    switch (request) {
    case TIOCGWINSZ:
        ktty_window(&ws.rows, &ws.cols);
        ws.xpixel = 0;
        ws.ypixel = 0;
        return uspace_copy_out(arg, &ws, sizeof ws) == KERR_OK ? 0 : KERR_FAULT;
    case TIOCSWINSZ:
        if (uspace_copy_in(&ws, arg, sizeof ws) != KERR_OK) {
            return KERR_FAULT;
        }
        ktty_set_window(ws.rows, ws.cols);
        return 0;
    case TIOCGPGRP:
        pgid = ktty_foreground();
        return uspace_copy_out(arg, &pgid, sizeof pgid) == KERR_OK ? 0 : KERR_FAULT;
    case TIOCSPGRP:
        if (uspace_copy_in(&pgid, arg, sizeof pgid) != KERR_OK) {
            return KERR_FAULT;
        }
        ktty_set_foreground(pgid);
        return 0;
    default:
        return console_attr_ioctl(request, arg);
    }
}

/* Named for the object rather than the operation, because `console_write`
 * is already the console port's own function and this is the file layer's
 * adapter onto it. */
static int32_t console_file_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    ktty_write(buf, n);
    return (int32_t)n;
}

static const struct file_ops console_ops = {
    .read = console_file_read,
    .write = console_file_write,
    .poll = console_file_poll,
    .ioctl = console_file_ioctl,
};

int fdobjects_open_console(void) {
    for (uint32_t i = 0; i < 3u; i++) {
        struct file *f = file_new(&console_ops, NULL, 0);
        int fd = fd_install(f);
        if (fd < 0) {
            return fd;
        }
    }
    return KERR_OK;
}

/* ── the eventfd ────────────────────────────────────────────────────────── */

/* A counter, and the semantics are the counter: a write adds, a read takes
 * it all and leaves zero, and a read of zero is a would-block rather than
 * end of file — a reader that got 0 bytes would take it for end of file and
 * stop polling. */
struct eventfd {
    uint64_t count;
};

static int32_t eventfd_read(struct file *f, void *buf, uint32_t n) {
    struct eventfd *e = f->priv;
    if (n < 8u) {
        return KERR_INVAL;
    }
    if (e->count == 0) {
        return KERR_AGAIN;
    }
    *(uint64_t *)buf = e->count;
    e->count = 0;
    file_readiness_changed(f);
    return 8;
}

static int32_t eventfd_write(struct file *f, const void *buf, uint32_t n) {
    struct eventfd *e = f->priv;
    if (n != 8u) {
        return KERR_INVAL;
    }
    e->count += *(const uint64_t *)buf;
    /* **The point of the whole object**: waking whoever is waiting. A
     * runtime writes here to interrupt its own poller, and a kernel that
     * only stores the number leaves the poller asleep until its original
     * timeout — which turned a 50 ms sleep into 950 ms. */
    file_readiness_changed(f);
    (void)sched_futex_wake(SCHED_POLLER_KEY, 64u);
    return 8;
}

static uint32_t eventfd_poll(struct file *f) {
    const struct eventfd *e = f->priv;
    return EPOLLOUT | (e->count != 0 ? EPOLLIN : 0u);
}

static void eventfd_release(struct file *f) {
    kfree(f->priv);
}

static const struct file_ops eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .poll = eventfd_poll,
    .release = eventfd_release,
};

int fdobjects_open_eventfd(uint64_t initial, uint32_t flags) {
    struct eventfd *e = kzalloc(sizeof *e);
    struct file *f;
    if (e == NULL) {
        return KERR_NOMEM;
    }
    e->count = initial;
    f = file_new(&eventfd_ops, e, flags);
    if (f == NULL) {
        kfree(e);
        return KERR_NOMEM;
    }
    return fd_install(f);
}

/* ── the few paths that exist ───────────────────────────────────────────── */

/* `/dev/virtual/kernel_logger`: the log's last lines, as a file.
 *
 * **A snapshot taken at open, not a stream.** The log keeps moving, and a
 * reader that got whatever happened to be in the ring between its two
 * `read` calls would see a line twice or not at all. Taking the tail once
 * means two reads of one descriptor return one consistent document, which
 * is what a program expects of a file.
 *
 * Read-only: a write would be a program claiming the kernel said something.
 */
#define KLOG_SNAPSHOT_BYTES (KLOG_TAIL_LINES * (KLOG_LINE_MAX + 1u))

/* **One snapshot per descriptor**, taken at open and freed at close. They
 * were one global each (the log, /proc, resolv.conf) on the reasoning that
 * only one open happens at a time — but descriptors outlive the open, so a
 * process holding a /proc descriptor read whatever the next opener's file
 * said, root's included, and two readers moved each other's offset. */
struct snapshot {
    uint32_t len;
    uint32_t offset;
    char text[];
};

static int32_t snapshot_read(struct file *f, void *buf, uint32_t n) {
    struct snapshot *s = f->priv;
    uint32_t left, take;
    if (s == NULL || s->offset >= s->len) {
        return 0; /* the end of the snapshot, as a file ends */
    }
    left = s->len - s->offset;
    take = n < left ? n : left;
    for (uint32_t i = 0; i < take; i++) {
        ((uint8_t *)buf)[i] = (uint8_t)s->text[s->offset + i];
    }
    s->offset += take;
    return (int32_t)take;
}

static void snapshot_release(struct file *f) {
    kfree(f->priv);
    f->priv = NULL;
}

static int32_t klog_write_refused(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    /* Not `EBADF`, which would say the descriptor is wrong. The descriptor
     * is fine; the file is the kernel's account of itself. */
    return KERR_PERM;
}

static const struct file_ops klog_ops = {
    .read = snapshot_read,
    .release = snapshot_release,
    .write = klog_write_refused,
};

/* `/etc/resolv.conf`, synthesised from what DHCP learned.
 *
 * **The kernel has no resolver and should not have one** — resolving a name
 * means a UDP exchange, a cache and a policy, none of which belong in here.
 * What the kernel does have is the only copy of the resolver's address: it
 * arrived in a DHCP option and was stored on the interface, where nothing in
 * user space could reach it. So a program could not resolve a name, not for
 * want of a resolver but for want of the one line that says where to ask.
 *
 * This is that line, in the file every resolver already looks in — so a
 * stock Go binary's `net.LookupHost` works with nothing ported.
 *
 * **A real file wins.** The filesystem is tried first and this is the
 * fallback, because an operator who wrote their own `/etc/resolv.conf` meant
 * it, and a kernel that shadowed it would be overriding a decision it cannot
 * see. Synthesising only when the file is absent is also what makes this
 * safe on a read-only root. */
#define RESOLV_SNAPSHOT_BYTES 64u

/* "nameserver a.b.c.d\n", built without a formatter: this runs under the
 * kernel lock on the open path and `ip4_format` is already here. */
static void fill_resolv_snapshot(struct snapshot *snap) {
    char ip[16];
    const char *prefix = "nameserver ";
    struct netif *nif = knet_interface();
    uint32_t at = 0;
            if (nif == NULL || nif->dns == 0) {
        return; /* nothing learned: an empty file, which is a true answer */
    }
    while (prefix[at] != '\0') {
        snap->text[at] = prefix[at];
        at++;
    }
    (void)ip4_format(nif->dns, ip);
    for (uint32_t i = 0; ip[i] != '\0' && at < RESOLV_SNAPSHOT_BYTES - 2u; i++) {
        snap->text[at++] = ip[i];
    }
    snap->text[at++] = '\n';
    snap->len = at;
}


static const struct file_ops procfile_ops = {
    .read = snapshot_read,
    .release = snapshot_release,
    .write = klog_write_refused,
};

static const struct file_ops resolv_ops = {
    .read = snapshot_read,
    .release = snapshot_release,
    .write = klog_write_refused,
};

/* `/dev/null`: reads end immediately, writes vanish. */
static int32_t null_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    return 0;
}

static int32_t null_write(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    return (int32_t)n;
}

/* `/dev/zero`: reads give zeros, for ever. */
static int32_t zero_read(struct file *f, void *buf, uint32_t n) {
    uint8_t *to = buf;
    (void)f;
    for (uint32_t i = 0; i < n; i++) {
        to[i] = 0;
    }
    return (int32_t)n;
}

/* `/dev/urandom` and `/dev/random`: the kernel's generator, which never
 * blocks once seeded (as `getrandom` already behaves). A write is accepted
 * and ignored, as Linux accepts one without crediting entropy. */
static int32_t random_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    random_fill(buf, n);
    return (int32_t)n;
}

static const struct file_ops null_ops = {.read = null_read, .write = null_write};
static const struct file_ops zero_ops = {.read = zero_read, .write = null_write};
static const struct file_ops random_ops = {.read = random_read, .write = null_write};

/* ── a regular file ─────────────────────────────────────────────────────── */

/* A file off the disk, read **whole** into the kernel's heap when it is
 * opened.
 *
 * Not how a real kernel does it, and the reason is the filesystem
 * underneath: `xfsmount` reads a file by name, not a block of one by
 * offset, so there is nothing to be lazy with yet. What this buys is the
 * two things that were missing — a descriptor a program can `read`, and a
 * mapping that can be backed from a file — and what it costs is the file's
 * size in kernel memory for as long as the descriptor is open. The limit is
 * stated rather than discovered: a file larger than it will not open.
 *
 * It is read-only. Writing one needs the filesystem's write path, which is
 * `ROADMAP.md` §3's business. */
#define DISKFILE_MAX_BYTES (16u * 1024u * 1024u)

struct diskfile {
    uint8_t *bytes;
    uint32_t size;
    uint32_t offset; /* how much of it this descriptor has handed over */
    bool used;       /* claimed before the read, which can sleep */
};

static struct diskfile diskfiles[FD_MAX];

/* A free slot, **claimed at once**: it was recognised by `bytes == NULL`,
 * which stays true across the read that fills it — and that read sleeps
 * (NVMe, netfs), so two opens took one slot and closing either freed the
 * other's bytes. */
static struct diskfile *diskfile_alloc(void) {
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (!diskfiles[i].used) {
            diskfiles[i] = (struct diskfile){.used = true};
            return &diskfiles[i];
        }
    }
    return NULL;
}

static int32_t diskfile_read(struct file *f, void *buf, uint32_t n) {
    struct diskfile *d = f->priv;
    uint32_t left = d->size - d->offset;
    uint32_t take = n < left ? n : left;
    for (uint32_t i = 0; i < take; i++) {
        ((uint8_t *)buf)[i] = d->bytes[d->offset + i];
    }
    d->offset += take;
    return (int32_t)take;
}

/* A range of the snapshot, copied out rather than stored through: the
 * caller's buffer is only a number until the copy checks it. */
static int32_t diskfile_pread(struct file *f, void *buf, uint32_t n, uint64_t off) {
    const struct diskfile *d = f->priv;
    uint32_t take = off >= d->size ? 0u : (d->size - off < n ? (uint32_t)(d->size - off) : n);
    if (take == 0) {
        return 0;
    }
    return uspace_copy_out((virt_t)(uintptr_t)buf, d->bytes + off, take) == KERR_OK
               ? (int32_t)take
               : KERR_FAULT;
}

static void diskfile_release_buffer(struct diskfile *d) {
    kfree_image(d->bytes);
    *d = (struct diskfile){0};
}

static void diskfile_release(struct file *f) {
    diskfile_release_buffer(f->priv);
}

/* `FIONREAD` on a file: what is left after this descriptor's offset. */
static int32_t diskfile_ioctl(struct file *f, uint32_t request, virt_t arg) {
    const struct diskfile *d = f->priv;
    uint32_t left;
    if (request != FIONREAD_REQ || d == NULL) {
        return KERR_NOTTY;
    }
    left = d->size - d->offset;
    return uspace_copy_out(arg, &left, sizeof left) == KERR_OK ? 0 : KERR_FAULT;
}

static const struct file_ops diskfile_ops = {.read = diskfile_read, .ioctl = diskfile_ioctl,
                                             .release = diskfile_release,
                                             .pread = diskfile_pread};

/* The bytes behind a descriptor, for a mapping to be backed from — and
 * `KERR_NODEV` for a descriptor that is not a file, which is `mmap`'s
 * answer for a console or a socket. */
int fdobjects_file_bytes(int32_t fd, const uint8_t **bytes, uint32_t *size) {
    struct file *f = fd_get(fd);
    const struct diskfile *d;
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops != &diskfile_ops) {
        return KERR_NODEV;
    }
    d = f->priv;
    *bytes = d->bytes;
    *size = d->size;
    return KERR_OK;
}

struct file *fdobjects_file_of(int32_t fd) {
    struct file *f = fd_get(fd);
    return f != NULL && f->ops == &diskfile_ops ? f : NULL;
}

/* The same question asked of the object rather than a descriptor, which is
 * what a *mapping* has: it holds the file itself, precisely so that it does
 * not depend on a descriptor number that the program may have closed. */
int fdobjects_file_of_object(void *object, const uint8_t **bytes, uint32_t *size) {
    const struct file *f = object;
    const struct diskfile *d;
    if (f == NULL || f->ops != &diskfile_ops) {
        return KERR_NODEV;
    }
    d = f->priv;
    *bytes = d->bytes;
    *size = d->size;
    return KERR_OK;
}

/* Opens a regular file, if the caller may read it.
 *
 * **The permission rule's first caller.** `access_permitted` and
 * `uspace_may_access` were written with §1's identity model and nothing
 * asked them anything: there was no file to be denied. Opening one by name
 * is exactly where the owner/group/other bits mean something, and the mode
 * comes from the filesystem's own inode rather than from a guess. */
static int open_directory(const char *path);
static int open_for_writing(const char *path, uint32_t flags);

static int open_regular(const char *path, uint32_t flags) {
    uint32_t uid = 0, gid = 0, mode = 0;
    if ((flags & O_ACCMODE) != O_RDONLY) {
        /* **Re-enabled, and the evidence is worth keeping.**
         *
         * This was switched off after the operator's SD card went from 8
         * root entries to 1116 with names that are not text. The suspicion
         * fell here, on the zero-length create at open and the chunked
         * writes at increasing offsets that a Go `io.Copy` produces, because
         * no other caller does either.
         *
         * Both are clean. `minikern.chunked` does exactly that sequence
         * against a disposable fixture image and the root gains **one**
         * entry for one file, with every call returning success — so the
         * pattern is not what corrupted anything.
         *
         * What did: the fixture runs over a RAM disk and the card runs
         * through `dwmmc`, and those pushes happened while **CMD24 was
         * timing out and the driver was issuing commands to a card still in
         * its programming state**. That is the defect `CMD13` polling now
         * prevents, and with it the whole write suite passes on the card.
         * The damage was done before the fix, by the driver, not by this.
         *
         * **Decided before the file is looked for**, because the stat below
         * would otherwise fail with `ENOENT` on a file that does not exist
         * yet — the one case `O_CREAT` is for. */
        return open_for_writing(path, flags);
    }
    {
        /* **A directory is a different kind of descriptor.** It used to fall
         * through to the file path and be refused as unreadable, so
         * `os.ReadDir` failed at the open and never reached `getdents64` —
         * which made a listing impossible for a reason that had nothing to
         * do with listing. */
        uint32_t dmode = 0, dnlink = 0;
        uint64_t dsize = 0;
        if (xfsmount_stat_full(path, &dmode, &dsize, &dnlink) == KERR_OK &&
            (dmode & 0170000u) == 0040000u) {
            return open_directory(path);
        }
    }
    uint8_t *bytes = NULL;
    uint32_t size = 0;
    struct diskfile *d;
    struct file *f;
    int rc = xfsmount_stat(path, &uid, &gid, &mode);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!uspace_may_access(uid, gid, mode, ACCESS_READ)) {
        return KERR_ACCES;
    }

    d = diskfile_alloc();
    rc = d == NULL ? KERR_NOMEM : xfsmount_read_file(path, DISKFILE_MAX_BYTES, &bytes, &size);
    if (rc != KERR_OK) {
        if (d != NULL) {
            d->used = false;
        }
        return rc;
    }
    d->bytes = bytes;
    d->size = size;
    d->offset = 0;
    f = file_new(&diskfile_ops, d, flags);
    if (f == NULL) {
        diskfile_release_buffer(d);
        return KERR_NOMEM;
    }
    return fd_install(f);
}

/* ── a file opened for writing ─────────────────────────────────────────── */

/* A file being written: its directory, its name, and the bytes so far.
 *
 * **Buffered and committed on close**, which is the same trade `diskfile`
 * makes in the other direction — it reads a whole file into memory and hands
 * it out. The writer the filesystem exposes takes a complete file rather
 * than an offset and a length, so a descriptor that wrote through on every
 * `write` would rewrite the whole file per call: a Go `io.Copy` moves 32 KiB
 * at a time, so pushing a 3.5 MB binary would write 120 MB.
 *
 * What that costs, said plainly: a file is not on the card until the
 * descriptor is closed, so a crash in between loses it, and a file larger
 * than the buffer is refused rather than truncated. Both are acceptable for
 * what this is — a way to put a binary on the board — and neither is a
 * surprise a caller discovers from corrupt data.
 *
 * The path is split once, at open, because the filesystem wants a directory
 * and a leaf; splitting per write would do the same work repeatedly and
 * give two chances to disagree about where the separator is. */
/* A path split once, and how far the writer has got. **No buffer at all**:
 * each `write` goes straight to the filesystem at its offset.
 *
 * The first version held the whole file and committed on close, which meant
 * a 16 MiB allocation per open descriptor and wedged the service after one
 * file. An offset-taking writer already existed in the mount layer; it was
 * only root-relative, which is the single reason it was not used. */
#define WRITEFILE_PATH_MAX 128u

struct writefile {
    char dir[WRITEFILE_PATH_MAX];
    char leaf[WRITEFILE_PATH_MAX];
    char path[WRITEFILE_PATH_MAX * 2u]; /* dir and leaf together, for stat */
    uint64_t offset;
    bool append; /* O_APPEND: every write goes to the end as it is now */
    bool readable; /* O_RDWR: reads go to the file at the offset too */
    uint64_t identity; /* reclock_identity of the path it was opened as */
    bool orphan; /* unlinked while open: removed at the last close */
    bool used;
};

static struct writefile writefiles[FD_MAX];

/* Splits a path into its directory and its final component. */
static int split_path(const char *path, char *dir, char *leaf) {
    uint32_t cut = 0, n = 0;
    for (uint32_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            cut = i;
        }
        n = i + 1u;
    }
    if (n == 0 || path[n - 1u] == '/' || n - cut - 1u >= WRITEFILE_PATH_MAX ||
        cut + 1u >= WRITEFILE_PATH_MAX) {
        return KERR_INVAL;
    }
    for (uint32_t i = 0; i < cut; i++) {
        dir[i] = path[i];
    }
    dir[cut] = '\0';
    /* A file directly in the root has an empty directory part, which the
     * filesystem spells "/" and not "". Getting this wrong produced
     * `//pushed.txt` in a diagnostic, which was the clue that the two
     * halves disagreed about who owns the separator. */
    if (cut == 0) {
        dir[0] = '/';
        dir[1] = '\0';
    }
    for (uint32_t i = 0; i < n - cut - 1u; i++) {
        leaf[i] = path[cut + 1u + i];
    }
    leaf[n - cut - 1u] = '\0';
    return KERR_OK;
}

static void copy_path(const char *from, char *to, uint32_t room) {
    uint32_t i = 0;
    while (from[i] != '\0' && i + 1u < room) {
        to[i] = from[i];
        i++;
    }
    to[i] = '\0';
}

/* How much of a write is staged in the kernel at once. */
#define WRITE_BOUNCE 16384u

/* Writes n bytes of the caller's buffer at w's offset **through a kernel
 * copy**, a chunk at a time: a write over netfs sleeps while the reply
 * comes back, and the caller's buffer must not be read after that (a
 * sibling thread may have unmapped it). */
static int write_bounced(struct writefile *w, const void *buf, uint32_t n, uint64_t at) {
    uint8_t *bounce = kzalloc(n < WRITE_BOUNCE ? (n == 0 ? 1u : n) : WRITE_BOUNCE);
    uint32_t done = 0;
    int rc = bounce == NULL ? KERR_NOMEM : KERR_OK;
    if (rc == KERR_OK && n == 0) {
        rc = xfsmount_write_at(w->dir, w->leaf, at, NULL, 0);
    }
    while (rc == KERR_OK && done < n) {
        uint32_t chunk = n - done < WRITE_BOUNCE ? n - done : WRITE_BOUNCE;
        rc = uspace_copy_in(bounce, (virt_t)(uintptr_t)buf + done, chunk) == KERR_OK
                 ? xfsmount_write_at(w->dir, w->leaf, at + done, bounce, chunk)
                 : KERR_FAULT;
        done += chunk;
    }
    kfree(bounce);
    return rc;
}

static int32_t writefile_write(struct file *f, const void *buf, uint32_t n) {
    struct writefile *w = f->priv;
    int rc;
    if (w == NULL) {
        return KERR_BADF;
    }
    if (w->append) {
        /* **The end as it is now**, not as it was at open: another writer
         * may have grown the file since, and appending means after it. */
        uint32_t mode = 0, nlink = 0;
        uint64_t size = 0;
        rc = xfsmount_stat_full(w->path, &mode, &size, &nlink);
        if (rc != KERR_OK) {
            return rc;
        }
        w->offset = size;
    }
    rc = write_bounced(w, buf, n, w->offset);
    if (rc != KERR_OK) {
        return rc;
    }
    w->offset += n;
    return (int32_t)n;
}

/* Reads n bytes at `at` into the caller's buffer **through a kernel copy**,
 * for the reason `write_bounced` writes through one: the read can sleep on
 * the network, and the caller's mapping may be gone when it returns. */
static int32_t read_bounced(const struct writefile *w, void *buf, uint32_t n, uint64_t at) {
    uint8_t *bounce = kzalloc(n < WRITE_BOUNCE ? (n == 0 ? 1u : n) : WRITE_BOUNCE);
    uint32_t done = 0;
    int rc = bounce == NULL ? KERR_NOMEM : KERR_OK;
    while (rc == KERR_OK && done < n) {
        uint32_t chunk = n - done < WRITE_BOUNCE ? n - done : WRITE_BOUNCE, got = 0;
        rc = xfsmount_read_path_at(w->path, at + done, bounce, chunk, &got);
        if (rc == KERR_OK && got > 0 &&
            uspace_copy_out((virt_t)(uintptr_t)buf + done, bounce, got) != KERR_OK) {
            rc = KERR_FAULT;
        }
        done += rc == KERR_OK ? got : 0u;
        if (got < chunk) {
            break; /* the end of the file */
        }
    }
    kfree(bounce);
    return rc != KERR_OK ? rc : (int32_t)done;
}

/* A read-write descriptor reads the file at its offset; one opened for
 * writing only refuses, as Linux does. It used to refuse both, so SQLite —
 * which opens its database `O_RDWR` and reads it a page at a time — could
 * not read a byte of a database it had just written. */
static int32_t writefile_read(struct file *f, void *buf, uint32_t n) {
    struct writefile *w = f->priv;
    int32_t got;
    if (w == NULL || !w->readable) {
        return KERR_BADF;
    }
    got = read_bounced(w, buf, n, w->offset);
    w->offset += got > 0 ? (uint64_t)got : 0u;
    return got;
}

static int32_t writefile_pread(struct file *f, void *buf, uint32_t n, uint64_t off) {
    const struct writefile *w = f->priv;
    return w == NULL || !w->readable ? KERR_BADF : read_bounced(w, buf, n, off);
}

/* `pwrite64`: at `off` and nowhere else — Linux ignores O_APPEND here only
 * through a long-standing bug, and nothing on this board appends this way. */
static int32_t writefile_pwrite(struct file *f, const void *buf, uint32_t n, uint64_t off) {
    struct writefile *w = f->priv;
    int rc = w == NULL ? KERR_BADF : write_bounced(w, buf, n, off);
    return rc != KERR_OK ? rc : (int32_t)n;
}

/* ── unlinked while open ───────────────────────────────────────────────
 *
 * A descriptor here holds a path, not an inode, so a file removed while a
 * writer has it open would be CREATED AGAIN by the next write or
 * `ftruncate` — the name a program just removed coming back. The answer is
 * NFS's "silly rename": the file moves to a hidden name beside it, every
 * descriptor follows, and the last close removes it. */
#define ORPHAN_PREFIX ".deleted-"

static bool same_path(const char *a, const char *b);

static uint32_t orphans_made;

/* How many write descriptors hold `path`. */
static uint32_t writers_of(const char *path) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < FD_MAX; i++) {
        n += writefiles[i].used && same_path(writefiles[i].path, path) ? 1u : 0u;
    }
    return n;
}

/* Appends text at out[*at], within room. */
static void append_text(char *out, uint32_t *at, uint32_t room, const char *text) {
    for (uint32_t i = 0; text[i] != '\0' && *at + 1u < room; i++) {
        out[(*at)++] = text[i];
    }
    out[*at] = '\0';
}

/* The hidden name `path` moves to: its directory, ".deleted-<n>". */
static int orphan_name(const char *path, char out[WRITEFILE_PATH_MAX * 2u]) {
    char dir[WRITEFILE_PATH_MAX], leaf[WRITEFILE_PATH_MAX], number[12];
    uint32_t n = ++orphans_made, at = 0, d = sizeof number - 1u;
    if (split_path(path, dir, leaf) != KERR_OK) {
        return KERR_INVAL;
    }
    number[d] = '\0';
    do {
        number[--d] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n != 0 && d > 0);
    append_text(out, &at, WRITEFILE_PATH_MAX * 2u, dir);
    append_text(out, &at, WRITEFILE_PATH_MAX * 2u, at == 1u && out[0] == '/' ? "" : "/");
    append_text(out, &at, WRITEFILE_PATH_MAX * 2u, ORPHAN_PREFIX);
    append_text(out, &at, WRITEFILE_PATH_MAX * 2u, number + d);
    return KERR_OK;
}

/* Points every writer of `from` at `to`, marking them orphans. */
static void follow_orphan(const char *from, const char *to) {
    char was[WRITEFILE_PATH_MAX * 2u];
    copy_path(from, was, sizeof was);
    for (uint32_t i = 0; i < FD_MAX; i++) {
        struct writefile *w = &writefiles[i];
        if (w->used && same_path(w->path, was) && split_path(to, w->dir, w->leaf) == KERR_OK) {
            copy_path(to, w->path, sizeof w->path);
            w->orphan = true;
        }
    }
}

int fdobjects_unlink(const char *path) {
    char hidden[WRITEFILE_PATH_MAX * 2u];
    int rc;
    if (writers_of(path) == 0) {
        return xfsmount_unlink_path(path);
    }
    rc = orphan_name(path, hidden);
    rc = rc != KERR_OK ? rc : xfsmount_rename_path(path, hidden);
    if (rc == KERR_OK) {
        follow_orphan(path, hidden);
    }
    return rc;
}

bool fdobjects_is_orphan_name(const char *name, uint32_t len) {
    uint32_t p = sizeof ORPHAN_PREFIX - 1u;
    for (uint32_t i = 0; i < p; i++) {
        if (i >= len || name[i] != ORPHAN_PREFIX[i]) {
            return false;
        }
    }
    return len > p;
}

static void writefile_release(struct file *f) {
    struct writefile *w = f->priv;
    if (w == NULL) {
        return;
    }
    w->used = false;
    if (w->orphan && writers_of(w->path) == 0) {
        (void)xfsmount_unlink_path(w->path); /* the last close of a removed file */
    }
    w->orphan = false;
}

static const struct file_ops writefile_ops = {
    .read = writefile_read,
    .write = writefile_write,
    .release = writefile_release,
    .pread = writefile_pread,
    .pwrite = writefile_pwrite,
};

/* Opens a path for writing, creating the file at once.
 *
 * **Created on open rather than on the first write**, so that a program
 * which opens a file and writes nothing still leaves an empty file — which
 * is what `O_CREAT` means and what every caller assumes. `O_TRUNC` empties
 * an existing file and `O_APPEND` writes at its end; both were ignored, so
 * every open wrote from byte 0 over what was there — QuokkaOS's service
 * logs, opened for appending, kept only their last write. */
/* Whether `path` may be opened for writing with these flags: write
 * permission (perm.h), O_CREAT needed to create, O_EXCL refusing an
 * existing file, and no directories. */
static int writable_open(const char *path, uint32_t flags) {
    uint32_t mode = 0, nlink = 0;
    uint64_t size = 0;
    bool exists = false;
    int rc = perm_may_write(path, &exists);
    if (rc != KERR_OK) {
        return rc;
    }
    if (!exists) {
        return (flags & O_CREAT) != 0 ? KERR_OK : KERR_NOENT;
    }
    if ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        return KERR_EXIST;
    }
    rc = xfsmount_stat_full(path, &mode, &size, &nlink);
    return rc == KERR_OK && (mode & 0170000u) == 0040000u ? KERR_ISDIR : rc;
}

/* A free writefile slot, claimed at once (the create below can sleep). */
static struct writefile *writefile_claim(void) {
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (!writefiles[i].used) {
            writefiles[i].used = true;
            return &writefiles[i];
        }
    }
    return NULL;
}

static int open_for_writing(const char *path, uint32_t flags) {
    struct writefile *w;
    struct file *f;
    int rc = writable_open(path, flags);
    if (rc != KERR_OK) {
        return rc;
    }
    w = writefile_claim();
    if (w == NULL) {
        return KERR_NOMEM;
    }
    rc = split_path(path, w->dir, w->leaf) != KERR_OK ? KERR_INVAL : KERR_OK;
    rc = rc != KERR_OK ? rc : xfsmount_write_at(w->dir, w->leaf, 0, NULL, 0);
    rc = rc != KERR_OK || (flags & O_TRUNC) == 0 ? rc : xfsmount_truncate_path(path, 0);
    f = rc == KERR_OK ? file_new(&writefile_ops, w, O_WRONLY) : NULL;
    if (f == NULL) {
        w->used = false;
        return rc != KERR_OK ? rc : KERR_NOMEM;
    }
    copy_path(path, w->path, sizeof w->path);
    w->offset = 0;
    w->append = (flags & O_APPEND) != 0;
    w->readable = (flags & O_ACCMODE) == O_RDWR;
    w->identity = reclock_identity(path);
    return fd_install(f);
}

/* ── a directory, as a descriptor ──────────────────────────────────────── */

/* An open directory: the path it names and how far a reader has got.
 *
 * **The cursor is a count of entries handed over, not a byte offset.** A
 * `getdents64` reply is a sequence of variable-length records, so a byte
 * offset into one reply means nothing on the next call — and the filesystem
 * walks a directory by callback rather than by index, which is why each call
 * re-walks and skips. That is O(n²) over a directory read in full, and the
 * right trade here: directories on this board are tens of entries, and the
 * alternative is holding a snapshot of every open directory in kernel memory
 * for as long as a descriptor lives.
 *
 * The path is kept because the filesystem resolves by name; there is no
 * inode handle to hold, and holding one would have to be invalidated by
 * every write. */
#define DIRFILE_PATH_MAX 128u

struct dirfile {
    char path[DIRFILE_PATH_MAX];
    uint32_t handed_over;
    bool used;
};

static struct dirfile dirfiles[FD_MAX];

static struct dirfile *dirfile_alloc(const char *path) {
    for (uint32_t i = 0; i < FD_MAX; i++) {
        if (dirfiles[i].used) {
            continue;
        }
        {
            uint32_t n = 0;
            while (path[n] != '\0' && n + 1u < DIRFILE_PATH_MAX) {
                dirfiles[i].path[n] = path[n];
                n++;
            }
            if (path[n] != '\0') {
                return NULL; /* too long to remember; refused rather than cut */
            }
            dirfiles[i].path[n] = '\0';
        }
        dirfiles[i].handed_over = 0;
        dirfiles[i].used = true;
        return &dirfiles[i];
    }
    return NULL;
}

/* `struct linux_dirent64`, by offset rather than as a struct — the same
 * reasoning as `struct stat` in `syscall.c`: a declaration here would have
 * to match the ABI's padding exactly, and the one time that was assumed
 * rather than laid out, a Go runtime read a field half a field late. */
#define DIRENT_INO 0u
#define DIRENT_OFF 8u
#define DIRENT_RECLEN 16u
#define DIRENT_TYPE 18u
#define DIRENT_NAME 19u
#define DIRENT_FIXED 19u
#define DT_DIR 4u
#define DT_REG 8u
#define DT_UNKNOWN 0u
#define DT_LNK 10u
#define DT_CHR 2u

/* What one `getdents64` call is filling. */
struct dirent_fill {
    uint8_t *at;
    uint32_t room;
    uint32_t used;
    uint32_t skip;
    uint32_t seen;
    bool full;
};

static void put_le16(uint8_t *at, uint16_t v) {
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
}

static void put_le64(uint8_t *at, uint64_t v) {
    for (uint32_t i = 0; i < 8u; i++) {
        at[i] = (uint8_t)(v >> (i * 8u));
    }
}

/* The filesystem's type byte as getdents's `d_type`. A symbolic link left
 * UNKNOWN made a caller stat it — and stat follows it, so a link was
 * reported as the file it names and copied as one. */
static uint8_t dirent_type(uint8_t ft) {
    switch (ft) {
    case XFS_DIR3_FT_DIR: return (uint8_t)DT_DIR;
    case XFS_DIR3_FT_REG: return (uint8_t)DT_REG;
    case XFS_DIR3_FT_SYMLINK: return (uint8_t)DT_LNK;
    case XFS_DIR3_FT_CHRDEV: return (uint8_t)DT_CHR;
    default: return (uint8_t)DT_UNKNOWN;
    }
}

/* Appends one entry, or marks the buffer full.
 *
 * **A record that does not fit is not written at all**, and the entry is not
 * counted as handed over — so the next call starts with it. A partial record
 * would be read as a complete one with a wrong length, which is how a reader
 * walks off the end of its own buffer. */
static int add_dirent(void *self, const struct xfs_dirent *entry) {
    struct dirent_fill *f = self;
    uint32_t reclen;
    uint32_t namelen = entry->name_len;
    if (fdobjects_is_orphan_name(entry->name, namelen)) {
        return 0; /* a removed file still open somewhere: not a name any more */
    }
    f->seen++;
    if (f->seen <= f->skip) {
        return 0; /* already handed to an earlier call */
    }
    if (namelen > 255u) {
        return 0; /* a name this reader cannot represent: skipped, not fatal */
    }
    /* Rounded up to eight, which the ABI requires: a reader steps by
     * `d_reclen` and an unaligned record misaligns every one after it. */
    reclen = (DIRENT_FIXED + namelen + 1u + 7u) & ~7u;
    if (f->used + reclen > f->room) {
        f->full = true;
        return 1; /* stop the walk; the rest is for the next call */
    }
    {
        uint8_t *rec = f->at + f->used;
        for (uint32_t i = 0; i < reclen; i++) {
            rec[i] = 0;
        }
        put_le64(rec + DIRENT_INO, entry->ino);
        /* `d_off` is "where to resume", and a count of entries is exactly
         * that for this implementation. A reader that seeks by it gets the
         * same place back. */
        put_le64(rec + DIRENT_OFF, f->seen);
        put_le16(rec + DIRENT_RECLEN, (uint16_t)reclen);
        /* The filesystem's own type byte, mapped. A filesystem without
         * `ftype` reports UNKNOWN for everything, and passing that through
         * is right: a caller then stats what it cares about, which is what
         * `os.ReadDir` does anyway. Inventing `DT_REG` would make every
         * directory on such a filesystem look like a file. */
        rec[DIRENT_TYPE] = dirent_type(entry->type);
        for (uint32_t i = 0; i < namelen; i++) {
            rec[DIRENT_NAME + i] = (uint8_t)entry->name[i];
        }
    }
    f->used += reclen;
    return 0;
}

static int32_t dirfile_read(struct file *f, void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    /* A directory is read with `getdents64`, not with `read`. Linux answers
     * `EISDIR` here and so does this: a program that reads a directory as
     * bytes is doing something that has not worked since the 1980s. */
    return KERR_ISDIR;
}

static int32_t dirfile_write_refused(struct file *f, const void *buf, uint32_t n) {
    (void)f;
    (void)buf;
    (void)n;
    return KERR_ISDIR;
}

static void dirfile_release(struct file *f) {
    struct dirfile *d = f->priv;
    if (d != NULL) {
        d->used = false;
    }
}

/* `FITRIM`, `_IOWR('X', 121, struct fstrim_range)`: what `fstrim` sends
 * the mount point it opened. */
#define FITRIM 0xc0185879u

struct fstrim_range {
    uint64_t start;
    uint64_t len;    /* in: the range; out: the bytes trimmed */
    uint64_t minlen;
};

/* A directory answers `FITRIM` for the volume it is on, and nothing else.
 * `CAP_SYS_ADMIN`, as on Linux: telling a drive to forget blocks is the
 * administrator's call, and costs every other user the device's time. */
static int32_t dirfile_ioctl(struct file *f, uint32_t request, virt_t arg) {
    const struct dirfile *d = f->priv;
    struct fstrim_range r;
    uint64_t trimmed = 0;
    int rc;
    if (request != FITRIM) {
        return KERR_NOTTY;
    }
    if (!uspace_capable(CAP_SYS_ADMIN)) {
        return KERR_PERM;
    }
    if (uspace_copy_in(&r, arg, sizeof r) != KERR_OK) {
        return KERR_FAULT;
    }
    rc = xfsmount_trim(d->path, r.start, r.len, r.minlen, &trimmed);
    r.len = trimmed;
    if (rc == KERR_OK && uspace_copy_out(arg, &r, sizeof r) != KERR_OK) {
        return KERR_FAULT;
    }
    return rc;
}

static const struct file_ops dirfile_ops = {
    .read = dirfile_read,
    .write = dirfile_write_refused,
    .ioctl = dirfile_ioctl,
    .release = dirfile_release,
};

const char *fdobjects_dir_path(const struct file *f) {
    const struct dirfile *d = f == NULL ? NULL : f->priv;
    return f != NULL && f->ops == &dirfile_ops && d != NULL ? d->path : NULL;
}

int32_t fdobjects_getdents(struct file *f, void *buf, uint32_t room) {
    struct dirfile *d = f == NULL ? NULL : f->priv;
    struct dirent_fill fill = {0};
    int rc;
    if (f == NULL || f->ops != &dirfile_ops || d == NULL) {
        return KERR_NOTDIR;
    }
    fill.at = buf;
    fill.room = room;
    fill.skip = d->handed_over;
    rc = xfsmount_list(d->path, add_dirent, &fill);
    /* A walk stopped by a full buffer is not a failure: `add_dirent`
     * returning non-zero is how `xfs_dir_list` is told to stop, and that is
     * the same signal a real error uses — so the buffer's own flag is what
     * distinguishes them. */
    if (rc != KERR_OK && !fill.full) {
        return rc;
    }
    d->handed_over = fill.seen - (fill.full ? 1u : 0u);
    return (int32_t)fill.used;
}

/* Opens a directory, for `getdents64`. */
static int open_directory(const char *path) {
    struct dirfile *d = dirfile_alloc(path);
    struct file *f;
    if (d == NULL) {
        return KERR_NOMEM;
    }
    f = file_new(&dirfile_ops, d, O_RDONLY);
    if (f == NULL) {
        d->used = false;
        return KERR_NOMEM;
    }
    return fd_install(f);
}

static bool same_path(const char *a, const char *b) {
    uint32_t i = 0;
    while (a[i] != '\0' && a[i] == b[i]) {
        i++;
    }
    return a[i] == b[i];
}

/* A snapshot file: `room` bytes filled by `fill`, its own per descriptor. */
static int open_snapshot(const struct file_ops *ops, uint32_t room, const char *path,
                         int (*fill)(const char *path, struct snapshot *snap, uint32_t room),
                         uint32_t flags) {
    struct snapshot *snap = kzalloc((uint32_t)sizeof *snap + room);
    struct file *f;
    int rc;
    if (snap == NULL) {
        return KERR_NOMEM;
    }
    rc = fill(path, snap, room);
    f = rc == KERR_OK ? file_new(ops, snap, flags) : NULL;
    if (f == NULL) {
        kfree(snap);
        return rc != KERR_OK ? rc : KERR_NOMEM;
    }
    return fd_install(f);
}

static int fill_klog(const char *path, struct snapshot *snap, uint32_t room) {
    (void)path;
    snap->len = klog_tail(snap->text, room);
    return KERR_OK;
}

static int fill_proc(const char *path, struct snapshot *snap, uint32_t room) {
    return procfs_generate(path, snap->text, room, &snap->len) == KERR_OK ? KERR_OK : KERR_NOENT;
}

static int fill_resolv(const char *path, struct snapshot *snap, uint32_t room) {
    (void)path;
    (void)room;
    fill_resolv_snapshot(snap);
    return KERR_OK;
}

/* Devices drivers registered (`fdobjects_add_device`). Few: one per piece
 * of hardware with a node, so a small fixed table. */
#define DRIVER_DEVICES_MAX 8u

struct driver_device {
    const char *path;
    const struct file_ops *ops;
    void *priv;
};

static struct driver_device driver_devices[DRIVER_DEVICES_MAX];
static uint32_t driver_device_count;

static const struct driver_device *driver_device(const char *path) {
    for (uint32_t i = 0; i < driver_device_count; i++) {
        if (same_path(path, driver_devices[i].path)) {
            return &driver_devices[i];
        }
    }
    return NULL;
}

int fdobjects_add_device(const char *path, const struct file_ops *ops, void *priv) {
    if (driver_device(path) != NULL) {
        return KERR_EXIST;
    }
    if (driver_device_count >= DRIVER_DEVICES_MAX) {
        return KERR_NOMEM;
    }
    driver_devices[driver_device_count++] = (struct driver_device){path, ops, priv};
    return KERR_OK;
}

/* Opens a driver's device. */
static int open_driver_device(const struct driver_device *d, uint32_t flags) {
    struct file *f = file_new(d->ops, d->priv, flags);
    return f == NULL ? KERR_NOMEM : fd_install(f);
}

/* The kernel's own device objects by path, or NULL. */
static const struct file_ops *device_ops(const char *path) {
    if (same_path(path, "/dev/null")) {
        return &null_ops;
    }
    if (same_path(path, "/dev/zero")) {
        return &zero_ops;
    }
    if (same_path(path, "/dev/urandom") || same_path(path, "/dev/random")) {
        return &random_ops;
    }
    return same_path(path, "/dev/console") || same_path(path, "/dev/tty") ? &console_ops : NULL;
}

int fdobjects_open_path(const char *path, uint32_t flags) {
    const struct file_ops *ops = device_ops(path);
    const struct driver_device *driver = driver_device(path);
    struct file *f;
    if (driver != NULL) {
        return open_driver_device(driver, flags);
    }
    if (ops != NULL) {
        f = file_new(ops, NULL, flags);
        return f == NULL ? KERR_NOMEM : fd_install(f);
    }
    if (same_path(path, "/dev/virtual/kernel_logger")) {
        return open_snapshot(&klog_ops, KLOG_SNAPSHOT_BYTES, path, fill_klog, flags);
    }
    if (procfs_has(path)) {
        return open_snapshot(&procfile_ops, PROCFS_MAX_BYTES, path, fill_proc, flags);
    }
    if (same_path(path, "/etc/resolv.conf")) {
        /* **The filesystem first.** An operator's own file is a decision
         * this kernel cannot see and must not override; the synthesised one
         * is for when there is none, which on a DHCP-configured machine
         * with a read-only root is every boot. */
        int real = open_regular(path, flags);
        return real >= 0 ? real
                         : open_snapshot(&resolv_ops, RESOLV_SNAPSHOT_BYTES, path, fill_resolv, flags);
    }
    return open_regular(path, flags);
}

/* ── a pipe ─────────────────────────────────────────────────────────────── */

/* A ring, shared by the two ends, and a count of how many ends are still
 * open — which is what makes a read return end-of-file rather than blocking
 * for ever once the writer has gone, and what makes a write fail rather than
 * filling a buffer nobody will drain.
 *
 * 4 KiB because that is the capacity a program may assume; it is also what
 * makes a report from a failed `exec` fit in one write. */
#define PIPE_BYTES 4096u

struct pipe {
    uint8_t data[PIPE_BYTES];
    uint32_t head;
    uint32_t count;
    bool reader_open;
    bool writer_open;
    /* Both ends, so a change at one is announced at the **other**: a
     * reader waits edge-triggered on the read end (Go's poller does), so a
     * write that only marked the write end changed never woke it, and nor
     * did the writer closing — a child's output, and its end of file,
     * never reached the parent. NULL once that end is released. */
    struct file *reader;
    struct file *writer;
};

/* Tells whoever waits on the other end that this end did something. */
static void announce(struct file *other) {
    if (other != NULL) {
        file_readiness_changed(other);
    }
    (void)sched_futex_wake(SCHED_POLLER_KEY, 64u);
}

static uint32_t pipe_used(const struct pipe *pp) {
    return pp->count;
}

static int32_t pipe_read(struct file *f, void *buf, uint32_t n) {
    struct pipe *pp = f->priv;
    uint8_t *to = buf;
    uint32_t took = 0;
    if (pipe_used(pp) == 0) {
        /* **End of file only when the writer has gone.** An empty pipe with
         * a live writer is "not yet", which is `EAGAIN` — reporting 0 there
         * would tell the reader the conversation was over. */
        return pp->writer_open ? KERR_AGAIN : 0;
    }
    while (took < n && pp->count > 0) {
        to[took++] = pp->data[pp->head];
        pp->head = (pp->head + 1u) % PIPE_BYTES;
        pp->count--;
    }
    file_readiness_changed(f);
    announce(pp->writer); /* room to write again */
    return (int32_t)took;
}

static int32_t pipe_write(struct file *f, const void *buf, uint32_t n) {
    struct pipe *pp = f->priv;
    const uint8_t *from = buf;
    uint32_t put = 0;
    if (!pp->reader_open) {
        /* Writing to a pipe nobody will read. Linux also raises SIGPIPE;
         * the error alone is what a runtime checks. */
        return KERR_PIPE;
    }
    while (put < n && pp->count < PIPE_BYTES) {
        pp->data[(pp->head + pp->count) % PIPE_BYTES] = from[put++];
        pp->count++;
    }
    if (put == 0) {
        return KERR_AGAIN; /* full: a short write of zero is a would-block */
    }
    file_readiness_changed(f);
    announce(pp->reader); /* something to read */
    return (int32_t)put;
}

static uint32_t pipe_read_poll(struct file *f) {
    const struct pipe *pp = f->priv;
    uint32_t ready = pipe_used(pp) != 0 ? EPOLLIN : 0u;
    /* A closed writer makes the end readable *and* hung up: a reader must
     * wake to see the end of the stream, not wait for data that cannot
     * come. */
    return ready | (pp->writer_open ? 0u : (EPOLLIN | EPOLLHUP));
}

static uint32_t pipe_write_poll(struct file *f) {
    const struct pipe *pp = f->priv;
    uint32_t ready = pipe_used(pp) < PIPE_BYTES ? EPOLLOUT : 0u;
    return ready | (pp->reader_open ? 0u : EPOLLERR);
}

/* The ring is freed when **both** ends have gone, which is why each end's
 * release only clears its own flag. */
static void pipe_release_read(struct file *f) {
    struct pipe *pp = f->priv;
    pp->reader_open = false;
    pp->reader = NULL;
    announce(pp->writer); /* a writer now gets EPIPE */
    if (!pp->writer_open) {
        kfree(pp);
    }
}

static void pipe_release_write(struct file *f) {
    struct pipe *pp = f->priv;
    pp->writer_open = false;
    pp->writer = NULL;
    announce(pp->reader); /* the reader now sees end of file */
    if (!pp->reader_open) {
        kfree(pp);
    }
}

static const struct file_ops pipe_read_ops = {
    .read = pipe_read,
    .poll = pipe_read_poll,
    .release = pipe_release_read,
};

static const struct file_ops pipe_write_ops = {
    .write = pipe_write,
    .poll = pipe_write_poll,
    .release = pipe_release_write,
};

int fdobjects_open_pipe(int32_t out[2], uint32_t flags) {
    struct pipe *pp = kzalloc(sizeof *pp);
    struct file *r;
    struct file *w;
    if (pp == NULL) {
        return KERR_NOMEM;
    }
    pp->reader_open = true;
    pp->writer_open = true;
    r = file_new(&pipe_read_ops, pp, flags);
    w = file_new(&pipe_write_ops, pp, flags);
    if (r == NULL || w == NULL) {
        file_put(r);
        file_put(w);
        kfree(pp);
        return KERR_NOMEM;
    }
    pp->reader = r;
    pp->writer = w;
    out[0] = (int32_t)fd_install(r);
    out[1] = (int32_t)fd_install(w);
    if (out[0] < 0 || out[1] < 0) {
        (void)fd_close(out[0]);
        (void)fd_close(out[1]);
        return KERR_MFILE;
    }
    return KERR_OK;
}

/* ── the epoll instance ─────────────────────────────────────────────────── */

#define EPOLL_WATCHED 32u

struct epoll_watch {
    int32_t fd;
    bool used;
    uint32_t events; /* what the caller is interested in */
    uint64_t data;   /* the caller's token, returned unchanged */
    /* Edge-triggered (`EPOLLET`) only: the file's readiness generation when
     * this watch was last reported, and the readiness last seen. */
    uint32_t reported_gen;
    uint32_t seen;
    bool reported;
};

struct epoll {
    struct epoll_watch watch[EPOLL_WATCHED];
};

static void epoll_release(struct file *f) {
    kfree(f->priv);
}

/* An epoll instance is itself pollable — a program may put one inside
 * another — and it is readable when anything it watches is ready. */
static uint32_t epoll_poll(struct file *f) {
    const struct epoll *ep = f->priv;
    for (uint32_t i = 0; i < EPOLL_WATCHED; i++) {
        struct file *watched;
        if (!ep->watch[i].used) {
            continue;
        }
        watched = fd_get(ep->watch[i].fd);
        if (watched != NULL && (file_poll(watched) & ep->watch[i].events) != 0) {
            return EPOLLIN;
        }
    }
    return 0;
}

static const struct file_ops epoll_ops = {
    .poll = epoll_poll,
    .release = epoll_release,
};

int fdobjects_open_epoll(uint32_t flags) {
    struct epoll *ep = kzalloc(sizeof *ep);
    struct file *f;
    if (ep == NULL) {
        return KERR_NOMEM;
    }
    f = file_new(&epoll_ops, ep, flags);
    if (f == NULL) {
        kfree(ep);
        return KERR_NOMEM;
    }
    return fd_install(f);
}

/* The instance a descriptor names, or NULL when it is not one. */
static struct epoll *epoll_of(int32_t epfd) {
    struct file *f = fd_get(epfd);
    return f != NULL && f->ops == &epoll_ops ? f->priv : NULL;
}

static struct epoll_watch *find_watch(struct epoll *ep, int32_t fd) {
    for (uint32_t i = 0; i < EPOLL_WATCHED; i++) {
        if (ep->watch[i].used && ep->watch[i].fd == fd) {
            return &ep->watch[i];
        }
    }
    return NULL;
}

int fdobjects_epoll_ctl(int32_t epfd, int32_t op, int32_t fd,
                        const struct user_epoll_event *event) {
    struct epoll *ep = epoll_of(epfd);
    struct epoll_watch *w;
    if (ep == NULL) {
        return KERR_BADF;
    }
    if (fd_get(fd) == NULL) {
        return KERR_BADF;
    }
    w = find_watch(ep, fd);
    switch (op) {
    case EPOLL_CTL_DEL:
        if (w == NULL) {
            return KERR_NOENT;
        }
        w->used = false;
        return KERR_OK;
    case EPOLL_CTL_MOD:
        if (w == NULL) {
            return KERR_NOENT;
        }
        break;
    case EPOLL_CTL_ADD:
        if (w != NULL) {
            return KERR_EXIST;
        }
        for (uint32_t i = 0; i < EPOLL_WATCHED && w == NULL; i++) {
            w = ep->watch[i].used ? NULL : &ep->watch[i];
        }
        if (w == NULL) {
            return KERR_NOSPC;
        }
        break;
    default:
        return KERR_INVAL;
    }
    if (event == NULL) {
        return KERR_FAULT;
    }
    /* **`EPOLLERR` and `EPOLLHUP` are always reported**, whether or not they
     * were asked for — that is the interface's rule, and a runtime relies on
     * it to notice a connection that failed rather than one that became
     * readable. */
    *w = (struct epoll_watch){.fd = fd,
                              .used = true,
                              .events = event->events | EPOLLERR | EPOLLHUP,
                              .data = event->data};
    return KERR_OK;
}

/* Whether a watch is reported now. Level-triggered: whenever it is ready.
 * Edge-triggered (`EPOLLET`, which Go's runtime asks for on every
 * descriptor): only once per change — the file's readiness generation has
 * moved since this watch was last reported, or a bit is set that was clear
 * the last time anybody looked (for the console, which has no generation).
 * Without this a descriptor that simply stays writable was reported on every
 * call, and an idle Go program's poller spun at 23,000 `epoll_pwait`s a
 * second. The readiness is noted even when nothing is ready, so that a rise
 * after a fall is an edge. */
static bool is_reported(struct epoll_watch *w, const struct file *f, uint32_t ready) {
    bool edge;
    if ((w->events & EPOLLET) == 0) {
        return ready != 0;
    }
    edge = ready != 0 &&
           (!w->reported || f->poll_gen != w->reported_gen || (ready & ~w->seen) != 0);
    w->seen = ready;
    if (edge) {
        w->reported = true;
        w->reported_gen = f->poll_gen;
    }
    return edge;
}

int fdobjects_epoll_collect(int32_t epfd, struct user_epoll_event *out, uint32_t max) {
    struct epoll *ep = epoll_of(epfd);
    uint32_t found = 0;
    if (ep == NULL) {
        return KERR_BADF;
    }
    for (uint32_t i = 0; i < EPOLL_WATCHED && found < max; i++) {
        struct file *watched;
        uint32_t ready;
        if (!ep->watch[i].used) {
            continue;
        }
        watched = fd_get(ep->watch[i].fd);
        if (watched == NULL) {
            /* The descriptor was closed without being removed, which is a
             * program's mistake rather than a kernel's: drop the watch
             * rather than report on a file that is gone. */
            ep->watch[i].used = false;
            continue;
        }
        ready = file_poll(watched) & ep->watch[i].events;
        if (is_reported(&ep->watch[i], watched, ready)) {
            out[found].events = ready;
            out[found].padding = 0;
            out[found].data = ep->watch[i].data;
            found++;
        }
    }
    return (int)found;
}

bool fdobjects_epoll_has_events(int32_t epfd) {
    struct file *f = fd_get(epfd);
    return f != NULL && f->ops == &epoll_ops && epoll_poll(f) != 0;
}

/* ── seeking ──────────────────────────────────────────────────────────────
 *
 * **Why this is one function rather than an op on every object.** Only two
 * kinds of descriptor here have a position at all — a file opened for
 * reading, which holds its contents and an offset into them, and one opened
 * for writing, which remembers where the next byte goes. Everything else is
 * a stream: a console, a pipe, a socket, an eventfd. `lseek` on a stream is
 * `ESPIPE`, which is what a program checks for to find out whether it may
 * seek at all — so answering it correctly matters as much as seeking does.
 */
/* A write descriptor's file as it is on disk: its end is its size there,
 * not how far this descriptor has written — another descriptor, or a
 * truncate, may have moved it. */
static uint64_t written_size(int32_t fd) {
    const char *path = fdobjects_path_of(fd);
    uint32_t mode = 0, nlink = 0;
    uint64_t size = 0;
    return path != NULL && xfsmount_stat_full(path, &mode, &size, &nlink) == KERR_OK ? size : 0;
}

int fdobjects_seek(int32_t fd, int64_t offset, uint32_t whence, uint64_t *result) {
    /* **The descriptor itself, not `fdobjects_file_of`**, which answers for
     * read descriptors only: every `lseek` on a file opened for writing was
     * `EBADF`, and the write branch below was never reached. */
    struct file *f = fd_get(fd);
    uint64_t base = 0;
    uint64_t size = 0;
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops == &diskfile_ops) {
        struct diskfile *d = f->priv;
        base = d->offset;
        size = d->size;
    } else if (f->ops == &writefile_ops) {
        struct writefile *w = f->priv;
        base = w->offset;
        size = written_size(fd);
    } else {
        return KERR_SPIPE;
    }
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        break;
    case SEEK_END:
        base = size;
        break;
    default:
        return KERR_INVAL;
    }
    /* **A negative result is refused, not clamped.** Clamping to zero would
     * let a program that computed an offset wrongly carry on reading from
     * the start of the file and get plausible data from the wrong place. */
    if (offset < 0 && (uint64_t)(-offset) > base) {
        return KERR_INVAL;
    }
    base = offset < 0 ? base - (uint64_t)(-offset) : base + (uint64_t)offset;
    if (f->ops == &diskfile_ops) {
        ((struct diskfile *)f->priv)->offset = (uint32_t)base;
    } else {
        ((struct writefile *)f->priv)->offset = base;
    }
    *result = base;
    return KERR_OK;
}

/* Two thin wrappers the board self-test uses.
 *
 * The test runs in the kernel, where there is no syscall layer to go
 * through, so it needs the same operations a program would get from `lseek`
 * and `read`. They return the seek result as a signed value because that is
 * what the test compares against `KERR_SPIPE`, and a separate out-parameter
 * would make every call site three lines.
 */
int64_t fdobjects_seek_for_test(int32_t fd, int64_t offset, uint32_t whence) {
    uint64_t where = 0;
    int rc = fdobjects_seek(fd, offset, whence, &where);
    return rc != KERR_OK ? rc : (int64_t)where;
}

int32_t fdobjects_read_for_test(int32_t fd, void *buf, uint32_t n) {
    struct file *f = fdobjects_file_of(fd);
    if (f == NULL || f->ops->read == NULL) {
        return KERR_BADF;
    }
    return f->ops->read(f, buf, n);
}

/* The path a descriptor was opened from, when it remembers one.
 *
 * **Only a descriptor opened for writing does.** One opened for reading
 * holds the file's bytes rather than its name, which is what makes reads
 * cheap and what makes this return NULL for them. That is a real limit and
 * `ftruncate` names it: a read-only descriptor cannot be truncated here,
 * and a file unlinked while open cannot be truncated by name because the
 * name is gone.
 *
 * The path is assembled into a static buffer because the two halves are
 * stored separately and there is nowhere else to join them. One buffer is
 * enough: the caller uses it immediately and the kernel lock is held across
 * the syscall, so a second call cannot be in flight.
 */
static struct writefile *writefile_of(int32_t fd) {
    struct file *f = fd_get(fd);
    return f != NULL && f->ops == &writefile_ops ? f->priv : NULL;
}

int fdobjects_identity_of(int32_t fd, uint64_t *identity) {
    const struct writefile *w = writefile_of(fd);
    if (w == NULL) {
        return KERR_BADF;
    }
    *identity = w->identity;
    return KERR_OK;
}

int fdobjects_stat_of(int32_t fd, uint32_t *mode, uint64_t *size, uint32_t *nlink,
                      uint64_t *ino) {
    const struct writefile *w = writefile_of(fd);
    if (w == NULL) {
        return KERR_BADF;
    }
    *ino = w->identity;
    return xfsmount_stat_full(w->path, mode, size, nlink);
}

int fdobjects_sync(int32_t fd) {
    struct file *f = fd_get(fd);
    if (f == NULL) {
        return KERR_BADF;
    }
    if (f->ops == &writefile_ops) {
        return xfsmount_sync_path(((const struct writefile *)f->priv)->path);
    }
    if (f->ops == &dirfile_ops) {
        return xfsmount_sync_path(((const struct dirfile *)f->priv)->path);
    }
    return f->ops == &diskfile_ops ? xfsmount_flush() : KERR_INVAL;
}

const char *fdobjects_path_of(int32_t fd) {
    static char joined[WRITEFILE_PATH_MAX * 2u];
    struct file *f = fd_get(fd); /* a write descriptor: `fdobjects_file_of` never returns one */
    const struct writefile *w;
    uint32_t at = 0;
    if (f == NULL || f->ops != &writefile_ops) {
        return NULL;
    }
    w = f->priv;
    while (w->dir[at] != '\0' && at + 2u < sizeof joined) {
        joined[at] = w->dir[at];
        at++;
    }
    if (at > 0 && joined[at - 1u] != '/') {
        joined[at++] = '/';
    }
    for (uint32_t i = 0; w->leaf[i] != '\0' && at + 1u < sizeof joined; i++) {
        joined[at++] = w->leaf[i];
    }
    joined[at] = '\0';
    return joined;
}
