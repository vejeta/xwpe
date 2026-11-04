/* test_debsess.c -- unit test for the session state and the readers of what
 * the debuggers print.  Links we_debsess.c only; no editor, no curses, no
 * debugger -- always runs (no skip).
 *
 * The words fed in are what gdb, pdb, jdb and a68g printed (captured from
 * the real debuggers; pdb's paths shortened to /work/p.py), and what the
 * stubs of tests/test_legacy_debuggers.py print for sdb, dbx and xdb.  They
 * live in tests/transcripts/.  A prompt has no line end and its trailing
 * space would not survive in a text file, so the test adds the prompt
 * after the transcript, as the debugger writes it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_debsess.h"

#define TRANSCRIPT_MAX  8192
#define TRANSCRIPT_DIR  "tests/transcripts/"
#define PATH_ROOM       1024
#define ANSWER_MAX      1024

#define JDB_PROMPT   "main[1] "
#define PDB_PROMPT   "(Pdb) "
#define A68G_PROMPT  "(a68g) "

static int fails;

/* The session is far too large for the stack of every platform. */
static E_D_SESSION sess;

static void chk_int(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL: %s: got %d want %d\n", what, got, want);
		fails++;
	}
}

static void chk_str(const char *what, const char *got, const char *want)
{
	if (!got || strcmp(got, want) != 0) {
		printf("FAIL: %s: got \"%s\" want \"%s\"\n", what,
		       got ? got : "(null)", want);
		fails++;
	}
}

/* The transcript @name, read whole, with @prompt (may be "") after it. */
static const char *transcript(const char *name, const char *prompt)
{
	static char buf[TRANSCRIPT_MAX];
	char path[PATH_ROOM];
	const char *srcdir = getenv("srcdir");
	FILE *fp;
	size_t n;

	if (!srcdir || !srcdir[0])
		srcdir = ".";
	snprintf(path, sizeof path, "%s/%s%s", srcdir, TRANSCRIPT_DIR, name);
	fp = fopen(path, "r");
	if (!fp) {
		printf("FAIL: cannot read %s\n", path);
		fails++;
		buf[0] = '\0';
		return buf;
	}
	n = fread(buf, 1, sizeof buf - 1 - strlen(prompt), fp);
	fclose(fp);
	buf[n] = '\0';
	strcat(buf, prompt);
	return buf;
}

/* Line @k (0-based) of @text, without its end, into @out. */
static const char *line_of(const char *text, int k, char *out, size_t n)
{
	const char *end;
	size_t len;

	while (k-- > 0 && text) {
		text = strchr(text, '\n');
		if (text)
			text++;
	}
	if (!text) {
		out[0] = '\0';
		return out;
	}
	end = strchr(text, '\n');
	len = end ? (size_t)(end - text) : strlen(text);
	if (len >= n)
		len = n - 1;
	memcpy(out, text, len);
	out[len] = '\0';
	return out;
}

/* An answer's lines joined as the editor joins them for a watch: a line
   end between two lines becomes a space. */
static const char *joined(const char *text, char *out, size_t n)
{
	size_t len;

	snprintf(out, n, "%s", text);
	len = strlen(out);
	while (len > 1 && out[len - 1] == '\n' && out[len - 2] == '\n')
		out[--len] = '\0';
	for (len = 0; out[len]; len++)
		if (out[len] == '\n' && out[len + 1] != '\0')
			out[len] = ' ';
	return out;
}

/* The file probe of the tests: only the names in @ctx (a NULL-ended list)
   exist. */
static int probe_listed(const char *file, void *ctx)
{
	const char **names = (const char **)ctx;

	for (; *names; names++)
		if (strcmp(*names, file) == 0)
			return 1;
	return 0;
}

/* One watch answer through the session, @error set when it came from the
   debugger's error stream: the state and the words shown. */
