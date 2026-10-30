/* The line discipline, on the host.
 *
 * Every case here is a *plausible* wrong behaviour rather than a crash, and
 * none of them can be tested with a board in the loop — they need a
 * human's fingers, or this.
 */
#include "core/tty.h"

#include <stdint.h>
#include <string.h>

#include "ktest.h"

/* Feeds a string in and returns the last action, so a test reads as the
 * keys somebody typed. */
static enum tty_action type(struct tty *t, const char *keys) {
    uint8_t echo[4];
    uint32_t echo_len = 0;
    enum tty_action last = TTY_NOTHING;
    for (const char *k = keys; *k != '\0'; k++) {
        last = tty_input(t, (uint8_t)*k, echo, &echo_len);
    }
    return last;
}

static void test_a_line_arrives_whole_and_keeps_its_newline(void) {
    struct tty t;
    char got[32];
    tty_init(&t);
    KTEST_CHECK(!tty_has_line(&t));
    KTEST_CHECK(type(&t, "ls") == TTY_NOTHING);
    /* Nothing is readable until Enter: that is what canonical mode *is*. */
    KTEST_CHECK(!tty_has_line(&t));
    KTEST_CHECK(type(&t, "\r") == TTY_LINE_READY);
    KTEST_CHECK(tty_has_line(&t));
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 3u);
    /* The newline is part of the line. A program that reads a terminal
     * expects it, and a line without one reads as still being typed. */
    KTEST_CHECK(memcmp(got, "ls\n", 3u) == 0);
    KTEST_CHECK(!tty_has_line(&t));
}

static void test_a_terminal_sends_cr_and_a_program_gets_lf(void) {
    struct tty t;
    char got[8];
    tty_init(&t);
    /* Enter is carriage return on the wire and must arrive as a line feed,
     * or every line a program reads ends in a character it does not expect. */
    KTEST_CHECK(type(&t, "x\r") == TTY_LINE_READY);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 2u);
    KTEST_CHECK(got[1] == '\n');
}

static void test_erase_removes_one_and_echoes_three_bytes(void) {
    struct tty t;
    uint8_t echo[4];
    uint32_t echo_len = 0;
    char got[8];
    tty_init(&t);
    (void)type(&t, "ab");
    KTEST_CHECK(tty_input(&t, TTY_ERASE, echo, &echo_len) == TTY_NOTHING);
    /* Back up, overwrite with a space, back up again. A lone backspace
     * moves the cursor and leaves the glyph on screen, which looks like the
     * key being ignored. */
    KTEST_CHECK(echo_len == 3u);
    KTEST_CHECK(echo[0] == TTY_ERASE_BS && echo[1] == ' ' && echo[2] == TTY_ERASE_BS);
    (void)type(&t, "\r");
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 2u);
    KTEST_CHECK(memcmp(got, "a\n", 2u) == 0);
    /* `^H` is what many terminals send instead, and must do the same. */
    tty_init(&t);
    (void)type(&t, "ab");
    KTEST_CHECK(tty_input(&t, TTY_ERASE_BS, echo, &echo_len) == TTY_NOTHING);
    (void)type(&t, "\r");
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 2u);
    KTEST_CHECK(memcmp(got, "a\n", 2u) == 0);
}

static void test_erase_on_an_empty_line_echoes_nothing(void) {
    struct tty t;
    uint8_t echo[4];
    uint32_t echo_len = 7u;
    tty_init(&t);
    KTEST_CHECK(tty_input(&t, TTY_ERASE, echo, &echo_len) == TTY_NOTHING);
    /* **Nothing.** Erasing here walks the cursor back over the prompt,
     * which is not this line's to erase. */
    KTEST_CHECK(echo_len == 0u);
    KTEST_CHECK(!tty_has_line(&t));
}

static void test_kill_clears_the_line(void) {
    struct tty t;
    char got[8];
    tty_init(&t);
    (void)type(&t, "oops");
    {
        uint8_t echo[4];
        uint32_t echo_len = 0;
        KTEST_CHECK(tty_input(&t, TTY_KILL, echo, &echo_len) == TTY_NOTHING);
    }
    (void)type(&t, "ok\r");
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 3u);
    KTEST_CHECK(memcmp(got, "ok\n", 3u) == 0);
}

