/* test_gdbmi.c -- unit test for the gdb/MI record parser, the line splitter
 * and what the session makes of gdb's records.  Links we_gdbmi.c and
 * we_debsess.c only; no editor, no curses, no debugger -- always runs
 * (no skip).
 *
 * The lines fed in are what gdb 18.1 printed on a small C program, with
 * its commands numbered (tests/transcripts/gdb-mi-*.txt).  Lines that
 * start with '#' in a transcript say how it was recorded and are not
 * records.  gdb ends its prompt line "(gdb) " with a space that a text
 * file does not keep; the parser takes both, and the parser cases below
 * feed the spaced form.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_gdbmi.h"
#include "../we_debsess.h"

#define TRANSCRIPT_MAX  16384
#define TRANSCRIPT_DIR  "tests/transcripts/"
#define PATH_ROOM       1024
#define LINE_ROOM       E_GDBMI_LINE_MAX
#define READ_LINES_MAX  64
#define OVERLONG_EXTRA  100
#define NESTING_EXTRA   8
/* gdb-mi-stop-breakpoint.txt closes the run's answer and the stop with a
   (gdb) line each. */
#define TRANSCRIPT_PROMPTS 2

static int fails;

/* The session and the records are far too large for the stack of every
   platform. */
static E_D_SESSION sess;
static E_GDBMI_RECORD rec;
static E_GDBMI_SPLIT split;

static void chk_int(const char *what, long got, long want)
{
	if (got != want) {
		printf("FAIL: %s: got %ld want %ld\n", what, got, want);
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

static void chk_has(const char *what, const char *got, const char *want)
{
	if (!got || !strstr(got, want)) {
		printf("FAIL: %s: \"%s\" lacks \"%s\"\n", what,
		       got ? got : "(null)", want);
		fails++;
	}
}

static void chk_mem(const char *what, const char *got, size_t got_len,
                    const char *want, size_t want_len)
{
	if (got_len != want_len || memcmp(got, want, want_len) != 0) {
		printf("FAIL: %s: %lu bytes differ from %lu expected\n", what,
		       (unsigned long)got_len, (unsigned long)want_len);
		fails++;
	}
}

/* The transcript @name, read whole. */
static const char *transcript(const char *name)
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
	n = fread(buf, 1, sizeof buf - 1, fp);
	fclose(fp);
	buf[n] = '\0';
	return buf;
}

/* The records of a transcript as gdb wrote them: its text without the
   lines that start with '#'. */
static const char *records_of(const char *name)
{
	static char body[TRANSCRIPT_MAX];
	const char *p = transcript(name);
	size_t used = 0;

	while (*p) {
		const char *end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) + 1 : strlen(p);

		if (*p != '#') {
			memcpy(body + used, p, len);
			used += len;
		}
		p += len;
	}
	body[used] = '\0';
	return body;
}

/* The next line of @*pos into @out, without its end; 0 at the end. */
static int next_line(const char **pos, char *out, size_t n)
{
	const char *end;
	size_t len;

	if (!**pos)
		return 0;
	end = strchr(*pos, '\n');
	len = end ? (size_t)(end - *pos) : strlen(*pos);
	if (len >= n)
		len = n - 1;
	memcpy(out, *pos, len);
	out[len] = '\0';
	*pos = end ? end + 1 : *pos + strlen(*pos);
	return 1;
}

/* Every record of the transcript @name into the session, in order.
   Return: how many records were taken. */
static int take_transcript(const char *name)
{
	static char line[LINE_ROOM];
	const char *pos = records_of(name);
	int n = 0, rc;

	while (next_line(&pos, line, sizeof line)) {
		rc = e_gdbmi_parse_line(line, &rec);
		if (rc != E_GDBMI_OK) {
			printf("FAIL: %s: line does not parse (%d): %.60s\n",
			       name, rc, line);
			fails++;
			continue;
		}
		e_ds_take_mi_record(&sess, &rec);
		n++;
	}
	return n;
}

/* The first record of kind @kind in the transcript @name, into @rec.
   Return: 1 when there is one. */
static int record_of(const char *name, int kind, const char *klass)
{
	static char line[LINE_ROOM];
	const char *pos = records_of(name);

	while (next_line(&pos, line, sizeof line)) {
		if (e_gdbmi_parse_line(line, &rec) != E_GDBMI_OK)
			continue;
		if (rec.kind == kind && (!klass || !strcmp(rec.klass, klass)))
			return 1;
	}
	printf("FAIL: %s has no record of kind %d\n", name, kind);
	fails++;
	return 0;
}

/* ---- the session: §6 tests 1 to 6 ---- */

/* 1: the stop names its place, and the user's bar and the Stack window get
   it. */
static void test_mi_stop_record_names_file_and_line(void)
{
	e_ds_reset(&sess);
	take_transcript("gdb-mi-stop-breakpoint.txt");
	chk_int("stop: phase", sess.phase, E_DS_STOPPED);
	chk_int("stop: reason", sess.stop_reason, E_DS_STOP_BREAKPOINT);
	chk_int("stop: one frame", sess.nframes, 1);
	chk_str("stop: file", sess.frames[0].file, "/work/p.c");
	chk_int("stop: line", sess.frames[0].line, 7);
	chk_str("stop: function", sess.frames[0].func, "sq");
	chk_int("stop: level", sess.frames[0].level, 0);
	chk_int("stop: the mark is the innermost frame", sess.marked_frame, 0);
	chk_int("stop: mark changed", (sess.changed & E_DS_CHANGED_MARK) != 0, 1);
	chk_int("stop: status changed",
	        (sess.changed & E_DS_CHANGED_STATUS) != 0, 1);
	chk_int("stop: not the editor's own", sess.own_stop, 0);

	take_transcript("gdb-mi-step-console.txt");
	chk_int("step: reason", sess.stop_reason, E_DS_STOP_STEP);
	chk_int("step: line", sess.frames[0].line, 8);

	take_transcript("gdb-mi-stack-eval.txt");
	chk_int("stack: frames", sess.nframes, 2);
	chk_str("stack: #1 function", sess.frames[1].func, "main");
	chk_int("stack: #1 line", sess.frames[1].line, 14);
	chk_int("stack: #1 level", sess.frames[1].level, 1);
	chk_int("stack: changed", (sess.changed & E_DS_CHANGED_STACK) != 0, 1);
	chk_int("stack: the marked frame", sess.marked_frame, 0);
	chk_int("stack: the mark stays on the innermost frame with source",
	        e_ds_frame_to_mark(&sess, NULL, NULL), 0);
}

/* What the splitter handed out for one read, and how the session fared. */
static int prompts_seen;

/* One read of @n bytes into the splitter, every whole line into the session.
   Return: how many lines the read finished. */
static int feed_read(const char *data, size_t n)
{
	size_t used = 0;
	const char *line;
	int count = 0, rc;

	while (e_gdbmi_split_next(&split, data, n, &used, &line)
	       == E_GDBMI_SPLIT_LINE) {
		rc = e_gdbmi_parse_line(line, &rec);
		count++;
		if (rc != E_GDBMI_OK) {
			printf("FAIL: split line does not parse: %.60s\n", line);
			fails++;
			continue;
		}
		if (rec.kind == E_GDBMI_K_PROMPT)
			prompts_seen++;
		e_ds_take_mi_record(&sess, &rec);
	}
	return count;
}

