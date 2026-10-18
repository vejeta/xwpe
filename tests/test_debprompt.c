/* test_debprompt.c -- unit test for the debugger prompt window.
 * Links we_debprompt.c only; no editor, no curses -- always runs (no skip).
 *
 * This is the test that F95 should have had.  The prompt arithmetic it replaces
 * lived inline in e_d_line_read, in four near-identical per-backend branches,
 * and it was wrong in two ways at once: a prompt split across two reads was
 * never recognised by any backend, and the one backend whose prompt is 7 bytes
 * rather than 6 wrote two bytes past a 12-byte scratch, which AddressSanitizer
 * reported as a global-buffer-overflow WRITE.  Both are asserted here, and the
 * overflow cases are the reason this test is worth building under -fsanitize.
 */
#include <stdio.h>
#include <string.h>

#include "../we_debprompt.h"

static int fails;

static void chk(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

/* Feed one read's worth of bytes, the way e_d_line_read does: len is the number
   of bytes read, and the caller has already stored '\0' at bytes[len]. */
static int feed(const char *prompt, const char *text)
{
	char buf[64];
	int len = (int)strlen(text);

	memset(buf, 0, sizeof buf);
	memcpy(buf, text, (size_t)len);
	buf[len] = '\0';
	return e_deb_prompt_feed(prompt, buf, len);
}

static void test_whole_prompt_in_one_read(void)
{
	e_deb_prompt_reset();
	chk("prompt arriving whole", feed("(gdb) ", "x (gdb) "), 1);
	e_deb_prompt_reset();
	chk("prompt alone", feed("(gdb) ", "(gdb) "), 1);
	e_deb_prompt_reset();
	chk("prompt at the end of a long line",
	    feed("(gdb) ", "Reading symbols from /usr/bin/a.out... done (gdb) "), 1);
}

static void test_a_prompt_in_passing_is_not_a_prompt(void)
{
	/* Only the last thing the debugger sent counts: a line that mentions the
	   prompt mid-way is output, not the debugger waiting for a command. */
	e_deb_prompt_reset();
	chk("prompt mentioned mid-line", feed("(gdb) ", "type (gdb) help now"), 0);
	e_deb_prompt_reset();
	chk("partial prompt at the end", feed("(gdb) ", "(gdb"), 0);
	e_deb_prompt_reset();
	chk("unrelated output", feed("(gdb) ", "Breakpoint 1 at 0x401136"), 0);
}

static void test_prompt_split_across_reads(void)
{
	/* The case the old code claimed to handle and never did. */
	e_deb_prompt_reset();
	chk("split 3+3, first half", feed("(gdb) ", "(gd"), 0);
	chk("split 3+3, second half", feed("(gdb) ", "b) "), 1);

	e_deb_prompt_reset();
	chk("split 1+3+2, first", feed("(gdb) ", "("), 0);
	chk("split 1+3+2, second", feed("(gdb) ", "gdb"), 0);
	chk("split 1+3+2, third", feed("(gdb) ", ") "), 1);

	e_deb_prompt_reset();
	chk("output then a split prompt", feed("(gdb) ", "done\n(g"), 0);
	chk("output then a split prompt, completed", feed("(gdb) ", "db) "), 1);
}

static void test_a68g_is_the_case_that_overflowed(void)
{
	/* "(a68g) " is 7 bytes.  The old code wrote str[7+j] for j up to i, with
	   i < 7, into a 12-byte scratch -- str[13] at worst.  Every split length
	   is driven here so the whole range is under the sanitizer, not just the
	   one that happened to abort in the field. */
	static const char prompt[] = "(a68g) ";
	int k;

	for (k = 0; k <= 6; k++) {
		char head[16], tail[16];
		int n;

		e_deb_prompt_reset();
		memcpy(head, prompt, (size_t)k);
		head[k] = '\0';
		strcpy(tail, prompt + k);
		if (k > 0)
			chk("a68g partial", feed(prompt, head), 0);
		n = feed(prompt, tail);
		if (n != 1) {
			printf("FAIL: a68g split at %d did not complete the prompt\n", k);
			fails++;
		}
	}
	e_deb_prompt_reset();
	chk("a68g whole", feed(prompt, "(a68g) "), 1);
}

static void test_every_backend_prompt(void)
{
	static const char *prompts[] = { "(gdb) ", "(dbx) ", "(Pdb) ", "(a68g) " };
	size_t i;

	for (i = 0; i < sizeof prompts / sizeof prompts[0]; i++) {
		e_deb_prompt_reset();
		chk(prompts[i], feed(prompts[i], prompts[i]), 1);
	}
}

static void test_a_matched_prompt_is_consumed(void)
{
	e_deb_prompt_reset();
	chk("first match", feed("(gdb) ", "(gdb) "), 1);
	chk("the same bytes do not match again", feed("(gdb) ", "break foo"), 0);
	e_deb_prompt_reset();
	feed("(gdb) ", "(gdb) ");
	chk("an empty read after a match", e_deb_prompt_feed("(gdb) ", "", 0), 0);
}

static void test_switching_backend_clears_the_window(void)
{
	/* A window filled for a 7-byte prompt must not be read as a 6-byte one,
	   or the tail of an a68g session could complete a gdb prompt. */
	e_deb_prompt_reset();
	chk("a68g partial", feed("(a68g) ", "(a68g"), 0);
	chk("gdb tail after an a68g partial", feed("(gdb) ", "b) "), 0);
}

static void test_reset_forgets_the_carry(void)
{
	e_deb_prompt_reset();
	chk("first half", feed("(gdb) ", "(gd"), 0);
	e_deb_prompt_reset();
	chk("second half after a reset", feed("(gdb) ", "b) "), 0);
}

static void test_inputs_that_must_be_refused_not_trusted(void)
{
	char buf[8];

	e_deb_prompt_reset();
	chk("NULL prompt", e_deb_prompt_feed(NULL, "x", 1), 0);
	chk("empty prompt", e_deb_prompt_feed("", "x", 1), 0);
	chk("NULL bytes", e_deb_prompt_feed("(gdb) ", NULL, 1), 0);
	chk("zero length", e_deb_prompt_feed("(gdb) ", "", 0), 0);
	chk("negative length", e_deb_prompt_feed("(gdb) ", "x", -1), 0);
	/* A prompt longer than the window is refused.  Refusing is the point: the
	   defect being replaced wrote past the end instead. */
	chk("over-long prompt", e_deb_prompt_feed("(prompt!) ", "(prompt!) ", 10), 0);
	memset(buf, 'x', sizeof buf);
	chk("over-long prompt after bytes", e_deb_prompt_feed("(prompt!) ", buf, 8), 0);
}

int main(void)
{
	test_whole_prompt_in_one_read();
	test_a_prompt_in_passing_is_not_a_prompt();
	test_prompt_split_across_reads();
	test_a68g_is_the_case_that_overflowed();
	test_every_backend_prompt();
	test_a_matched_prompt_is_consumed();
	test_switching_backend_clears_the_window();
	test_reset_forgets_the_carry();
	test_inputs_that_must_be_refused_not_trusted();

	if (fails) {
		printf("%d failure(s)\n", fails);
		return 1;
	}
	printf("test_debprompt: all checks passed\n");
	return 0;
}