static void test_interrupt_and_suspend_are_actions_not_data(void) {
    struct tty t;
    uint8_t echo[4];
    uint32_t echo_len = 0;
    tty_init(&t);
    (void)type(&t, "half");
    KTEST_CHECK(tty_input(&t, TTY_INTR, echo, &echo_len) == TTY_SIGNAL_INTR);
    /* Echoed as `^C`, never as a raw 0x03 — which a terminal would act on
     * rather than display. */
    KTEST_CHECK(echo_len == 2u && echo[0] == '^' && echo[1] == 'C');
    /* And the half-typed line goes with the interrupt: it is not delivered
     * to whatever runs next. */
    KTEST_CHECK(!tty_has_line(&t));
    KTEST_CHECK(tty_input(&t, TTY_SUSP, echo, &echo_len) == TTY_SIGNAL_SUSP);
    KTEST_CHECK(echo_len == 2u && echo[1] == 'Z');
}

static void test_end_of_file_only_on_an_empty_line(void) {
    struct tty t;
    char got[8];
    uint8_t echo[4];
    uint32_t echo_len = 0;
    tty_init(&t);
    /* Empty: end of file. */
    KTEST_CHECK(tty_input(&t, TTY_EOF, echo, &echo_len) == TTY_END_OF_FILE);
    /* Not empty: the line is delivered **without** a newline, and must not
     * be thrown away — this is the version of `^D` that loses a user's
     * typing. */
    (void)type(&t, "data");
    KTEST_CHECK(tty_input(&t, TTY_EOF, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 4u);
    KTEST_CHECK(memcmp(got, "data", 4u) == 0);
}

static void test_a_small_reader_gets_the_line_in_pieces(void) {
    struct tty t;
    char got[4];
    tty_init(&t);
    (void)type(&t, "abcdef\r");
    /* Seven bytes with the newline, into a four-byte buffer. */
    KTEST_CHECK(tty_take_line(&t, got, 4u) == 4u);
    KTEST_CHECK(memcmp(got, "abcd", 4u) == 0);
    KTEST_CHECK(tty_has_line(&t));
    KTEST_CHECK(tty_take_line(&t, got, 4u) == 3u);
    KTEST_CHECK(memcmp(got, "ef\n", 3u) == 0);
    KTEST_CHECK(!tty_has_line(&t));
}

static void test_an_over_long_line_does_not_overrun(void) {
    struct tty t;
    uint8_t echo[4];
    uint32_t echo_len = 0;
    tty_init(&t);
    /* Twice the buffer, one byte at a time. The guard is that this returns
     * at all and the length never exceeds the array. */
    for (uint32_t i = 0; i < TTY_LINE_MAX * 2u; i++) {
        (void)tty_input(&t, 'x', echo, &echo_len);
        KTEST_CHECK(t.len < TTY_LINE_MAX);
    }
    KTEST_CHECK(tty_input(&t, TTY_LF, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(t.ready <= TTY_LINE_MAX);
}

static void test_nulls_are_refused_not_dereferenced(void) {
    uint8_t echo[4];
    uint32_t echo_len = 0;
    struct tty t;
    tty_init(&t);
    tty_init(NULL);
    KTEST_CHECK(tty_input(NULL, 'x', echo, &echo_len) == TTY_NOTHING);
    KTEST_CHECK(tty_input(&t, 'x', NULL, &echo_len) == TTY_NOTHING);
    KTEST_CHECK(tty_input(&t, 'x', echo, NULL) == TTY_NOTHING);
    KTEST_CHECK(tty_take_line(NULL, echo, sizeof echo) == 0u);
    KTEST_CHECK(tty_take_line(&t, NULL, 4u) == 0u);
    KTEST_CHECK(!tty_has_line(NULL));
}

/* Typing ahead of a reader that has not read the last line must ADD to the
 * buffer: the next line used to be written over the start of the one still
 * waiting. */
static void test_typing_ahead_does_not_overwrite_a_waiting_line(void) {
    struct tty t;
    char got[32];
    tty_init(&t);
    KTEST_CHECK(type(&t, "first\r") == TTY_LINE_READY);
    KTEST_CHECK(type(&t, "sec") == TTY_NOTHING);
    KTEST_CHECK(tty_take_line(&t, got, 6) == 6u);
    KTEST_CHECK(memcmp(got, "first\n", 6u) == 0);
    KTEST_CHECK(!tty_has_line(&t)); /* "sec" is still being edited */
    KTEST_CHECK(type(&t, "ond\r") == TTY_LINE_READY);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 7u);
    KTEST_CHECK(memcmp(got, "second\n", 7u) == 0);
}

/* Raw mode: each byte is ready at once, editing keys are data, and a line
 * half-typed when raw mode began is handed over, not lost. */
static void test_raw_mode_delivers_every_byte_at_once(void) {
    struct tty t;
    char got[32];
    uint8_t echo[4];
    uint32_t echo_len = 0;
    tty_init(&t);
    (void)type(&t, "ab");
    tty_set_mode(&t, false, true, true);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 2u && memcmp(got, "ab", 2u) == 0);
    KTEST_CHECK(tty_input(&t, 'x', echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_input(&t, TTY_ERASE, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_input(&t, TTY_EOF, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 3u);
    KTEST_CHECK(got[0] == 'x' && (uint8_t)got[1] == TTY_ERASE && (uint8_t)got[2] == TTY_EOF);
}

/* ISIG off makes ^C a byte; ICRNL off keeps a CR a CR. */
static void test_signals_and_cr_translation_are_switches(void) {
    struct tty t;
    char got[8];
    uint8_t echo[4];
    uint32_t echo_len = 0;
    tty_init(&t);
    tty_set_mode(&t, false, false, false);
    KTEST_CHECK(tty_input(&t, TTY_INTR, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_input(&t, TTY_CR, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 2u);
    KTEST_CHECK((uint8_t)got[0] == TTY_INTR && (uint8_t)got[1] == TTY_CR);
    tty_set_mode(&t, false, true, true);
    KTEST_CHECK(tty_input(&t, TTY_INTR, echo, &echo_len) == TTY_SIGNAL_INTR);
    KTEST_CHECK(tty_input(&t, TTY_CR, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(tty_take_line(&t, got, sizeof got) == 1u && got[0] == '\n');
}

/* A raw buffer fills and then refuses, like a canonical line. */
static void test_a_full_raw_buffer_drops_rather_than_overruns(void) {
    struct tty t;
    uint8_t echo[4];
    uint32_t echo_len = 0;
    tty_init(&t);
    tty_set_mode(&t, false, false, false);
    for (uint32_t i = 0; i < TTY_LINE_MAX * 2u; i++) {
        (void)tty_input(&t, 'z', echo, &echo_len);
    }
    KTEST_CHECK(t.ready == TTY_LINE_MAX - 1u);
}

/* Enter echoes one LF: the output side adds the CR, and an echo that added
 * one too showed every line ending as CR CR LF. With echo off, nothing. */
static void test_enter_echoes_a_single_newline(void) {
    struct tty t;
    uint8_t echo[4];
    uint32_t echo_len = 9;
    tty_init(&t);
    KTEST_CHECK(tty_input(&t, TTY_CR, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(echo_len == 1u && echo[0] == TTY_LF);
    t.echo = false;
    KTEST_CHECK(tty_input(&t, TTY_CR, echo, &echo_len) == TTY_LINE_READY);
    KTEST_CHECK(echo_len == 0u);
}

int main(void) {
    KTEST_RUN(test_enter_echoes_a_single_newline);
    KTEST_RUN(test_typing_ahead_does_not_overwrite_a_waiting_line);
    KTEST_RUN(test_raw_mode_delivers_every_byte_at_once);
    KTEST_RUN(test_signals_and_cr_translation_are_switches);
    KTEST_RUN(test_a_full_raw_buffer_drops_rather_than_overruns);
    KTEST_RUN(test_a_line_arrives_whole_and_keeps_its_newline);
    KTEST_RUN(test_a_terminal_sends_cr_and_a_program_gets_lf);
    KTEST_RUN(test_erase_removes_one_and_echoes_three_bytes);
    KTEST_RUN(test_erase_on_an_empty_line_echoes_nothing);
    KTEST_RUN(test_kill_clears_the_line);
    KTEST_RUN(test_interrupt_and_suspend_are_actions_not_data);
    KTEST_RUN(test_end_of_file_only_on_an_empty_line);
    KTEST_RUN(test_a_small_reader_gets_the_line_in_pieces);
    KTEST_RUN(test_an_over_long_line_does_not_overrun);
    KTEST_RUN(test_nulls_are_refused_not_dereferenced);
    return ktest_failures();
}