/* The offset in @text where line @k starts. */
static size_t line_start(const char *text, int k)
{
	const char *p = text;

	while (k-- > 0)
		p = strchr(p, '\n') + 1;
	return (size_t)(p - text);
}

/* How many lines @text has. */
static int line_count(const char *text)
{
	int n = 0;

	for (; *text; text++)
		if (*text == '\n')
			n++;
	return n;
}

/* 2: gdb's stop comes in pieces.  A read that ends inside the *stopped line
   shows no stop yet; the read that finishes it does; the (gdb) line that
   arrives alone in the second read is a record of its own.  And wherever
   the cut falls, the lines are the same. */
static void test_mi_stop_record_split_across_two_reads(void)
{
	const char *text = records_of("gdb-mi-stop-breakpoint.txt");
	size_t len = strlen(text);
	int lines = line_count(text);
	size_t cut, stopped_at, mid, prompt_at;
	int whole;

	/* The whole text in one read: the reference. */
	e_ds_reset(&sess);
	e_gdbmi_split_init(&split);
	prompts_seen = 0;
	whole = feed_read(text, len);
	chk_int("one read: every line", whole, lines);
	chk_int("one read: stopped", sess.phase, E_DS_STOPPED);
	chk_int("one read: both prompts", prompts_seen, TRANSCRIPT_PROMPTS);

	/* A cut in the middle of the *stopped line. */
	stopped_at = line_start(text, lines - 2);
	mid = stopped_at + 20;
	e_ds_reset(&sess);
	e_gdbmi_split_init(&split);
	prompts_seen = 0;
	feed_read(text, mid);
	chk_int("cut inside *stopped: still running", sess.phase, E_DS_RUNNING);
	chk_int("cut inside *stopped: the first prompt only", prompts_seen, 1);
	feed_read(text + mid, len - mid);
	chk_int("cut inside *stopped: stopped after the rest", sess.phase,
	        E_DS_STOPPED);
	chk_str("cut inside *stopped: file", sess.frames[0].file, "/work/p.c");
	chk_int("cut inside *stopped: the last prompt", prompts_seen,
	        TRANSCRIPT_PROMPTS);

	/* The closing (gdb) line alone in the second read. */
	prompt_at = line_start(text, lines - 1);
	e_ds_reset(&sess);
	e_gdbmi_split_init(&split);
	prompts_seen = 0;
	feed_read(text, prompt_at);
	chk_int("prompt in the second read: stopped by the first",
	        sess.phase, E_DS_STOPPED);
	chk_int("prompt in the second read: not yet", prompts_seen,
	        TRANSCRIPT_PROMPTS - 1);
	feed_read(text + prompt_at, len - prompt_at);
	chk_int("prompt in the second read: seen", prompts_seen,
	        TRANSCRIPT_PROMPTS);

	/* Every byte position as the cut. */
	for (cut = 0; cut <= len; cut++) {
		int total;

		e_ds_reset(&sess);
		e_gdbmi_split_init(&split);
		prompts_seen = 0;
		total = feed_read(text, cut);
		total += feed_read(text + cut, len - cut);
		if (total != lines || sess.phase != E_DS_STOPPED
		    || sess.frames[0].line != 7
		    || prompts_seen != TRANSCRIPT_PROMPTS) {
			printf("FAIL: cut at %lu: %d lines of %d, phase %d,"
			       " line %d, prompts %d\n", (unsigned long)cut,
			       total, lines, sess.phase, sess.frames[0].line,
			       prompts_seen);
			fails++;
			break;
		}
	}

	/* One byte per read. */
	e_ds_reset(&sess);
	e_gdbmi_split_init(&split);
	prompts_seen = 0;
	whole = 0;
	for (cut = 0; cut < len; cut++)
		whole += feed_read(text + cut, 1);
	chk_int("byte by byte: every line", whole, lines);
	chk_int("byte by byte: stopped", sess.phase, E_DS_STOPPED);
}

/* 3: -break-insert's answer: the entry is verified, held pending, or refused
   with gdb's own message. */
static void test_mi_break_insert_answer_verifies_the_entry(void)
{
	e_ds_reset(&sess);
	e_ds_set_brk(&sess, 0, "p.c", 6);
	chk_int("brk: before the answer", sess.brks[0].state, E_D_BRK_PENDING);
	if (record_of("gdb-mi-break-insert.txt", E_GDBMI_K_RESULT, "done"))
		chk_int("brk: the answer is for a breakpoint",
		        e_ds_take_mi_brk_result(&sess, 0, &rec), 1);
	chk_int("brk: verified", sess.brks[0].state, E_D_BRK_VERIFIED);
	chk_int("brk: gdb's number", sess.brks[0].id, 1);
	chk_str("brk: no words", sess.brks[0].msg, "");
	chk_int("brk: changed", (sess.changed & E_DS_CHANGED_BRKS) != 0, 1);

	e_ds_set_brk(&sess, 1, "nosuch.c", 5);
	if (record_of("gdb-mi-break-pending.txt", E_GDBMI_K_RESULT, "done"))
		e_ds_take_mi_brk_result(&sess, 1, &rec);
	chk_int("brk pending: still hollow", sess.brks[1].state,
	        E_D_BRK_PENDING);
	chk_int("brk pending: gdb's number", sess.brks[1].id, 1);
	chk_has("brk pending: says what it waits for", sess.brks[1].msg,
	        "nosuch.c:5");

	/* A breakpoint with several places: the places have no name. */
	e_ds_set_brk(&sess, 2, "t.cc", 3);
	if (record_of("gdb-mi-break-multiple.txt", E_GDBMI_K_RESULT, "done")) {
		const E_GDBMI_NODE *place;

		e_ds_take_mi_brk_result(&sess, 2, &rec);
		chk_str("brk multiple: the breakpoint", e_gdbmi_get(&rec,
		        "bkpt.number"), "1");
		place = e_gdbmi_next_item(&rec, e_gdbmi_find(&rec, "bkpt"));
		chk_str("brk multiple: the first place has no name",
		        e_gdbmi_item_name(&rec, place), "");
		chk_str("brk multiple: its number",
		        e_gdbmi_get_in(&rec, place, "number"), "1.1");
		place = e_gdbmi_next_item(&rec, place);
		chk_str("brk multiple: the second place's function",
		        e_gdbmi_get_in(&rec, place, "func"), "twice<double>(double)");
		chk_int("brk multiple: no third",
		        e_gdbmi_next_item(&rec, place) == NULL, 1);
	}
	chk_int("brk multiple: verified", sess.brks[2].state, E_D_BRK_VERIFIED);
	chk_int("brk multiple: gdb's number", sess.brks[2].id, 1);
}

