/* The controlling terminal: the kernel's half of what `core/tty.c` decides.
 *
 * The line discipline is pure and lives in the hexagon. This file does the
 * three things only the kernel can:
 *
 *   - **pull bytes off the wire**, through a port so that the hexagon is
 *     not reaching at a UART;
 *   - **raise a signal on the foreground process group**, which is what
 *     makes `^C` interrupt the job rather than whatever happens to be
 *     running;
 *   - **block a reader** until a line is ready, and refuse one that is not
 *     in the foreground.
 *
 * ## What a controlling terminal actually has to know
 *
 * One thing: which process group is in the foreground. Everything else
 * follows from it. `^C` goes to that group; a read from any *other* group
 * gets `SIGTTIN` instead of the line, because two programs reading one
 * keyboard would each get an arbitrary half of what was typed. A shell sets
 * it when it starts a job and takes it back when the job stops.
 */
#ifndef MINIKERN_KERNEL_TTY_H
#define MINIKERN_KERNEL_TTY_H

#include <stdbool.h>
#include <stdint.h>

#include "core/ports.h"

/* The four requests a program actually makes of a terminal. Their numbers
 * are the ABI's, like every other constant a program hands the kernel. */
#define TIOCGWINSZ 0x5413u
#define TIOCSWINSZ 0x5414u
#define TIOCGPGRP 0x540fu
#define TIOCSPGRP 0x5410u

/* What `TIOCGWINSZ` fills in. The pixel fields are part of the shape and
 * nothing on a serial line can know them, so they are reported as zero
 * rather than invented. */
struct user_winsize {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;
    uint16_t ypixel;
};

/* Gives the terminal its input source and the session that owns it. The
 * port is how the hexagon rule is kept: this file never names a UART. */
void ktty_init(const struct console_input_port *input, uint32_t session_pid);

/* Pulls whatever has arrived off the wire and runs it through the
 * discipline, echoing and raising signals as it goes.
 *
 * Called from the housekeeping beat, which is what makes a terminal work
 * without an interrupt wired to it — the same mechanism the polled network
 * uses, and the same cost. An interrupt from the UART would remove it, and
 * is on the roadmap with the rest of the polling. */
void ktty_poll(void);

/* The foreground process group, and setting it (`TIOCGPGRP`/`TIOCSPGRP`).
 * Zero means nobody, which is the state before a shell claims it and the
 * state in which a read is simply allowed — refusing every read on a
 * machine with no shell would make the terminal unusable rather than
 * correct. */
uint32_t ktty_foreground(void);
void ktty_set_foreground(uint32_t pgid);

/* A read, for the console file object.
 *
 * Blocks until a line is ready. Returns 0 at end of file, a negative errno
 * otherwise — including `KERR_AGAIN` for a reader that is not in the
 * foreground group, after raising `SIGTTIN` on it. */
int32_t ktty_read(void *buf, uint32_t n);

/* Whether a read would return immediately, for `poll`. */
bool ktty_ready(void);

/* How many bytes of completed line a read would take now (`FIONREAD`). */
uint32_t ktty_pending(void);

/* Whether typed bytes are echoed, and switching it (`TCSETS` with or
 * without `ECHO`, which is how a password prompt hides what is typed). */
bool ktty_echo(void);
void ktty_set_echo(bool on);

/* The terminal's mode, as `TCGETS`/`TCSETS` carry it (Linux's flag words
 * and nineteen control characters). */
#define KTTY_NCC 19u
#define KTTY_VTIME 5u
#define KTTY_VMIN 6u

struct ktty_mode {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[KTTY_NCC];
};

void ktty_get_mode(struct ktty_mode *out);

/* Adopts a mode. **Every bit is honoured or the call is refused**
 * (`KERR_INVAL`), never accepted and ignored: ICANON, ECHO, ISIG, ICRNL,
 * OPOST/ONLCR (off only where the console can write untranslated), VMIN and
 * VTIME. The line settings and the other control characters are fixed. */
int ktty_set_mode(const struct ktty_mode *mode);

/* Writes a program's output, with or without the CR-before-LF `ONLCR` asks
 * for. */
void ktty_write(const void *buf, uint32_t n);

/* The window size, for `TIOCGWINSZ`. There is no way to ask a serial line
 * how big the window is, so these are the values a program is told; 80x24
 * is the convention a program expects when nothing knows better, and
 * `TIOCSWINSZ` lets whatever does know set it. */
void ktty_window(uint16_t *rows, uint16_t *cols);
void ktty_set_window(uint16_t rows, uint16_t cols);

#endif