static void watch_case_from(const char *what, int backend, const char *answer,
                            int error, int want_state, const char *want_text)
{
	E_D_ANSWER a;
	char label[256];

	e_ds_reset(&sess);
	e_ds_set_watch(&sess, 0, "v");
	e_ds_set_watch(&sess, 1, "w");
	e_ds_answer_borrow(&a, answer, error);
	e_ds_take_watch_answer(&sess, backend, 0, &a);
	snprintf(label, sizeof label, "%s: state", what);
	chk_int(label, sess.watches[0].state, want_state);
	snprintf(label, sizeof label, "%s: value", what);
	chk_str(label, sess.watches[0].value, want_text);
	snprintf(label, sizeof label, "%s: expression kept", what);
	chk_str(label, sess.watches[0].expr, "v");
	snprintf(label, sizeof label, "%s: the other watch still waits", what);
	chk_int(label, sess.watches[1].state, E_DS_WATCH_WAITING);
	snprintf(label, sizeof label, "%s: changed says watches", what);
	chk_int(label, (sess.changed & E_DS_CHANGED_WATCHES) != 0, 1);
}

/* One watch answer read from the debugger's output, as the editor reads it. */
static void watch_case(const char *what, int backend, const char *answer,
                       int want_state, const char *want_text)
{
	watch_case_from(what, backend, answer, 0, want_state, want_text);
}

/* 7: the value of a watch, and the debugger's reason when it has none. */
static void test_watch_answer_fills_the_value_and_an_error_keeps_waiting_text(void)
{
	char buf[ANSWER_MAX], row[ANSWER_MAX];

	watch_case("gdb value", E_DS_GDB,
	           joined(transcript("gdb-watch-value.txt", ""), buf, sizeof buf),
	           E_DS_WATCH_VALUE, "3\n");
	/* gdb writes "No symbol ..." on its error stream, so its output holds
	   only the prompt: the session's own words, whole. */
	watch_case("gdb undefined name", E_DS_GDB, NULL,
	           E_DS_WATCH_ERROR, E_DS_NO_SYMBOL);
	watch_case_from("gdb error stream", E_DS_GDB,
	                joined(transcript("gdb-watch-error.txt", ""), buf,
	                       sizeof buf),
	                1, E_DS_WATCH_ERROR, E_DS_NO_SYMBOL);
	/* An answer with no "=" carries no value: the session's own words,
	   their first letter included, for every backend that reads "=". */
	line_of(transcript("legacy-stub-rows.txt", ""), 3, row, sizeof row);
	watch_case("dbx answer without =", E_DS_DBX, row,
	           E_DS_WATCH_ERROR, E_DS_NO_SYMBOL);
	watch_case("gdb answer without =", E_DS_GDB, row,
	           E_DS_WATCH_ERROR, E_DS_NO_SYMBOL);
	watch_case("jdb answer without =", E_DS_JDB, row,
	           E_DS_WATCH_ERROR, E_DS_NO_SYMBOL);
	watch_case("xdb answer without =", E_DS_XDB, row,
	           E_DS_WATCH_ERROR, E_DS_NO_SYMBOL);
	watch_case("pdb value", E_DS_PDB,
	           joined(transcript("pdb-watch-value.txt", ""), buf, sizeof buf),
	           E_DS_WATCH_VALUE, "3\n");
	watch_case("pdb error", E_DS_PDB,
	           joined(transcript("pdb-watch-error.txt", ""), buf, sizeof buf),
	           E_DS_WATCH_ERROR,
	           "*** NameError: name 'nosuch' is not defined\n");
	watch_case("jdb value", E_DS_JDB,
	           joined(transcript("jdb-watch-value.txt", ""), buf, sizeof buf),
	           E_DS_WATCH_VALUE, "3\n");
	watch_case("a68g value", E_DS_A68G,
	           joined(transcript("a68g-watch-value.txt", ""), buf, sizeof buf),
	           E_DS_WATCH_VALUE, "(INT) +3\n");
	watch_case("a68g out of scope", E_DS_A68G,
	           joined(transcript("a68g-watch-error.txt", ""), buf, sizeof buf),
	           E_DS_WATCH_ERROR, E_DS_NOT_IN_SCOPE);
	line_of(transcript("legacy-stub-rows.txt", ""), 0, row, sizeof row);
	watch_case("sdb value", E_DS_SDB, row, E_DS_WATCH_VALUE, "3");
	watch_case("sdb no answer", E_DS_SDB, NULL, E_DS_WATCH_ERROR,
	           E_DS_NO_SYMBOL);
}