static void test_mi_break_insert_error_refuses_with_its_message(void)
{
	e_ds_reset(&sess);
	e_ds_set_brk(&sess, 0, "nosuch.c", 5);
	if (record_of("gdb-mi-break-error.txt", E_GDBMI_K_RESULT, "error"))
		chk_int("brk error: the answer is for a breakpoint",
		        e_ds_take_mi_brk_result(&sess, 0, &rec), 1);
	chk_int("brk error: refused", sess.brks[0].state, E_D_BRK_REFUSED);
	chk_str("brk error: gdb's words, without its styling",
	        sess.brks[0].msg, "No source file named nosuch.c.");
	chk_int("brk error: no number", sess.brks[0].id, 0);

	/* An answer that is not for a breakpoint changes nothing. */
	e_ds_set_brk(&sess, 1, "p.c", 6);
	if (record_of("gdb-mi-stack-eval.txt", E_GDBMI_K_RESULT, "done"))
		chk_int("brk: a stack answer is not a breakpoint's",
		        e_ds_take_mi_brk_result(&sess, 1, &rec), 0);
	chk_int("brk: left hollow", sess.brks[1].state, E_D_BRK_PENDING);
	chk_int("brk: a bad index is refused",
	        e_ds_take_mi_brk_result(&sess, E_DS_BRKS_MAX, &rec), 0);
}

/* 4: a SIGINT stop is the user's Pause, the editor's own interrupt, or the
   program's, by what the editor said it was doing. */
static void test_mi_sigint_stop_during_a_sync_interrupt_is_not_the_users(void)
{
	/* The editor interrupted to apply a breakpoint change. */
	e_ds_reset(&sess);
	e_ds_set_interrupt(&sess, E_DS_INT_SYNC);
	chk_int("sync: interrupt recorded", sess.interrupt_for, E_DS_INT_SYNC);
	take_transcript("gdb-mi-sigint.txt");
	chk_int("sync: stopped", sess.phase, E_DS_STOPPED);
	chk_int("sync: reason is the interrupt, not a signal",
	        sess.stop_reason, E_DS_STOP_PAUSE);
	chk_int("sync: the editor's own stop", sess.own_stop, 1);
	chk_int("sync: the interrupt is used up", sess.interrupt_for,
	        E_DS_INT_NONE);
	chk_int("sync: nothing for the user's bar",
	        (sess.changed & E_DS_CHANGED_MARK) != 0, 0);

	/* The user's Pause. */
	e_ds_reset(&sess);
	e_ds_set_interrupt(&sess, E_DS_INT_PAUSE);
	take_transcript("gdb-mi-sigint.txt");
	chk_int("pause: reason", sess.stop_reason, E_DS_STOP_PAUSE);
	chk_int("pause: shown to the user", sess.own_stop, 0);
	chk_int("pause: the mark", (sess.changed & E_DS_CHANGED_MARK) != 0, 1);
	chk_int("pause: the interrupt is used up", sess.interrupt_for,
	        E_DS_INT_NONE);
	chk_str("pause: the library frame", sess.frames[0].func,
	        "__internal_syscall_cancel");
	chk_int("pause: its line", sess.frames[0].line, 44);

	/* The program's own SIGINT, no interrupt in flight. */
	e_ds_reset(&sess);
	take_transcript("gdb-mi-sigint.txt");
	chk_int("program's: reason", sess.stop_reason, E_DS_STOP_SIGNAL);
	chk_str("program's: the signal", sess.signal, "SIGINT");
	chk_int("program's: shown to the user", sess.own_stop, 0);
}

/* 5: the program's end gives the exit code, which gdb writes in octal. */
static void test_mi_exited_record_gives_the_exit_code(void)
{
	e_ds_reset(&sess);
	take_transcript("gdb-mi-exit.txt");
	chk_int("exit: ended", sess.phase, E_DS_ENDED);
	chk_int("exit: reason", sess.stop_reason, E_DS_STOP_EXITED);
	chk_int("exit: the code", sess.exit_code, 3);
	chk_int("exit: ended bit", (sess.changed & E_DS_CHANGED_ENDED) != 0, 1);

	e_ds_reset(&sess);
	take_transcript("gdb-mi-exit-octal.txt");
	chk_int("exit 012: the code is ten", sess.exit_code, 10);
	chk_int("exit 012: ended", sess.phase, E_DS_ENDED);
}

/* 6: gdb's own words, and the log and target records, wait for Messages in
   the order they came, without gdb's styling. */
static void test_mi_console_records_are_kept_for_messages(void)
{
	e_ds_reset(&sess);
	take_transcript("gdb-mi-stop-breakpoint.txt");
	chk_int("console: changed", (sess.changed & E_DS_CHANGED_CONSOLE) != 0,
	        1);
	chk_has("console: the stop's words", sess.console,
	        "Breakpoint 1, sq (v=3) at p.c:7\n7\t\tint r = v * v;\n");
	chk_int("console: length", sess.console_len,
	        (int)strlen(sess.console));
	chk_int("console: nothing lost", sess.console_lost, 0);

	/* Records gdb splits in two read as one text. */
	e_ds_reset(&sess);
	take_transcript("gdb-mi-sigint.txt");
	chk_has("console: split records join", sess.console,
	        "\nProgram received signal SIGINT, Interrupt.\n");
	chk_has("console: the log record", sess.console,
	        "warning: 44\t./nptl/cancellation.c: No such file or directory");

	/* A refusal's styling does not reach the status row. */
	e_ds_reset(&sess);
	take_transcript("gdb-mi-break-error.txt");
	chk_str("console: an error answer is the status row's words",
	        sess.last_error, "No source file named nosuch.c.");

	/* Shown: forgotten. */
	e_ds_console_clear(&sess);
	chk_int("console: cleared", sess.console_len, 0);
	chk_str("console: empty text", sess.console, "");

	/* Too much for the room: the rest is counted, not overflowed. */
	e_ds_reset(&sess);
	{
		int k;

		for (k = 0; k < E_DS_CONSOLE_MAX; k++)
			take_transcript("gdb-mi-step-console.txt");
	}
	chk_int("console: full", sess.console_len < E_DS_CONSOLE_MAX, 1);
	chk_int("console: the overflow is counted", sess.console_lost > 0, 1);
}

static int parse_ok(const char *what, const char *line);

/* Only files of the user's own tree are there to be shown. */
static int only_the_users_files(const char *file, void *ctx)
{
	(void)ctx;
	return strncmp(file, "/work/", strlen("/work/")) == 0;
}

/* A stack whose only frame is inside the C library (gdb names it by the
   library's build tree) gives the user's bar no line to go to. */
static void test_mi_stack_mark_needs_a_file_that_is_there(void)
{
	e_ds_reset(&sess);
	e_ds_set_file_probe(&sess, only_the_users_files, NULL);
	take_transcript("gdb-mi-sigint.txt");
	chk_int("library stack: one frame", sess.nframes, 1);
	chk_int("library stack: no frame to mark", sess.marked_frame,
	        E_DS_NOT_FOUND);

	e_ds_reset(&sess);
	take_transcript("gdb-mi-stack-eval.txt");
	chk_int("user's stack: the innermost frame", sess.marked_frame, 0);
	e_ds_set_file_probe(&sess, NULL, NULL);
}

#define WORDS_ROOM (E_DS_CONSOLE_MAX + 64)

