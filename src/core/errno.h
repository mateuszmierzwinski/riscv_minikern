/* errno.h — the kernel's error codes. The values are Linux's (asm-generic
 * errno-base.h), because the same numbers are returned to user space as
 * -errno by the syscall layer: a Go binary reads them unchanged. */
#ifndef MINIKERN_CORE_ERRNO_H
#define MINIKERN_CORE_ERRNO_H

#define KERR_OK      0
#define KERR_PERM    (-1)   /* operation not permitted */
#define KERR_NOENT   (-2)   /* no such entry */
#define KERR_SRCH    (-3)   /* no such process or thread */
#define KERR_INTR    (-4)   /* interrupted by a signal */
#define KERR_IO      (-5)
#define KERR_NOEXEC  (-8)   /* not an executable format we run */
#define KERR_BADF    (-9)   /* not an open file descriptor */
#define KERR_PIPE    (-32)  /* writing to a pipe with no reader */
#define KERR_CHILD   (-10)  /* no child processes */
#define KERR_AGAIN   (-11)  /* try again (also EWOULDBLOCK) */
#define KERR_NOMEM   (-12)  /* out of memory */
#define KERR_ACCES   (-13)  /* permission denied */
#define KERR_FAULT   (-14)  /* bad address */
#define KERR_EXIST   (-17)
#define KERR_NODEV   (-19)  /* no such device */
#define KERR_NOTDIR  (-20)
#define KERR_ISDIR   (-21)
#define KERR_INVAL   (-22)  /* invalid argument */
#define KERR_MFILE   (-24)  /* too many open files */
#define KERR_NOTTY   (-25)  /* not a terminal */
#define KERR_FBIG    (-27)  /* file too large */
#define KERR_BUSY    (-16)  /* in use, so the request cannot be served */
#define KERR_NOSPC   (-28)  /* no space left on the device */
#define KERR_SPIPE   (-29)  /* not seekable */
#define KERR_ROFS    (-30)  /* read-only filesystem or device */
/* **A quota refused it, which is not the same as the disk being full.** A
 * caller that saw `ENOSPC` would conclude the filesystem has no room and
 * perhaps delete something unrelated; `EDQUOT` says this owner has used
 * their allowance and the filesystem is otherwise fine. */
#define KERR_DQUOT   (-122) /* EDQUOT: over an owner's quota */
#define KERR_RANGE   (-34)
#define KERR_NOLCK   (-37)  /* no record locks left */
#define KERR_2BIG        (-7)   /* the argument list is too long */
#define KERR_NAMETOOLONG (-36)
#define KERR_LOOP        (-40)  /* too many symbolic links */
#define KERR_NOSYS   (-38)  /* not implemented */
#define KERR_NOTEMPTY (-39)
#define KERR_PROTO   (-71)  /* a peer broke its own protocol */
#define KERR_XDEV    (-18)  /* EXDEV: a link across two filesystems */
#define KERR_NOTSUP  (-95)  /* EOPNOTSUPP: understood, and refused */
#define KERR_TIMEDOUT (-110)
/* A filesystem's log holds work that was never committed to its metadata.
 * Not an error on its own — it says the filesystem is readable and must not
 * be written — so it is distinct from a refusal. */
#define KERR_DIRTY   (-117) /* EUCLEAN: the structure is not clean */

#endif