/* 8: the frames of a "where", and the one whose line is marked. */
static void test_cli_where_rows_mark_the_innermost_frame_with_source(void)
{
	static const char *only_s_c[] = { "s.c", NULL };
	const char *text;
	char row[ANSWER_MAX];
	int k;

	/* gdb stopped in libc's nanosleep, under the user's tick(). */
	text = transcript("gdb-where-library.txt", "");
	e_ds_reset(&sess);
	e_ds_begin_stack(&sess);
	for (k = 0; k < 4; k++)
		chk_int("gdb library row is a frame",
		        e_ds_take_where_row(&sess, E_DS_GDB,
		                            line_of(text, k, row, sizeof row)), 1);
	chk_int("gdb library: frames", sess.nframes, 4);
	chk_str("gdb library: innermost file", sess.frames[0].file,
	        "../sysdeps/unix/sysv/linux/nanosleep.c");
	chk_int("gdb library: innermost line", sess.frames[0].line, 25);
	chk_int("gdb library: level of #2", sess.frames[2].level, 2);
	chk_str("gdb library: #2 file", sess.frames[2].file, "s.c");
	chk_int("gdb library: #2 line", sess.frames[2].line, 4);
	chk_int("gdb library: marked frame has source",
	        e_ds_frame_to_mark(&sess, probe_listed, (void *)only_s_c), 2);
	chk_int("gdb library: no probe marks the innermost",
	        e_ds_frame_to_mark(&sess, NULL, NULL), 0);
	chk_int("gdb library: row number of #3",
	        e_ds_where_row_frame(line_of(text, 3, row, sizeof row)), 3);

	/* gdb stopped in the user's sq(). */
	text = transcript("gdb-where.txt", "");
	e_ds_begin_stack(&sess);
	e_ds_take_where_row(&sess, E_DS_GDB, line_of(text, 0, row, sizeof row));
	e_ds_take_where_row(&sess, E_DS_GDB, line_of(text, 1, row, sizeof row));
	chk_int("gdb: frames", sess.nframes, 2);
	chk_str("gdb: #1 file", sess.frames[1].file, "p.c");
	chk_int("gdb: #1 line", sess.frames[1].line, 8);
	chk_int("gdb: stack changed", (sess.changed & E_DS_CHANGED_STACK) != 0, 1);

	/* The sdb and dbx stubs' rows. */
	text = transcript("legacy-stub-rows.txt", "");
	e_ds_begin_stack(&sess);
	chk_int("sdb t row is a frame",
	        e_ds_take_where_row(&sess, E_DS_SDB,
	                            line_of(text, 1, row, sizeof row)), 1);
	chk_str("sdb t row: file", sess.frames[0].file, "p.c");
	chk_int("sdb t row: line", sess.frames[0].line, 7);
	e_ds_begin_stack(&sess);
	chk_int("dbx where row is a frame",
	        e_ds_take_where_row(&sess, E_DS_DBX,
	                            line_of(text, 2, row, sizeof row)), 1);
	chk_str("dbx where row: file", sess.frames[0].file, "p.c");
	chk_int("dbx where row: line", sess.frames[0].line, 7);
	chk_int("dbx: a value row is no frame",
	        e_ds_take_where_row(&sess, E_DS_DBX,
	                            line_of(text, 0, row, sizeof row)), 0);
}