/* A ~ record or ^error answer of @fill bytes of 'a' followed by é. */
static const char *words_line(const char *head, size_t fill)
{
	static char line[WORDS_ROOM];
	size_t k = strlen(head);

	memcpy(line, head, k);
	memset(line + k, 'a', fill);
	k += fill;
	memcpy(line + k, "\\303\\251\"", sizeof "\\303\\251\"");
	return line;
}

/* gdb's words longer than the room are cut after a whole character, and a
   broken styling sequence takes none of the words after it. */
static void test_mi_words_are_cut_at_a_character_and_kept_after_styling(void)
{
	e_ds_reset(&sess);
	if (parse_ok("an error just too long",
	             words_line("^error,msg=\"", E_DS_LINE_MAX - 2)))
		e_ds_take_mi_record(&sess, &rec);
	chk_int("error words: cut before the character that does not fit",
	        (int)strlen(sess.last_error), E_DS_LINE_MAX - 2);

	e_ds_reset(&sess);
	if (parse_ok("console words just too long",
	             words_line("~\"", E_DS_CONSOLE_MAX - 2)))
		e_ds_take_mi_record(&sess, &rec);
	chk_int("console: cut before the character", sess.console_len,
	        E_DS_CONSOLE_MAX - 2);
	chk_int("console: the whole character is counted lost",
	        sess.console_lost, 2);

	e_ds_reset(&sess);
	if (parse_ok("a broken styling sequence",
	             "^error,msg=\"a\\e[1\\303\\251b\""))
		e_ds_take_mi_record(&sess, &rec);
	chk_str("error words: what follows a broken sequence stays",
	        sess.last_error, "a\303\251b");
}

/* ---- the parser's own cases ---- */

/* Parse @line, which must be a record. */
static int parse_ok(const char *what, const char *line)
{
	int rc = e_gdbmi_parse_line(line, &rec);

	if (rc != E_GDBMI_OK) {
		printf("FAIL: %s: parse returned %d\n", what, rc);
		fails++;
	}
	return rc == E_GDBMI_OK;
}

static void test_parser_kinds_classes_and_tokens(void)
{
	if (parse_ok("result", "^done")) {
		chk_int("result: kind", rec.kind, E_GDBMI_K_RESULT);
		chk_str("result: class", rec.klass, "done");
		chk_int("result: no token", rec.token, E_GDBMI_NONE);
	}
	if (parse_ok("token", "12^done,value=\"3\"")) {
		chk_int("token: value", rec.token, 12);
		chk_str("token: get", e_gdbmi_get(&rec, "value"), "3");
	}
	if (parse_ok("token with zeros", "007*stopped"))
		chk_int("token with zeros", rec.token, 7);
	if (parse_ok("token 0", "0^running"))
		chk_int("token 0", rec.token, 0);
	if (parse_ok("the largest token", "2147483647^done"))
		chk_int("the largest token", rec.token, 2147483647);
	chk_int("a token past an int",
	        e_gdbmi_parse_line("2147483648^done", &rec), E_GDBMI_ERR_SYNTAX);
	if (parse_ok("exec", "*running,thread-id=\"all\"")) {
		chk_int("exec: kind", rec.kind, E_GDBMI_K_EXEC);
		chk_str("exec: class", rec.klass, "running");
		chk_str("exec: field", e_gdbmi_get(&rec, "thread-id"), "all");
	}
	if (parse_ok("status", "+download,section=\".text\"")) {
		chk_int("status: kind", rec.kind, E_GDBMI_K_STATUS);
		chk_str("status: field", e_gdbmi_get(&rec, "section"), ".text");
	}
	if (parse_ok("notify", "=thread-group-added,id=\"i1\"")) {
		chk_int("notify: kind", rec.kind, E_GDBMI_K_NOTIFY);
		chk_str("notify: class", rec.klass, "thread-group-added");
	}
	if (parse_ok("notify without results", "=cmd-param-changed")) {
		chk_str("notify bare: class", rec.klass, "cmd-param-changed");
		chk_int("notify bare: no field", e_gdbmi_get(&rec, "id") == NULL, 1);
	}
	if (parse_ok("console", "~\"Reading symbols from p...\\n\"")) {
		chk_int("console: kind", rec.kind, E_GDBMI_K_CONSOLE);
		chk_str("console: text", e_gdbmi_stream_text(&rec),
		        "Reading symbols from p...\n");
	}
	if (parse_ok("target", "@\"hello\\n\"")) {
		chk_int("target: kind", rec.kind, E_GDBMI_K_TARGET);
		chk_str("target: text", e_gdbmi_stream_text(&rec), "hello\n");
	}
	if (parse_ok("log", "&\"warning\\n\"")) {
		chk_int("log: kind", rec.kind, E_GDBMI_K_LOG);
		chk_str("log: text", e_gdbmi_stream_text(&rec), "warning\n");
	}
	if (parse_ok("empty console", "~\"\""))
		chk_str("empty console: text", e_gdbmi_stream_text(&rec), "");
	if (parse_ok("prompt", "(gdb)"))
		chk_int("prompt: kind", rec.kind, E_GDBMI_K_PROMPT);
	if (parse_ok("prompt with its space", "(gdb) ")) {
		chk_int("prompt space: kind", rec.kind, E_GDBMI_K_PROMPT);
		chk_int("prompt: no text", e_gdbmi_stream_text(&rec) == NULL, 1);
	}
	chk_int("a result has no stream text",
	        e_gdbmi_parse_line("^done", &rec) == 0
	        && e_gdbmi_stream_text(&rec) == NULL, 1);
}

static void test_parser_decodes_the_escapes(void)
{
	static const char want[] = "a\tb\nc\"d\\eA\xe2\x9d\x8c\x1b" "[1m\x1b"
	                           "!z\r\a\b\f\v'";
	const char *line = "~\"a\\tb\\nc\\\"d\\\\e\\101\\342\\235\\214"
	                   "\\033[1m\\e!\\000z\\r\\a\\b\\f\\v\\'\"";
	const char *text;

	if (!parse_ok("escapes", line))
		return;
	text = e_gdbmi_stream_text(&rec);
	chk_mem("escapes decoded", text, strlen(text), want, sizeof want - 1);

	if (parse_ok("short octal", "~\"\\1\\12x\"")) {
		text = e_gdbmi_stream_text(&rec);
		chk_mem("octal of one and two digits", text, strlen(text),
		        "\x01\nx", 3);
	}
	if (parse_ok("octal then digits", "~\"\\1011\"")) {
		text = e_gdbmi_stream_text(&rec);
		chk_str("octal takes three digits", text, "A1");
	}
	if (parse_ok("a result string", "^done,msg=\"No symbol \\\"x\\\" here.\"")) {
		chk_str("a result's quotes", e_gdbmi_get(&rec, "msg"),
		        "No symbol \"x\" here.");
	}
	if (parse_ok("a comma and a brace in a string",
	             "^done,a=\"x,y}]\",b=\"z\""))
		chk_str("the field after a punctuated string",
		        e_gdbmi_get(&rec, "b"), "z");
}

