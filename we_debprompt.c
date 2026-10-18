/* Recognising a debugger's prompt in the bytes its output arrives as.
 *
 * The debugger writes its prompt without a newline, so the editor's read of the
 * pipe stops in the middle of it as often as not: "(gd" now, "b) " next.  The
 * answer therefore cannot come from one read's buffer, and the previous
 * implementation of this -- four near-identical branches inside e_d_line_read,
 * one per backend -- got it wrong twice over.  It placed the new bytes at a
 * fixed offset from a 12-byte scratch instead of next to the carried tail, so
 * the comparison never matched a split prompt at all; and it indexed that
 * scratch with the prompt's length, so the one backend whose prompt is 7 bytes
 * rather than 6 wrote two bytes past the end (F95, caught by AddressSanitizer as
 * a global-buffer-overflow WRITE in e_d_line_read).
 *
 * What replaces it is one sliding window of the last plen bytes seen, which is
 * the only thing prompt recognition ever needs.  Every write lands inside an
 * array whose size is checked against the prompt's length, so a longer prompt
 * is refused rather than allowed to run off the end.
 */
#include <string.h>
#include "we_debprompt.h"

/* Longest prompt this will match.  The window only ever holds the last plen
   bytes, so one array of this size serves every backend; "(a68g) " at 7 is the
   longest the editor knows. */
#define WPE_DEB_PROMPT_MAX 8

static char win[WPE_DEB_PROMPT_MAX];
static int win_len;            /* valid bytes in win, never above the prompt's */
static int win_plen;           /* the prompt length win currently belongs to */

void e_deb_prompt_reset(void)
{
	memset(win, 0, sizeof win);
	win_len = 0;
	win_plen = 0;
}

int e_deb_prompt_feed(const char *prompt, const char *bytes, int len)
{
	int plen, i;

	if (!prompt || !bytes || len < 0)
		return (0);
	plen = (int)strlen(prompt);
	if (plen <= 0 || plen > WPE_DEB_PROMPT_MAX)
		return (0);          /* not a prompt this window can hold */
	if (plen != win_plen) {
		/* A different backend: whatever the window holds belongs to another
		   prompt length and would only produce a false match. */
		memset(win, 0, sizeof win);
		win_len = 0;
		win_plen = plen;
	}

	for (i = 0; i < len; i++) {
		if (win_len < plen)
			win[win_len++] = bytes[i];
		else {
			memmove(win, win + 1, (size_t)(plen - 1));
			win[plen - 1] = bytes[i];
		}
	}

	/* Checked once, after the whole read, because the prompt has to be the last
	   thing the debugger sent: a line that merely mentions "(gdb) " in passing
	   is not the debugger waiting for a command. */
	if (win_len == plen && memcmp(win, prompt, (size_t)plen) == 0) {
		win_len = 0;         /* consumed, so it cannot match twice */
		return (1);
	}
	return (0);
}
