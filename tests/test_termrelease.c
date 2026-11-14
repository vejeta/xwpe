/* test_termrelease.c -- unit test for the string a signal handler writes to
 * give the terminal back.  Links we_termrelease.c only; no editor, no
 * curses -- always runs (no skip).
 *
 * The strings below are what terminfo gives for the three terminals the
 * editor is measured on (infocmp -1): a handler that is cut short on one of
 * them leaves the shell in the alternate screen or with the keyboard in the
 * editor's mode.
 */
#include <stdio.h>
#include <string.h>

#include "../we_termrelease.h"

#define KEYS_OFF  "\033[<u\033[>4m"
#define MODES_OFF "\033[?2004l\033[?1002l\033[?1006l\033[?1000l"
#define POINTER   "\033]22;\a"

#define XTERM_SGR0  "\033(B\033[m"
#define XTERM_CNORM "\033[?12l\033[?25h"
#define XTERM_RMKX  "\033[?1l\033>"
#define XTERM_RMCUP "\033[?1049l\033[23;0;0t"

#define LINUX_SGR0  "\033[m\017"
#define LINUX_CNORM "\033[?25h\033[?0c"

#define ABSENT ((const char *)-1)

#define XTERM_FULL_LEN 89

static int fails;

static void chk_str(const char *what, const char *got, size_t got_len,
                    const char *want)
{
	if (got_len != strlen(want) || memcmp(got, want, got_len) != 0) {
		printf("FAIL: %s: got %zu bytes, want %zu\n", what, got_len,
		       strlen(want));
		fails++;
	}
}

static void chk_int(const char *what, size_t got, size_t want)
{
	if (got != want) {
		printf("FAIL: %s: got %zu want %zu\n", what, got, want);
		fails++;
	}
}

/* @c SC-term-05 xterm-256color: eighty-nine bytes, the alternate screen last. */
static void xterm_gives_all_of_it(void)
{
	char buf[E_T_RELEASE_MAX];
	size_t n = e_t_release_string_build(buf, sizeof buf, 0, XTERM_SGR0,
	                                    XTERM_CNORM, XTERM_RMKX, XTERM_RMCUP);

	chk_int("xterm length", n, XTERM_FULL_LEN);
	chk_str("xterm bytes", buf, n,
	        KEYS_OFF MODES_OFF POINTER XTERM_SGR0 XTERM_CNORM XTERM_RMKX
	        XTERM_RMCUP);
}

/* @c SC-term-05 a Linux console is sent no keyboard-protocol reset. */
static void console_gets_no_keyboard_reset(void)
{
	char buf[E_T_RELEASE_MAX];
	size_t n = e_t_release_string_build(buf, sizeof buf, 1, XTERM_SGR0,
	                                    XTERM_CNORM, XTERM_RMKX, XTERM_RMCUP);

	chk_str("console bytes", buf, n,
	        MODES_OFF POINTER XTERM_SGR0 XTERM_CNORM XTERM_RMKX XTERM_RMCUP);
	chk_int("console has no CSI < u", (size_t)(strstr(buf, "\033[<u") != NULL), 0);
	chk_int("console has no CSI > 4 m", (size_t)(strstr(buf, "\033[>4m") != NULL), 0);
}

/* @c SC-term-05 linux has no rmkx and no rmcup: the string ends in cnorm. */
static void linux_ends_in_the_cursor(void)
{
	char buf[E_T_RELEASE_MAX];
	size_t n = e_t_release_string_build(buf, sizeof buf, 1, LINUX_SGR0,
	                                    LINUX_CNORM, NULL, NULL);

	chk_str("linux bytes", buf, n, MODES_OFF POINTER LINUX_SGR0 LINUX_CNORM);
}

/* @c SC-term-05 NULL and (char *)-1 are a capability the terminal lacks. */
static void absent_capabilities_are_left_out(void)
{
	char buf[E_T_RELEASE_MAX];
	size_t n = e_t_release_string_build(buf, sizeof buf, 0, NULL, ABSENT,
	                                    NULL, ABSENT);

	chk_str("absent bytes", buf, n, KEYS_OFF MODES_OFF POINTER);
}

/* @c SC-term-05 a buffer of twenty bytes keeps only the whole pieces. */
static void a_small_buffer_keeps_whole_pieces(void)
{
	char buf[20];
	size_t n = e_t_release_string_build(buf, sizeof buf, 0, XTERM_SGR0,
	                                    XTERM_CNORM, XTERM_RMKX, XTERM_RMCUP);
	size_t i;

	if (n >= sizeof buf) {
		printf("FAIL: small buffer: %zu bytes in a 20 byte buffer\n", n);
		fails++;
		return;
	}
	/* The keyboard reset (9 bytes) and the pointer (6) fit; the next
	   piece, sgr0 (6), would need 22 with the closing NUL.  Nothing is
	   cut. */
	chk_str("small buffer bytes", buf, n, KEYS_OFF POINTER);
	for (i = 0; i < n; i++)
		if (buf[i] == '\0') {
			printf("FAIL: small buffer: NUL inside the string\n");
			fails++;
		}
}

/* @c SC-term-05 a terminfo padding mark is no byte to write. */
static void padding_marks_are_taken_out(void)
{
	char buf[E_T_RELEASE_MAX];
	size_t n = e_t_release_string_build(buf, sizeof buf, 1, "\033[m$<2>",
	                                    NULL, NULL, "\033[?1049l$<10/>");

	chk_str("padding bytes", buf, n, MODES_OFF POINTER "\033[m" "\033[?1049l");
}

/* @c SC-term-05 no buffer, no room: nothing is written, nothing breaks. */
static void no_room_gives_nothing(void)
{
	char buf[1];

	chk_int("null buffer", e_t_release_string_build(NULL, 10, 0, NULL, NULL,
	                                                NULL, NULL), 0);
	chk_int("zero size", e_t_release_string_build(buf, 0, 0, NULL, NULL,
	                                              NULL, NULL), 0);
	chk_int("one byte", e_t_release_string_build(buf, 1, 0, NULL, NULL,
	                                             NULL, NULL), 0);
}

int main(void)
{
	xterm_gives_all_of_it();
	console_gets_no_keyboard_reset();
	linux_ends_in_the_cursor();
	absent_capabilities_are_left_out();
	a_small_buffer_keeps_whole_pieces();
	padding_marks_are_taken_out();
	no_room_gives_nothing();
	if (fails == 0)
		printf("test_termrelease: ok\n");
	return fails ? 1 : 0;
}