static void test_parser_nested_tuples_and_lists(void)
{
	const E_GDBMI_NODE *n;
	int k;

	if (parse_ok("nested", "^done,a={b={c=\"1\",d=[\"x\",\"y\"]},e=\"2\"},f=\"3\"")) {
		chk_str("a.b.c", e_gdbmi_get(&rec, "a.b.c"), "1");
		chk_str("a.e", e_gdbmi_get(&rec, "a.e"), "2");
		chk_str("f", e_gdbmi_get(&rec, "f"), "3");
		n = e_gdbmi_find(&rec, "a.b.d");
		chk_int("a list is found", n != NULL, 1);
		chk_int("a list is no string", e_gdbmi_text_of(&rec, n) == NULL, 1);
		n = e_gdbmi_first_item(&rec, n);
		chk_str("list item 0", e_gdbmi_text_of(&rec, n), "x");
		n = e_gdbmi_next_item(&rec, n);
		chk_str("list item 1", e_gdbmi_text_of(&rec, n), "y");
		chk_int("list ends", e_gdbmi_next_item(&rec, n) == NULL, 1);
		chk_str("a bare item has no name",
		        e_gdbmi_item_name(&rec, n), "");
	}
	if (parse_ok("empty tuple and list", "^done,a={},b=[],c=\"1\"")) {
		chk_int("empty tuple is found", e_gdbmi_find(&rec, "a") != NULL, 1);
		chk_int("empty tuple has no item",
		        e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "a")) == NULL, 1);
		chk_int("empty list has no item",
		        e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "b")) == NULL, 1);
		chk_str("a field after them", e_gdbmi_get(&rec, "c"), "1");
	}
	if (parse_ok("list of tuples", "^done,a=[{k=\"1\"},{k=\"2\"},{k=\"3\"}]")) {
		k = 0;
		for (n = e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "a")); n;
		     n = e_gdbmi_next_item(&rec, n)) {
			k++;
			chk_str("tuple item", e_gdbmi_get_in(&rec, n, "k"),
			        k == 1 ? "1" : k == 2 ? "2" : "3");
		}
		chk_int("three items", k, 3);
	}
	if (parse_ok("list of results", "^done,a=[x=\"1\",y=\"2\"]")) {
		n = e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "a"));
		chk_str("result item name", e_gdbmi_item_name(&rec, n), "x");
		chk_str("result item value", e_gdbmi_text_of(&rec, n), "1");
		chk_str("a path into a list of results", e_gdbmi_get(&rec, "a.y"),
		        "2");
	}
	if (parse_ok("same name twice", "^done,a=\"first\",a=\"second\""))
		chk_str("the first of two names", e_gdbmi_get(&rec, "a"), "first");

	/* The real answers. */
	if (record_of("gdb-mi-break-insert.txt", E_GDBMI_K_RESULT, "done")) {
		chk_str("bkpt.number", e_gdbmi_get(&rec, "bkpt.number"), "1");
		chk_str("bkpt.line", e_gdbmi_get(&rec, "bkpt.line"), "7");
		chk_int("bkpt.thread-groups is a list, no string",
		        e_gdbmi_text_of(&rec,
		                e_gdbmi_find(&rec, "bkpt.thread-groups")) == NULL, 1);
		chk_str("first thread group", e_gdbmi_text_of(&rec,
		        e_gdbmi_first_item(&rec,
		                e_gdbmi_find(&rec, "bkpt.thread-groups"))), "i1");
	}
	if (record_of("gdb-mi-stack-eval.txt", E_GDBMI_K_RESULT, "done")) {
		static const char *funcs[] = { "sq", "main" };
		k = 0;
		for (n = e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "stack")); n;
		     n = e_gdbmi_next_item(&rec, n)) {
			chk_str("stack item name", e_gdbmi_item_name(&rec, n), "frame");
			if (k < 2)
				chk_str("stack item function",
				        e_gdbmi_get_in(&rec, n, "func"), funcs[k]);
			k++;
		}
		chk_int("two frames", k, 2);
	}
	if (record_of("gdb-mi-stop-breakpoint.txt", E_GDBMI_K_EXEC, "stopped")) {
		chk_str("reason", e_gdbmi_get(&rec, "reason"), "breakpoint-hit");
		chk_str("frame.line", e_gdbmi_get(&rec, "frame.line"), "7");
		chk_str("frame.fullname", e_gdbmi_get(&rec, "frame.fullname"),
		        "/work/p.c");
		chk_str("the first argument's name", e_gdbmi_get_in(&rec,
		        e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "frame.args")),
		        "name"), "v");
		chk_str("the first argument's value", e_gdbmi_get_in(&rec,
		        e_gdbmi_first_item(&rec, e_gdbmi_find(&rec, "frame.args")),
		        "value"), "3");
		chk_str("a field after the frame", e_gdbmi_get(&rec, "thread-id"),
		        "1");
	}
	if (record_of("gdb-mi-stack-eval.txt", E_GDBMI_K_RESULT, "error"))
		chk_str("msg with escaped quotes", e_gdbmi_get(&rec, "msg"),
		        "No symbol \"nosuch\" in current context.");
}