/* 9: where pdb stopped, and a68g's stop at its monitor prompt. */
static void test_pdb_stop_place_from_a_transcript(void)
{
	static const char *p_py[] = { "/work/p.py", NULL };
	const char *text = transcript("pdb-stop.txt", PDB_PROMPT);
	char file[PATH_ROOM];
	int line = 0;

	chk_int("pdb: the answer names a place",
	        e_ds_pdb_stop_place(text, file, sizeof file, &line,
	                            probe_listed, (void *)p_py), 1);
	chk_str("pdb: file", file, "/work/p.py");
	chk_int("pdb: line", line, 2);
	e_ds_reset(&sess);
	e_ds_set_file_probe(&sess, probe_listed, (void *)p_py);
	chk_int("pdb: the session takes the stop",
	        e_ds_take_cli_stop_line(&sess, E_DS_PDB, text), 1);
	chk_int("pdb: stopped", sess.phase, E_DS_STOPPED);
	chk_str("pdb: frame file", sess.frames[0].file, "/work/p.py");
	chk_int("pdb: frame line", sess.frames[0].line, 2);
	chk_str("pdb: frame function", sess.frames[0].func, "sq");
	chk_int("pdb: mark changed", (sess.changed & E_DS_CHANGED_MARK) != 0, 1);
}

/* The kind a68g's line @k of @text is, as the stop reader sees it. */
static int a68g_kind_of_line(const char *text, int k)
{
	char row[ANSWER_MAX];

	line_of(text, k, row, sizeof row);
	return e_ds_a68g_line_kind(row, (int)strlen(row));
}

static void test_a68g_monitor_stop_line(void)
{
	const char *text = transcript("a68g-stop.txt", A68G_PROMPT);
	char row[ANSWER_MAX];
	int kinds[E_D_A68G_STOP_LINES];

	kinds[0] = a68g_kind_of_line(text, 4);
	kinds[1] = a68g_kind_of_line(text, 3);
	kinds[2] = a68g_kind_of_line(text, 2);
	line_of(text, 2, row, sizeof row);
	chk_int("a68g: thread line", kinds[0], E_D_A68G_LINE_THREAD);
	chk_int("a68g: column marker", kinds[1], E_D_A68G_LINE_MARKER);
	chk_int("a68g: source line", kinds[2], E_D_A68G_LINE_SOURCE);
	chk_int("a68g: the three lines make a stop",
	        e_ds_a68g_lines_make_a_stop(kinds), 1);
	chk_int("a68g: the line", e_ds_a68g_parse_line(row), 4);
	e_ds_reset(&sess);
	chk_int("a68g: the session takes the stop",
	        e_ds_take_cli_stop_line(&sess, E_DS_A68G, text), 1);
	chk_int("a68g: frame line", sess.frames[0].line, 4);
	chk_int("a68g: stopped", sess.phase, E_DS_STOPPED);
}

/* 10: JDK-8213954: jdb writes its answer to a breakpoint set while the
   program runs into the middle of its report of a hit.  The stop and the
   set answer are each read for what they are. */
