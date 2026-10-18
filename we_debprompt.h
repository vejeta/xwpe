#ifndef WPE_DEBPROMPT_H
#define WPE_DEBPROMPT_H

/* Recognition of a debugger's prompt in the bytes its output arrives as.
   Kept in its own file, with no editor headers, so it can be unit tested
   directly: we_debug.c is far too entangled with the editor to link on its own. */

/**
 * e_deb_prompt_feed - Slide new debugger output into the window and report
 * whether the prompt has just been completed.
 * @prompt: the prompt to look for, such as "(gdb) ".  NULL or "" matches
 *          nothing, so a backend without a prompt string is simply inert.
 * @bytes:  the bytes this read produced.
 * @len:    how many of them; may be 0.
 *
 * A prompt can arrive split across reads -- the debugger writes "(gd" and the
 * editor's non-blocking read returns before "b) " -- so the answer cannot come
 * from this buffer alone.  The window keeps the last strlen(@prompt) bytes seen
 * across calls, which is what makes a split prompt recognisable at all and what
 * keeps every byte written inside one array of a known size.
 *
 * Return: 1 when the window ends with @prompt (the prompt is then consumed, so
 * the same bytes cannot match twice), else 0.
 */
int e_deb_prompt_feed(const char *prompt, const char *bytes, int len);

/**
 * e_deb_prompt_reset - Forget everything the window holds.
 *
 * For a new debugging session, so output from a previous one cannot complete a
 * prompt in this one.  Switching backend does not need it: a different prompt
 * length clears the window on its own.
 */
void e_deb_prompt_reset(void);

#endif /* WPE_DEBPROMPT_H */