static void test_get_is_null_when_absent(void)
{
	if (!parse_ok("absent", "^done,frame={line=\"7\",args=[]}"))
		return;
	chk_int("no such field", e_gdbmi_get(&rec, "nosuch") == NULL, 1);
	chk_int("no such inner field", e_gdbmi_get(&rec, "frame.nosuch") == NULL, 1);
	chk_int("a field of a string", e_gdbmi_get(&rec, "frame.line.x") == NULL, 1);
	chk_int("a tuple is no string", e_gdbmi_get(&rec, "frame") == NULL, 1);
	chk_int("empty path", e_gdbmi_get(&rec, "") == NULL, 1);
	chk_int("leading dot", e_gdbmi_get(&rec, ".frame") == NULL, 1);
	chk_int("trailing dot", e_gdbmi_get(&rec, "frame.") == NULL, 1);
	chk_int("double dot", e_gdbmi_get(&rec, "frame..line") == NULL, 1);
	chk_int("NULL path", e_gdbmi_get(&rec, NULL) == NULL, 1);
	chk_int("find in nothing", e_gdbmi_find_in(&rec, NULL, "line") == NULL, 1);
	chk_int("a name longer than any gdb writes",
	        e_gdbmi_get(&rec, "frame.aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == NULL,
	        1);
	if (parse_ok("a stream", "~\"x\""))
		chk_int("a stream has no results", e_gdbmi_get(&rec, "x") == NULL, 1);
	chk_int("numbers: decimal", e_gdbmi_number("12", 10, -5), 12);
	chk_int("numbers: octal", e_gdbmi_number("012", 8, -5), 10);
	chk_int("numbers: not a number", e_gdbmi_number("x", 10, -5), -5);
	chk_int("numbers: NULL", e_gdbmi_number(NULL, 10, -5), -5);
	chk_int("numbers: empty", e_gdbmi_number("", 10, -5), -5);
	chk_int("numbers: trailing text", e_gdbmi_number("12x", 10, -5), -5);
}

/* Every line here must be refused, with the record left of no kind. */
static void test_parser_refuses_malformed_lines(void)
{
	static const char *bad[] = {
		"", " ", "x", "^", "*", "=", "12", "12 ^done", "^ done", "^done,",
		"^done,a", "^done,a=", "^done,=\"x\"", "^done,a=\"x", "^done,a=x",
		"^done,a=\"x\"junk", "^done,a=\"x\",", "^done,a=\"x\",,b=\"y\"",
		"^done,a={", "^done,a={b=\"1\"", "^done,a={b=\"1\",}",
		"^done,a={\"x\"}", "^done,a=[", "^done,a=[\"x\"", "^done,a=[\"x\",]",
		"^done,a=[1]", "^done,a=[{b=\"1\"]", "^done,a={b=[\"1\"}",
		"^done,a=\"x\"}", "^done,a=\"x\"]", "^done,a=\"\\", "^done,a=\"x\\\"",
		"~", "~x", "~\"x", "~\"x\\", "~\"x\"y", "~\"x\" ", "5~\"x\"",
		"@", "&\"", "(gdb", "(gdb)x", "(gdb) x", "5(gdb)", "(GDB)",
		"^done,a=\"x\" ", " ^done", "99999999999999999999^done",
		"^aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		NULL
	};
	int k;

	for (k = 0; bad[k]; k++) {
		int rc = e_gdbmi_parse_line(bad[k], &rec);

		if (rc >= 0 || rec.kind != E_GDBMI_K_NONE) {
			printf("FAIL: malformed line accepted (rc %d, kind %d): \"%s\"\n",
			       rc, rec.kind, bad[k]);
			fails++;
		}
	}
	chk_int("NULL line", e_gdbmi_parse_line(NULL, &rec) < 0, 1);
	chk_int("a failed parse leaves no results",
	        e_gdbmi_get(&rec, "a") == NULL && e_gdbmi_stream_text(&rec) == NULL,
	        1);
}

/* Each prefix of a real stop line, in a block of exactly its size: the
   parser may accept or refuse it, but must never read past its end (the
   sanitizer run is what sees that). */
static void test_parser_never_reads_past_the_line(void)
{
	static char line[LINE_ROOM];
	const char *pos;
	size_t len, k;
	char *exact;
	int lines = 0;

	if (!record_of("gdb-mi-sigint.txt", E_GDBMI_K_EXEC, "stopped"))
		return;
	pos = records_of("gdb-mi-sigint.txt");
	while (next_line(&pos, line, sizeof line)) {
		if (line[0] != '*' || !strstr(line, "stopped"))
			continue;
		lines++;
		len = strlen(line);
		for (k = 0; k <= len; k++) {
			exact = malloc(k + 1);
			if (!exact)
				return;
			memcpy(exact, line, k);
			exact[k] = '\0';
			e_gdbmi_parse_line(exact, &rec);
			free(exact);
		}
	}
	chk_int("the stop line was cut at every byte", lines, 1);
	chk_int("the whole line parses", record_of("gdb-mi-sigint.txt",
	        E_GDBMI_K_EXEC, "stopped"), 1);
}

/* The limits are reported, not overflowed. */
static void test_parser_reports_what_does_not_fit(void)
{
	static char line[LINE_ROOM + OVERLONG_EXTRA];
	size_t k;
	int rc, depth;

	/* A line past the limit. */
	memset(line, 'a', sizeof line - 1);
	line[sizeof line - 1] = '\0';
	line[0] = '~';
	line[1] = '"';
	line[sizeof line - 2] = '"';
	chk_int("over-long line", e_gdbmi_parse_line(line, &rec), E_GDBMI_ERR_LONG);
	chk_int("over-long line leaves no kind", rec.kind, E_GDBMI_K_NONE);

	/* The longest line that is allowed. */
	memset(line, 'a', sizeof line - 1);
	line[E_GDBMI_LINE_MAX - 1] = '\0';
	line[0] = '~';
	line[1] = '"';
	line[E_GDBMI_LINE_MAX - 2] = '"';
	chk_int("longest line", e_gdbmi_parse_line(line, &rec), E_GDBMI_OK);
	chk_int("longest line: its text", (int)strlen(e_gdbmi_stream_text(&rec)),
	        E_GDBMI_LINE_MAX - 4);

	/* More values than a record holds. */
	strcpy(line, "^done,a=[");
	k = strlen(line);
	for (depth = 0; depth < E_GDBMI_NODES_MAX; depth++) {
		line[k++] = '"';
		line[k++] = 'x';
		line[k++] = '"';
		line[k++] = ',';
	}
	k--;
	line[k++] = ']';
	line[k] = '\0';
	rc = e_gdbmi_parse_line(line, &rec);
	chk_int("too many values", rc, E_GDBMI_ERR_TOO_BIG);

	/* Tuples nested past the limit. */
	line[0] = '\0';
	strcat(line, "^done,");
	for (depth = 0; depth < E_GDBMI_DEPTH_MAX + NESTING_EXTRA; depth++)
		strcat(line, "a={");
	strcat(line, "b=\"1\"");
	for (depth = 0; depth < E_GDBMI_DEPTH_MAX + NESTING_EXTRA; depth++)
		strcat(line, "}");
	chk_int("too deep", e_gdbmi_parse_line(line, &rec), E_GDBMI_ERR_DEPTH);

	/* The deepest that is allowed. */
	line[0] = '\0';
	strcat(line, "^done,");
	for (depth = 0; depth < E_GDBMI_DEPTH_MAX; depth++)
		strcat(line, "a={");
	strcat(line, "b=\"1\"");
	for (depth = 0; depth < E_GDBMI_DEPTH_MAX; depth++)
		strcat(line, "}");
	chk_int("deep enough", e_gdbmi_parse_line(line, &rec), E_GDBMI_OK);
}

/* A record is parsed over the one before it with nothing left of it. */
static void test_parser_drops_the_record_before(void)
{
	parse_ok("first", "^done,a=\"1\",b=\"2\"");
	parse_ok("second", "*stopped,c=\"3\"");
	chk_int("a field of the record before", e_gdbmi_get(&rec, "a") == NULL, 1);
	chk_str("a field of this one", e_gdbmi_get(&rec, "c"), "3");
	parse_ok("a stream after results", "~\"x\"");
	chk_int("results of the record before", e_gdbmi_get(&rec, "c") == NULL, 1);
	chk_str("the class is cleared", rec.klass, "");
}

/* ---- the splitter ---- */

/* All the lines @split hands out for one read of @s, joined with '|'. */
static void split_joined(const char *s, size_t n, char *out, size_t room,
                         int *overlong)
{
	size_t used = 0;
	const char *line;
	int rc;

	while ((rc = e_gdbmi_split_next(&split, s, n, &used, &line))
	       != E_GDBMI_SPLIT_NEED_MORE) {
		if (rc == E_GDBMI_SPLIT_OVERLONG && overlong)
			(*overlong)++;
		if (strlen(out) + strlen(line) + 2 < room) {
			strcat(out, line);
			strcat(out, "|");
		}
	}
}

static void test_splitter_keeps_a_partial_line(void)
{
	char out[LINE_ROOM];
	int overlong = 0;

	e_gdbmi_split_init(&split);
	out[0] = '\0';
	split_joined("^done\n(gd", 9, out, sizeof out, &overlong);
	chk_str("first read: only the whole line", out, "^done|");
	out[0] = '\0';
	split_joined("b) \n*run", 8, out, sizeof out, &overlong);
	chk_str("second read: the prompt finished", out, "(gdb) |");
	out[0] = '\0';
	split_joined("ning\n", 5, out, sizeof out, &overlong);
	chk_str("third read: the exec record finished", out, "*running|");
	chk_int("none was over-long", overlong, 0);

	/* A read that ends exactly at a line end leaves nothing. */
	out[0] = '\0';
	split_joined("(gdb)\n", 6, out, sizeof out, &overlong);
	chk_str("exact end", out, "(gdb)|");
	out[0] = '\0';
	split_joined("", 0, out, sizeof out, &overlong);
	chk_str("an empty read", out, "");

	/* Several lines in one read, a blank one, and CRLF. */
	e_gdbmi_split_init(&split);
	out[0] = '\0';
	split_joined("a\n\nb\r\nc\n", 8, out, sizeof out, &overlong);
	chk_str("many lines in one read", out, "a||b|c|");

	/* A character cut in the middle keeps its bytes. */
	e_gdbmi_split_init(&split);
	out[0] = '\0';
	split_joined("~\"\xe2\x9d", 4, out, sizeof out, &overlong);
	split_joined("\x8c\"\n", 3, out, sizeof out, &overlong);
	chk_str("a multibyte character cut in two", out, "~\"\xe2\x9d\x8c\"|");
}

static void test_splitter_reports_an_overlong_line_and_goes_on(void)
{
	static char big[LINE_ROOM + OVERLONG_EXTRA + 16];
	static char out[LINE_ROOM + 64];
	size_t k;
	int overlong = 0;

	e_gdbmi_split_init(&split);
	for (k = 0; k < LINE_ROOM + OVERLONG_EXTRA; k++)
		big[k] = 'a';
	big[k++] = '\n';
	memcpy(big + k, "^done\n", 6);
	k += 6;
	out[0] = '\0';
	split_joined(big, k, out, sizeof out, &overlong);
	chk_int("over-long: reported once", overlong, 1);
	chk_has("over-long: the next line is whole", out, "^done|");
	chk_int("over-long: the head is a line of its own", (int)strlen(out)
	        > 6, 1);

	/* The same, the line's end in a later read. */
	e_gdbmi_split_init(&split);
	overlong = 0;
	out[0] = '\0';
	split_joined(big, LINE_ROOM + OVERLONG_EXTRA - 10, out, sizeof out,
	             &overlong);
	chk_int("over-long, first read: nothing reported yet", overlong, 0);
	split_joined(big + LINE_ROOM + OVERLONG_EXTRA - 10,
	             k - (LINE_ROOM + OVERLONG_EXTRA - 10), out, sizeof out,
	             &overlong);
	chk_int("over-long, second read: reported once", overlong, 1);
	chk_has("over-long, second read: the next line is whole", out, "^done|");

	/* A line of exactly the limit is not over-long. */
	e_gdbmi_split_init(&split);
	overlong = 0;
	for (k = 0; k < E_GDBMI_LINE_MAX - 1; k++)
		big[k] = 'a';
	big[k++] = '\n';
	out[0] = '\0';
	split_joined(big, k, out, sizeof out, &overlong);
	chk_int("a line at the limit is whole", overlong, 0);
}

/* ---- one command's answer, as the editor's request engine reads it ---- */

/* The answer to 5-data-evaluate-expression with an interrupt's answer in
   front of it and program output that looks like a prompt. */
#define ANSWER_TOKEN 5
#define ANSWER_HEAD  "&\"Quit\\n\"\n(gdb) \n"
#define ANSWER_LATE  "4^done,value=\"1\"\n(gdb) \n"
#define ANSWER_OWN   "5^done,value=\"3\"\n(gdb) \n"

static void test_answer_ends_at_the_prompt_after_its_own_result(void)
{
	const char *head = ANSWER_HEAD;
	const char *late = ANSWER_HEAD ANSWER_LATE;
	const char *whole = ANSWER_HEAD ANSWER_LATE ANSWER_OWN;
	const char *no_end = ANSWER_HEAD "5^done,value=\"3\"\n";
	size_t plen = 0;

	chk_int("answer: an interrupt's prompt ends nothing",
		e_gdbmi_answer_ended(head, strlen(head), ANSWER_TOKEN, &plen), 0);
	chk_int("answer: another command's answer ends nothing",
		e_gdbmi_answer_ended(late, strlen(late), ANSWER_TOKEN, &plen), 0);
	chk_int("answer: no (gdb) after the result yet",
		e_gdbmi_answer_ended(no_end, strlen(no_end), ANSWER_TOKEN,
				     &plen), 0);
	chk_int("answer: its own result, then (gdb)",
		e_gdbmi_answer_ended(whole, strlen(whole), ANSWER_TOKEN, &plen),
		1);
	chk_int("answer: the prompt line is cut", (long)plen,
		(long)strlen("(gdb) \n"));
	chk_int("answer: a prompt without its line end at a read's edge",
		e_gdbmi_answer_ended("5^done\n(gdb) ", strlen("5^done\n(gdb) "),
				     ANSWER_TOKEN, &plen), 1);
	chk_int("answer: a console record naming (gdb) is no prompt",
		e_gdbmi_is_prompt("~\"(gdb)\"", strlen("~\"(gdb)\"")), 0);
	chk_int("answer: program text after (gdb) is no prompt",
		e_gdbmi_is_prompt("(gdb) x", strlen("(gdb) x")), 0);
}

static void test_lines_of_no_request_are_told_apart(void)
{
	chk_int("async: a stop", e_gdbmi_line_is_async(
		"*stopped,reason=\"exited\"", ANSWER_TOKEN), 1);
	chk_int("async: a notice", e_gdbmi_line_is_async(
		"=breakpoint-modified,bkpt={number=\"1\"}", ANSWER_TOKEN), 1);
	chk_int("async: a command given up on", e_gdbmi_line_is_async(
		"4^done,value=\"1\"", ANSWER_TOKEN), 1);
	chk_int("async: the command in flight", e_gdbmi_line_is_async(
		"5^done,value=\"3\"", ANSWER_TOKEN), 0);
	chk_int("async: gdb's words belong to the answer",
		e_gdbmi_line_is_async("~\"$1 = 3\\n\"", ANSWER_TOKEN), 0);
	chk_int("async: the prompt belongs to the answer",
		e_gdbmi_line_is_async("(gdb) ", ANSWER_TOKEN), 0);
	chk_int("async: a token too long to be one is not taken for one",
		e_gdbmi_line_is_async("99999999999^done", ANSWER_TOKEN), 0);
}

static void test_result_of_its_own_command_is_read(void)
{
	const char *text = ANSWER_HEAD ANSWER_LATE ANSWER_OWN;

	chk_int("result: found",
		e_gdbmi_parse_result(text, ANSWER_TOKEN, &rec), E_GDBMI_OK);
	chk_str("result: its value", e_gdbmi_get(&rec, "value"), "3");
	chk_int("result: none for an unknown command",
		e_gdbmi_parse_result(text, ANSWER_TOKEN + 1, &rec) == E_GDBMI_OK,
		0);
	chk_int("result: none is of no kind", rec.kind, E_GDBMI_K_NONE);
}

#define VERSION_ANSWER \
	"~\"GNU gdb (Debian 18.1-3) 18.1\\n\"\n" \
	"~\"Copyright (C) 2026 Free Software Foundation, Inc.\\n\"\n" \
	"1^done\n(gdb) \n"
#define BANNER_ROOM 256

static void test_version_banner_names_gdbs_own_version(void)
{
	char banner[BANNER_ROOM];
	int major = 0, minor = 0;

	e_gdbmi_console_join(VERSION_ANSWER, banner, sizeof banner);
	chk_has("version: console words joined", banner,
		"GNU gdb (Debian 18.1-3) 18.1\nCopyright");
	chk_int("version: found",
		e_gdbmi_version_of(banner, &major, &minor), 1);
	chk_int("version: major", major, 18);
	chk_int("version: minor", minor, 1);
	chk_int("version: three parts give two",
		e_gdbmi_version_of("GNU gdb (GDB) 7.12.1", &major, &minor), 1);
	chk_int("version: 7.12 major", major, 7);
	chk_int("version: 7.12 minor", minor, 12);
	chk_int("version: an old one",
		e_gdbmi_version_of("GNU gdb 6.3\nmore 99.9", &major, &minor),
		1);
	chk_int("version: only the first line counts", major, 6);
	chk_int("version: none",
		e_gdbmi_version_of("GNU gdb\n7.6", &major, &minor), 0);
}

static void test_features_list_is_searched_by_name(void)
{
	chk_int("features: parses", e_gdbmi_parse_line(
		"2^done,features=[\"frozen-varobjs\",\"pending-breakpoints\","
		"\"breakpoint-notifications\"]", &rec), E_GDBMI_OK);
	chk_int("features: pending", e_gdbmi_list_has(&rec, "features",
		"pending-breakpoints"), 1);
	chk_int("features: notices", e_gdbmi_list_has(&rec, "features",
		"breakpoint-notifications"), 1);
	chk_int("features: not a prefix", e_gdbmi_list_has(&rec, "features",
		"pending"), 0);
	chk_int("features: no such list", e_gdbmi_list_has(&rec, "nosuch",
		"pending-breakpoints"), 0);
}

#define WATCH_ROOM 64
#define ROWS_ROOM  1024
#define ARGS_LINE \
	"10^done,stack-args=[frame={level=\"0\",args=[{name=\"v\"," \
	"value=\"3\"}]},frame={level=\"1\",args=[]}]"

static void test_watch_and_stack_read_as_under_the_line_interface(void)
{
	static E_GDBMI_RECORD args;
	char text[WATCH_ROOM], rows[ROWS_ROOM];

	chk_int("watch: value", e_gdbmi_parse_line("8^done,value=\"3\"", &rec),
		E_GDBMI_OK);
	chk_int("watch: has one", e_ds_mi_watch_text(&rec, text, sizeof text),
		1);
	chk_str("watch: as the line interface gives it", text, "= 3");
	chk_int("watch: error", e_gdbmi_parse_line(
		"9^error,msg=\"No symbol \\\"nosuch\\\" in current context.\"",
		&rec), E_GDBMI_OK);
	chk_int("watch: none for an error",
		e_ds_mi_watch_text(&rec, text, sizeof text), 0);

	chk_int("stack: args parse", e_gdbmi_parse_line(ARGS_LINE, &args),
		E_GDBMI_OK);
	if (!record_of("gdb-mi-stack-eval.txt", E_GDBMI_K_RESULT, "done"))
		return;
	chk_int("stack: two rows", e_ds_mi_where_rows(&rec, &args, rows,
		sizeof rows), 2);
	chk_str("stack: rows as bt prints them", rows,
		"#0  sq (v=3) at p.c:7\n"
		"#1  0x000055555555516e in main () at p.c:14\n");
	chk_int("stack: only whole rows fit",
		e_ds_mi_where_rows(&rec, &args, rows, 30), 1);
	chk_str("stack: the first row alone", rows, "#0  sq (v=3) at p.c:7\n");
	chk_int("stack: a frame without source parses", e_gdbmi_parse_line(
		"10^done,stack=[frame={level=\"0\",addr=\"0x00007ffff7e29a02\","
		"func=\"clock_nanosleep\",from=\"/lib/libc.so.6\"}]", &rec),
		E_GDBMI_OK);
	chk_int("stack: one row without source",
		e_ds_mi_where_rows(&rec, NULL, rows, sizeof rows), 1);
	chk_str("stack: a frame without source names its library", rows,
		"#0  0x00007ffff7e29a02 in clock_nanosleep () from /lib/libc.so.6\n");
}

/* A result record with no number answers no command the editor sent. */
static void test_a_result_without_a_number_ends_no_answer(void)
{
	const char *text = "^done\n(gdb) \n";
	size_t plen = 0;

	chk_int("no number: belongs to no request",
		e_gdbmi_line_is_async("^done", ANSWER_TOKEN), 1);
	chk_int("no number: ends no answer",
		e_gdbmi_answer_ended(text, strlen(text), ANSWER_TOKEN, &plen), 0);
	chk_int("no number: is no command's result",
		e_gdbmi_parse_result(text, ANSWER_TOKEN, &rec) == E_GDBMI_OK, 0);
	chk_int("the highest number: read whole",
		e_gdbmi_line_is_async("999999999^done", 999999999), 0);
	chk_int("the highest number: ends its answer",
		e_gdbmi_answer_ended("999999999^done\n(gdb) \n",
				     strlen("999999999^done\n(gdb) \n"),
				     999999999, &plen), 1);
}

int main(void)
{
	test_answer_ends_at_the_prompt_after_its_own_result();
	test_lines_of_no_request_are_told_apart();
	test_result_of_its_own_command_is_read();
	test_version_banner_names_gdbs_own_version();
	test_features_list_is_searched_by_name();
	test_watch_and_stack_read_as_under_the_line_interface();
	test_a_result_without_a_number_ends_no_answer();
	test_mi_stop_record_names_file_and_line();
	test_mi_stop_record_split_across_two_reads();
	test_mi_break_insert_answer_verifies_the_entry();
	test_mi_break_insert_error_refuses_with_its_message();
	test_mi_sigint_stop_during_a_sync_interrupt_is_not_the_users();
	test_mi_exited_record_gives_the_exit_code();
	test_mi_console_records_are_kept_for_messages();
	test_mi_stack_mark_needs_a_file_that_is_there();
	test_mi_words_are_cut_at_a_character_and_kept_after_styling();
	test_parser_kinds_classes_and_tokens();
	test_parser_decodes_the_escapes();
	test_parser_nested_tuples_and_lists();
	test_get_is_null_when_absent();
	test_parser_refuses_malformed_lines();
	test_parser_never_reads_past_the_line();
	test_parser_reports_what_does_not_fit();
	test_parser_drops_the_record_before();
	test_splitter_keeps_a_partial_line();
	test_splitter_reports_an_overlong_line_and_goes_on();
	if (fails) {
		printf("test_gdbmi: %d check(s) failed\n", fails);
		return 1;
	}
	printf("test_gdbmi: all checks passed\n");
	return 0;
}