static void test_jdb_breakpoint_hit_before_its_set_answer_is_kept_apart(void)
{
	const char *text = transcript("jdb-interleave-8213954.txt", JDB_PROMPT);
	char row[ANSWER_MAX], spec[ANSWER_MAX], cls[ANSWER_MAX];
	const char *words = NULL, *stop;
	E_D_ANSWER a;
	int k, state = E_DS_NOT_FOUND;

	chk_int("jdb: the reply is a stop", e_ds_jdb_reply_is_stop(text), 1);
	stop = e_ds_jdb_stop_line_in(text);
	chk_int("jdb: a stop line is found", stop != NULL, 1);
	if (stop) {
		chk_int("jdb: stop's line", e_ds_jdb_stop_line(stop), 6);
		chk_int("jdb: stop names a class",
		        e_ds_jdb_stop_class(stop, cls, sizeof cls), 1);
		chk_str("jdb: stop's class", cls, "P");
	}
	e_ds_reset(&sess);
	e_ds_set_brk(&sess, 0, "P.java", 6);
	chk_int("jdb: the session takes the stop",
	        e_ds_take_cli_stop_line(&sess, E_DS_JDB, text), 1);
	chk_int("jdb: reason", sess.stop_reason, E_DS_STOP_BREAKPOINT);
	chk_str("jdb: frame class", sess.frames[0].file, "P");
	chk_int("jdb: frame line", sess.frames[0].line, 6);
	chk_int("jdb: the stop alone does not verify the breakpoint",
	        sess.brks[0].state, E_D_BRK_PENDING);
	for (k = 0; k < 6; k++) {
		line_of(text, k, row, sizeof row);
		state = e_ds_jdb_live_word(row, spec, sizeof spec, &words);
		if (state != E_DS_NOT_FOUND)
			break;
	}
	chk_int("jdb: the set answer is found on the hit's line", k, 1);
	chk_int("jdb: the set answer verifies", state, E_D_BRK_VERIFIED);
	chk_str("jdb: the set answer's spec", spec, "P:6");
	e_ds_answer_borrow(&a, row, 0);
	e_ds_take_brk_answer(&sess, E_DS_JDB, 0, &a);
	chk_int("jdb: breakpoint verified", sess.brks[0].state, E_D_BRK_VERIFIED);
	chk_str("jdb: no words kept for a set", sess.brks[0].msg, "");
	chk_int("jdb: the stop is still line 6", sess.frames[0].line, 6);

	/* The ordinary order, for comparison: the same stop. */
	text = transcript("jdb-stop.txt", JDB_PROMPT);
	e_ds_reset(&sess);
	chk_int("jdb plain: the session takes the stop",
	        e_ds_take_cli_stop_line(&sess, E_DS_JDB, text), 1);
	chk_int("jdb plain: frame line", sess.frames[0].line, 3);
	chk_str("jdb plain: frame class", sess.frames[0].file, "P");
}

/* gdb's answer to "b p.c:3" in each of its three shapes, through the one
   answer type: its number, its refusal on the error stream, nothing. */
static void test_gdb_breakpoint_answer_in_each_shape(void)
{
	E_D_ANSWER a;

	e_ds_reset(&sess);
	e_ds_set_brk(&sess, 0, "p.c", 3);
	e_ds_answer_borrow(&a, "Breakpoint 2 at 0x1149: file p.c, line 3.\n", 0);
	e_ds_take_brk_answer(&sess, E_DS_GDB, 0, &a);
	chk_int("gdb brk: verified", sess.brks[0].state, E_D_BRK_VERIFIED);
	chk_int("gdb brk: its number", sess.brks[0].id, 2);
	e_ds_answer_borrow(&a, "No line 99 in file \"p.c\".\n", 1);
	e_ds_take_brk_answer(&sess, E_DS_GDB, 0, &a);
	chk_int("gdb brk refused: state", sess.brks[0].state, E_D_BRK_REFUSED);
	chk_str("gdb brk refused: words", sess.brks[0].msg,
	        "No line 99 in file \"p.c\".");
	e_ds_answer_borrow(&a, NULL, 0);
	e_ds_take_brk_answer(&sess, E_DS_GDB, 0, &a);
	chk_int("gdb brk no answer: state", sess.brks[0].state, E_D_BRK_REFUSED);
	chk_str("gdb brk no answer: words", sess.brks[0].msg, E_D_BRK_NO_NUMBER);
}

int main(void)
{
	test_watch_answer_fills_the_value_and_an_error_keeps_waiting_text();
	test_cli_where_rows_mark_the_innermost_frame_with_source();
	test_pdb_stop_place_from_a_transcript();
	test_a68g_monitor_stop_line();
	test_jdb_breakpoint_hit_before_its_set_answer_is_kept_apart();
	test_gdb_breakpoint_answer_in_each_shape();
	if (fails) {
		printf("test_debsess: %d check(s) failed\n", fails);
		return 1;
	}
	printf("test_debsess: all checks passed\n");
	return 0;
}
