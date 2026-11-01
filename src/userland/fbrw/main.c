/* `/dev/fb0` as a file: its position, and who may open it.
 *
 * `read`, `write` and `lseek` move through the pixels as through a file
 * (what `cat /dev/fb0 > shot` and `dd` rely on), and the node is root's and
 * the `video` group's, 0660, so a process outside both cannot read the
 * screen or draw over it. Run with `minikern.vfb=<W>x<H>`; the video group
 * is the kernel's default, 39.
 *
 * The exit status is a bitmask of the checks that **passed**:
 *   1   write() of four bytes at the start returns 4
 *   2   lseek(0, SEEK_SET), then read() gives the same four bytes back
 *   4   and the position after that read is 4 (lseek(0, SEEK_CUR))
 *   8   lseek(0, SEEK_END) is the framebuffer's length (FBIOGET_FSCREENINFO)
 *   16  read() at the end returns 0
 *   32  a child with uid and gid 1000 is refused the open with EACCES
 *   64  a child with uid 1000 in group 39 may open it
 * 127 is all seven.
 */
#include <stdint.h>

#define SYS_IOCTL 29
#define SYS_OPENAT 56
#define SYS_CLOSE 57
#define SYS_LSEEK 62
#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94
#define SYS_SETRESUID 147
#define SYS_SETRESGID 149
#define SYS_CLONE 220
#define SYS_WAIT4 260
#define AT_FDCWD (-100L)
#define O_RDWR 2L
#define SEEK_SET 0L
#define SEEK_CUR 1L
#define SEEK_END 2L
#define SIGCHLD 17L
#define EACCES 13
#define FBIOGET_FSCREENINFO 0x4602L
#define FIX_SMEM_LEN 24u /* after id[16] and smem_start */
#define FIX_BYTES 80u
#define VIDEO_GID 39L
#define NOBODY 1000L

static long syscall4(long nr, long a, long b, long c, long d) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static long open_fb(void) {
    return syscall4(SYS_OPENAT, AT_FDCWD, (long)"/dev/fb0", O_RDWR, 0);
}

static long seek(long fd, long off, long whence) {
    return syscall4(SYS_LSEEK, fd, off, whence, 0);
}

/* Bits 1 to 16: the position, through write, read and lseek. */
static uint32_t positions(long fd) {
    static const uint8_t out[4] = {0x11, 0x22, 0x33, 0x44};
    static uint8_t fix[FIX_BYTES]; /* static: a zeroed local would need memset */
    uint8_t in[4] = {0};
    uint32_t passed = syscall4(SYS_WRITE, fd, (long)out, 4, 0) == 4 ? 1u : 0u, len = 0;
    if (seek(fd, 0, SEEK_SET) == 0 && syscall4(SYS_READ, fd, (long)in, 4, 0) == 4 &&
        in[0] == 0x11 && in[3] == 0x44) {
        passed |= 2u;
    }
    passed |= seek(fd, 0, SEEK_CUR) == 4 ? 4u : 0u;
    if (syscall4(SYS_IOCTL, fd, FBIOGET_FSCREENINFO, (long)fix, 0) == 0) {
        for (uint32_t i = 0; i < 4u; i++) {
            len |= (uint32_t)fix[FIX_SMEM_LEN + i] << (8u * i);
        }
        passed |= len != 0 && seek(fd, 0, SEEK_END) == (long)len ? 8u : 0u;
    }
    passed |= syscall4(SYS_READ, fd, (long)in, 4, 0) == 0 ? 16u : 0u;
    return passed;
}

/* Runs a child as uid 1000 with group `gid`; its exit status is 1 when the
 * open answered `want` (0 meaning "succeeded"), else 0. */
static uint32_t child_open_is(long gid, long want) {
    long pid = syscall4(SYS_CLONE, SIGCHLD, 0, 0, 0);
    int32_t status = 0;
    if (pid == 0) {
        long fd;
        syscall4(SYS_SETRESGID, gid, gid, gid, 0);
        syscall4(SYS_SETRESUID, NOBODY, NOBODY, NOBODY, 0);
        fd = open_fb();
        syscall4(SYS_EXIT_GROUP, want == 0 ? fd >= 0 : fd == want, 0, 0, 0);
    }
    if (pid < 0 || syscall4(SYS_WAIT4, pid, (long)&status, 0, 0) != pid) {
        return 0;
    }
    return ((status >> 8) & 0xff) == 1 ? 1u : 0u;
}

void user_main(void) {
    long fd = open_fb();
    uint32_t passed = fd >= 0 ? positions(fd) : 0u;
    syscall4(SYS_CLOSE, fd, 0, 0, 0);
    passed |= child_open_is(NOBODY, -EACCES) != 0 ? 32u : 0u;
    passed |= child_open_is(VIDEO_GID, 0) != 0 ? 64u : 0u;
    syscall4(SYS_EXIT_GROUP, (long)passed, 0, 0, 0);
}
