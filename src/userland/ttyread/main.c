/* A line typed at the console, read by a program blocked in `read`.
 *
 * With the network tickless the machine may take no timer interrupt for
 * seconds, so nothing polls the UART: the typed bytes have to wake the
 * reader by the UART's own receive interrupt. A reader that is never woken
 * is what this catches — the case times out instead of printing the line.
 *
 * Prints "ttyread: ready", reads one line from fd 0, echoes it, and exits
 * with its length (the harness types "minikern\r": 9, the LF included).
 */
#include <stdint.h>

#define SYS_READ 63
#define SYS_WRITE 64
#define SYS_EXIT_GROUP 94

static long syscall3(long nr, long a, long b, long c) {
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a7 __asm__("a7") = nr;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static void say(const char *s, long n) {
    (void)syscall3(SYS_WRITE, 1, (long)s, n);
}

void user_main(void) {
    static char line[64];
    long n;
    say("ttyread: ready\n", 15);
    n = syscall3(SYS_READ, 0, (long)line, sizeof line);
    if (n > 0) {
        say("ttyread: got ", 13);
        say(line, n);
    }
    (void)syscall3(SYS_EXIT_GROUP, n, 0, 0);
}
