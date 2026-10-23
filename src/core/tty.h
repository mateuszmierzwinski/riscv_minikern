/* tty — the line discipline, as a pure state machine.
 *
 * What turns a stream of bytes from a wire into the thing a program expects
 * when it reads a terminal: a *line* at a time, with the character the user
 * typed last removable, and three control characters that are not data at
 * all but requests to the kernel.
 *
 * ## Why this is pure
 *
 * No console, no process table, no signals — it takes a byte and says what
 * happened. That is deliberate: the editing rules are where terminals are
 * subtly wrong, and every one of those mistakes is a *plausible* behaviour
 * that only shows up under a human's fingers. Backspace at the start of an
 * empty line erasing the prompt, an echo that prints a control character as
 * itself, a line delivered without its newline, `^D` on a non-empty line
 * being treated as end of file and throwing the line away — none of those
 * fault, and none of them can be tested at all with a board in the loop.
 *
 * So the rules live here and are tested on the host, and the kernel side
 * does the three things only it can: read the wire, raise the signal, and
 * block the reader.
 *
 * ## Canonical mode only
 *
 * `ICANON` with `ECHO`, which is what a shell and a line-reading program
 * want. Raw mode is a flag away but nothing needs it yet, and a mode nobody
 * uses is a mode nobody tests.
 */
#ifndef MINIKERN_CORE_TTY_H
#define MINIKERN_CORE_TTY_H

#include <stdbool.h>
#include <stdint.h>

/* One line is as much as a reader can be given at once. 256 is generous for
 * a command line and small enough to sit in a kernel structure; a longer
 * line is delivered in pieces rather than refused, which is what a program
 * reading a terminal already copes with. */
#define TTY_LINE_MAX 256u

/* The control characters, named rather than spelled as numbers at the call
 * sites — `\003` in a comparison is how a terminal ends up doing the wrong
 * thing on the wrong key. */
#define TTY_INTR 0x03u  /* ^C: interrupt the foreground group */
#define TTY_EOF 0x04u   /* ^D: end of file, if the line is empty */
#define TTY_ERASE 0x7fu /* DEL: erase one character */
#define TTY_ERASE_BS 0x08u /* ^H, which many terminals send instead */
#define TTY_KILL 0x15u  /* ^U: erase the whole line */
#define TTY_SUSP 0x1au  /* ^Z: suspend the foreground group */
#define TTY_CR 0x0du    /* carriage return, which a terminal sends for Enter */
#define TTY_LF 0x0au    /* line feed, which a program expects to receive */

/* What the kernel must do about a byte. Exactly one of these per byte, so a
 * caller is a `switch` with no interactions to reason about. */
enum tty_action {
    TTY_NOTHING = 0,   /* absorbed: it changed the line being edited */
    TTY_LINE_READY,    /* a complete line is waiting in the buffer */
    TTY_SIGNAL_INTR,   /* raise the interrupt signal on the foreground group */
    TTY_SIGNAL_SUSP,   /* suspend the foreground group */
    TTY_END_OF_FILE    /* the reader should see end of file */
};

/* The editing state. Owned by whoever has the terminal; this file never
 * allocates. */
struct tty {
    uint8_t line[TTY_LINE_MAX];
    uint32_t len;      /* how much of `line` is being edited */
    uint32_t ready;    /* how much of `line` is a completed line, 0 if none */
    bool echo;         /* print what is typed back at the terminal */
};

/* Starts a terminal in canonical mode with echo on. */
void tty_init(struct tty *t);

/* Feeds one byte in.
 *
 * `echo_out` receives up to four bytes the caller should write back to the
 * terminal, and `echo_len` how many — the caller does the writing, because
 * echoing is I/O and this file does none. A backspace echoes three bytes
 * (back up, space, back up again), which is what actually erases a glyph on
 * a terminal; printing `\b` alone moves the cursor and leaves the character
 * on the screen.
 *
 * The returned action is what the kernel must do. `TTY_LINE_READY` means a
 * line is in the buffer and `tty_take_line` will hand it over. */
enum tty_action tty_input(struct tty *t, uint8_t byte, uint8_t *echo_out, uint32_t *echo_len);

/* Moves a completed line out, up to `cap` bytes, and returns how many.
 *
 * A line longer than `cap` is given in pieces across calls rather than
 * truncated: a reader with a small buffer is normal, and losing the rest of
 * somebody's command line because of it is not. Returns 0 when no line is
 * ready. */
uint32_t tty_take_line(struct tty *t, void *to, uint32_t cap);

/* Whether a read would return immediately. */
bool tty_has_line(const struct tty *t);

#endif
