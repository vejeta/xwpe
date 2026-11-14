#ifndef WPE_TERMRELEASE_H
#define WPE_TERMRELEASE_H

/* The bytes that give the terminal back to the shell when the editor ends
   in a signal handler.  Kept in its own file, with no curses or editor
   headers, so a test can link it alone. */

#include <stddef.h>

/** Room the terminal owner keeps for the string: the longest one measured
    (xterm-256color) is 89 bytes. */
#define E_T_RELEASE_MAX 256

/**
 * e_t_release_string_build - Compose what a signal handler writes to give the
 * terminal back.
 * @buf: where the string goes.
 * @size: the room in @buf.
 * @console: 1 on a Linux console, which is sent no keyboard-protocol reset.
 * @sgr0: terminfo's attributes reset, or NULL.
 * @cnorm: terminfo's normal cursor, or NULL.
 * @rmkx: terminfo's keypad-transmit off, or NULL.
 * @rmcup: terminfo's alternate-screen exit, or NULL.
 *
 * The string is what the editor's own exit writes, as one run of bytes: the
 * keyboard protocols off, bracketed paste and the mouse modes off, the mouse
 * pointer given back, then the terminal's own strings for plain attributes,
 * the cursor, the keypad and leaving the alternate screen, which comes last.
 * It is built once, before the editor first changes a mode, because the
 * handler that writes it may run only async-signal-safe code.  A terminfo
 * string that is NULL or (char *)-1 (the capability is absent) is left out,
 * its padding marks ("$<5>") are taken out, and a piece that does not fit
 * whole is left out, never cut.
 * Return: the length of the string.
 */
size_t e_t_release_string_build(char *buf, size_t size, int console,
                                const char *sgr0, const char *cnorm,
                                const char *rmkx, const char *rmcup);

#endif
