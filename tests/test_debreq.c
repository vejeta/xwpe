/* test_debreq.c -- unit test for the request engine.  Links we_debreq.c
 * only; no editor, no curses, no debugger, always runs (no skip).
 *
 * The world is a fake: a clock that moves only when the test says so, timers
 * that are recorded and fired in due order, and a transcript of what was
 * sent.  No real time passes and nothing waits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../we_debreq.h"

#define SENT_MAX      32
#define LOG_MAX       16
#define TEXT_ROOM     512
#define PROMPT        "(gdb) "
#define ASYNC_STOP    "*stopped"
#define SETTLE_CMD    "echo SETTLED"
#define SETTLE_MARKER "SETTLED"
#define NO_TIMER      (-1L)
#define MS_PER_S      1000L
#define CHAIN_WATCHES 3

static int fails;

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

static void chk_has(const char *what, const char *got, const char *part)
{
	if (!got || !strstr(got, part)) {
		printf("FAIL: %s: \"%s\" lacks \"%s\"\n", what,
		       got ? got : "(null)", part);
		fails++;
	}
}

/* ---- the fake world ---- */

typedef struct
{
	long now;
	long armed[E_D_TIMERS];               /* due time, or NO_TIMER */
	char sent[SENT_MAX][E_D_REQ_CMD_MAX];
	int nsent;
	long wait_secs[LOG_MAX];
	char wait_cmd[LOG_MAX][E_D_REQ_CMD_MAX];
	int nwait;
	char notice[LOG_MAX][TEXT_ROOM];
	int nnotice;
	char async[LOG_MAX][TEXT_ROOM];
	int nasync;
	char errors[TEXT_ROOM];               /* written on the error stream, unread */
	size_t nerrors;
} FAKE;

/* The error stream: what the debugger wrote there and nobody read yet. */
static size_t fake_errors_waiting(char *buf, size_t n, void *io)
{
	FAKE *f = io;
	size_t take = f->nerrors < n ? f->nerrors : n;

	memcpy(buf, f->errors, take);
	memmove(f->errors, f->errors + take, f->nerrors - take);
	f->nerrors -= take;
	return take;
}

/* The debugger writes @s on its error stream; the editor has not read it. */
static void fake_write_error(FAKE *f, const char *s)
{
	size_t n = strlen(s);

	if (f->nerrors + n > sizeof f->errors)
		n = sizeof f->errors - f->nerrors;
	memcpy(f->errors + f->nerrors, s, n);
	f->nerrors += n;
}

static int fake_send(const char *cmd, void *io)
{
	FAKE *f = io;

	if (f->nsent < SENT_MAX)
		snprintf(f->sent[f->nsent], sizeof f->sent[0], "%s", cmd);
	f->nsent++;
	return 0;
}

static int fake_answer_ended(const E_D_ANSWER *a, size_t *prompt_len, void *io)
{
	size_t n = strlen(PROMPT);

	(void)io;
	*prompt_len = n;
	return a->text && a->len >= n && strcmp(a->text + a->len - n, PROMPT) == 0;
}

static int fake_line_is_async(const char *line, void *io)
{
	(void)io;
	return strncmp(line, ASYNC_STOP, strlen(ASYNC_STOP)) == 0;
}

static void fake_deliver_async(const char *line, void *io)
{
	FAKE *f = io;

	if (f->nasync < LOG_MAX)
		snprintf(f->async[f->nasync], sizeof f->async[0], "%s", line);
	f->nasync++;
}

static long fake_now(void *io)
{
	return ((FAKE *)io)->now;
}

static void fake_arm(int which, long ms, void *io)
{
	FAKE *f = io;

	f->armed[which] = ms < 0 ? NO_TIMER : f->now + ms;
}

static void fake_waiting(const char *cmd, long secs, void *io)
{
	FAKE *f = io;

	if (f->nwait < LOG_MAX) {
		snprintf(f->wait_cmd[f->nwait], sizeof f->wait_cmd[0], "%s", cmd);
		f->wait_secs[f->nwait] = secs;
	}
	f->nwait++;
}

static void fake_notice(const char *msg, void *io)
{
	FAKE *f = io;

	if (f->nnotice < LOG_MAX)
		snprintf(f->notice[f->nnotice], sizeof f->notice[0], "%s", msg);
	f->nnotice++;
}

/* The other prompts the fake debugger can show: pdb's, then gdb's block
   prompt and MI's console-stream form of it. */
enum { OTHER_COM, OTHER_DEBUG, OTHER_INTERACT, OTHER_CONT1, OTHER_CONT3,
       OTHER_GT, OTHER_MI_GT };

static const E_D_OTHER_PROMPT other_rows[] = {
	{ "(com)",   "(com) ",   E_D_PROMPT_ENDS_WITH },
	{ "((Pdb))", "((Pdb)) ", E_D_PROMPT_ENDS_WITH },
	{ ">>>",     ">>> ",     E_D_PROMPT_ENDS_WITH },
	{ "...",     "... ",     E_D_PROMPT_ENDS_WITH },
	{ "...",     "...   ",   E_D_PROMPT_ENDS_WITH },
	{ ">",       ">",        E_D_PROMPT_LAST_LINE },
	{ ">",       "~\">\"",   E_D_PROMPT_LAST_LINE },
	{ NULL, NULL, 0 }
};

static const E_D_OTHER_PROMPT *fake_other_prompt(const E_D_ANSWER *a, size_t *len,
                                                 void *io)
{
	(void)io;
	return e_d_req_other_prompt_at(a, other_rows, len);
}

static void world_init(FAKE *f, E_D_REQ_IO *io, int with_settle)
{
	int i;

	memset(f, 0, sizeof *f);
	for (i = 0; i < E_D_TIMERS; i++)
		f->armed[i] = NO_TIMER;
	memset(io, 0, sizeof *io);
	io->send = fake_send;
	io->answer_ended = fake_answer_ended;
	io->line_is_async = fake_line_is_async;
	io->deliver_async = fake_deliver_async;
	io->now_ms = fake_now;
	io->arm_timer = fake_arm;
	io->waiting = fake_waiting;
	io->notice = fake_notice;
	io->other_prompt = fake_other_prompt;
	io->name = "gdb";
	if (with_settle) {
		io->settle_cmd = SETTLE_CMD;
		io->settle_marker = SETTLE_MARKER;
	}
	io->io = f;
}

/* The earliest armed timer at or before @until, or -1. */
static int earliest_due(const FAKE *f, long until)
{
	int i, best = -1;

	for (i = 0; i < E_D_TIMERS; i++)
		if (f->armed[i] != NO_TIMER && f->armed[i] <= until &&
		    (best < 0 || f->armed[i] < f->armed[best]))
			best = i;
	return best;
}

/* Let @ms pass: each timer due on the way fires, in due order, with the
   clock at its due time. */
static void advance(FAKE *f, E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, long ms)
{
	long until = f->now + ms;
	int which;

	while ((which = earliest_due(f, until)) >= 0) {
		f->now = f->armed[which];
		f->armed[which] = NO_TIMER;
		e_d_req_timer(rq, io, which);
	}
	f->now = until;
}

static void feed(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, const char *s)
{
	e_d_req_feed(rq, io, s, strlen(s));
}

/* ---- what a request's owner sees ---- */

typedef struct
{
	int id;
	int calls;
	E_D_REQ_END how;
	char text[TEXT_ROOM];
	char err[TEXT_ROOM];
	char prompt[E_D_REQ_NAME_MAX];
	int error;
	int verdict;                         /* what the owner says to an other prompt */
} CALL;

static int call_order[LOG_MAX];
static int ncall_order;

static int on_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
	CALL *c = ctx;

	c->calls++;
	c->how = how;
	c->error = ans ? ans->error : 0;
	snprintf(c->text, sizeof c->text, "%s", ans && ans->text ? ans->text : "");
	snprintf(c->err, sizeof c->err, "%s", ans && ans->err ? ans->err : "");
	snprintf(c->prompt, sizeof c->prompt, "%s", ans && ans->prompt ? ans->prompt : "");
	if (ncall_order < LOG_MAX)
		call_order[ncall_order++] = c->id;
	if (how == E_D_REQ_OTHER_PROMPT)
		return c->verdict;
	return E_D_STEP_NEXT;
}

static void call_init(CALL *c, int id)
{
	memset(c, 0, sizeof *c);
	c->id = id;
	ncall_order = 0;
}

/* ---- test 11 ---- */

static void test_request_deadline_fires_without_an_answer(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c;
	char msg[TEXT_ROOM];

	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	chk_int("deadline: default", e_d_req_timeout_from_text(NULL), E_D_REQ_TIMEOUT_S);
	chk_int("deadline: sent", e_d_req_send(&rq, &io, "p x", on_answered, &c),
	        E_D_SEND_OK);
	chk_str("deadline: command went out", f.sent[0], "p x");
	chk_int("deadline: a request is pending", e_d_req_pending(&rq), 1);
	chk_int("deadline: quiet timer armed", f.armed[E_D_TIMER_QUIET], E_D_REQ_QUIET_MS);
	chk_int("deadline: deadline timer armed", f.armed[E_D_TIMER_DEADLINE],
	        (long)E_D_REQ_TIMEOUT_S * MS_PER_S);

	advance(&f, &rq, &io, E_D_REQ_QUIET_MS - 1);
	chk_int("deadline: no waiting line before the quiet delay", f.nwait, 0);
	advance(&f, &rq, &io, 1);
	chk_int("deadline: waiting line after the quiet delay", f.nwait, 1);
	chk_str("deadline: waiting names the command", f.wait_cmd[0], "p x");
	chk_int("deadline: waiting says 1 s", f.wait_secs[0], 1);
	advance(&f, &rq, &io, E_D_REQ_SECOND_MS);
	chk_int("deadline: the line counts on", f.nwait, 2);
	chk_int("deadline: waiting says 2 s", f.wait_secs[1], 2);

	advance(&f, &rq, &io, (long)E_D_REQ_TIMEOUT_S * MS_PER_S);
	chk_int("deadline: request called once", c.calls, 1);
	chk_int("deadline: request ended timed out", c.how, E_D_REQ_TIMED_OUT);
	chk_int("deadline: notice shown", f.nnotice, 1);
	chk_has("deadline: notice names the command", f.notice[0], "p x");
	chk_has("deadline: notice says how long", f.notice[0], "30 s");
	chk_has("deadline: notice says how to leave", f.notice[0], "Quit");
	chk_int("deadline: no timer left", earliest_due(&f, f.now + 1000000L), -1);

	/* the owed answer makes the next command wait for the settle */
	e_d_req_busy_message(&rq, &io, msg, sizeof msg);
	chk_has("deadline: busy message names the owed command", msg, "p x");

	/* the environment's override */
	chk_int("deadline: override 5", e_d_req_timeout_from_text("5"), 5);
	chk_int("deadline: override junk", e_d_req_timeout_from_text("abc"),
	        E_D_REQ_TIMEOUT_S);
	chk_int("deadline: override zero", e_d_req_timeout_from_text("0"),
	        E_D_REQ_TIMEOUT_S);
	chk_int("deadline: override negative", e_d_req_timeout_from_text("-3"),
	        E_D_REQ_TIMEOUT_S);
	chk_int("deadline: override too large",
	        e_d_req_timeout_from_text("999999999"), E_D_REQ_TIMEOUT_MAX_S);
	e_d_req_free(&rq);

	/* an answer in time disarms every timer and the deadline never fires */
	world_init(&f, &io, 1);
	e_d_req_init(&rq, "5");
	call_init(&c, 1);
	e_d_req_send(&rq, &io, "p y", on_answered, &c);
	chk_int("deadline: override armed", f.armed[E_D_TIMER_DEADLINE], 5 * MS_PER_S);
	advance(&f, &rq, &io, 2 * MS_PER_S);
	feed(&rq, &io, "$1 = 3\n" PROMPT);
	chk_int("deadline: answered once", c.calls, 1);
	chk_int("deadline: answered done", c.how, E_D_REQ_DONE);
	chk_str("deadline: answer text", c.text, "$1 = 3\n");
	chk_int("deadline: timers disarmed", earliest_due(&f, f.now + 1000000L), -1);
	advance(&f, &rq, &io, 60 * MS_PER_S);
	chk_int("deadline: still answered once", c.calls, 1);
	chk_int("deadline: no notice", f.nnotice, 0);
	chk_int("deadline: nothing pending", e_d_req_pending(&rq), 0);
	e_d_req_free(&rq);
}

/* ---- test 12 ---- */

static void test_requests_go_out_one_at_a_time_and_the_queue_refuses_past_its_bound(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c[E_D_REQ_QUEUE_MAX + 1];
	static char cmd[E_D_REQ_CMD_MAX];
	int i, rc;

	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	ncall_order = 0;
	for (i = 0; i < E_D_REQ_QUEUE_MAX; i++) {
		call_init(&c[i], i);
		snprintf(cmd, sizeof cmd, "p v%d", i);
		rc = e_d_req_send(&rq, &io, cmd, on_answered, &c[i]);
		chk_int("queue: accepted", rc, E_D_SEND_OK);
	}
	ncall_order = 0;
	chk_int("queue: only the first went out", f.nsent, 1);
	chk_str("queue: it is the first", f.sent[0], "p v0");

	call_init(&c[E_D_REQ_QUEUE_MAX], E_D_REQ_QUEUE_MAX);
	rc = e_d_req_send(&rq, &io, "p over", on_answered, &c[E_D_REQ_QUEUE_MAX]);
	chk_int("queue: past the bound is refused", rc, E_D_SEND_FULL);
	chk_int("queue: refused request not sent", f.nsent, 1);
	ncall_order = 0;

	for (i = 0; i < E_D_REQ_QUEUE_MAX; i++) {
		chk_int("queue: sent so far", f.nsent, i + 1);
		feed(&rq, &io, "$1 = 0\n" PROMPT);
		chk_int("queue: answered", c[i].calls, 1);
		chk_int("queue: answered done", c[i].how, E_D_REQ_DONE);
	}
	chk_int("queue: every command went out", f.nsent, E_D_REQ_QUEUE_MAX);
	for (i = 0; i < E_D_REQ_QUEUE_MAX; i++) {
		snprintf(cmd, sizeof cmd, "p v%d", i);
		chk_str("queue: order", f.sent[i], cmd);
		chk_int("queue: callback order", call_order[i], i);
	}
	chk_int("queue: empty again", e_d_req_pending(&rq), 0);
	call_init(&c[0], 0);
	chk_int("queue: room again", e_d_req_send(&rq, &io, "p again", on_answered, &c[0]),
	        E_D_SEND_OK);
	chk_int("queue: a command over the old 255 is taken for gdb", e_d_req_send(&rq, &io,
	        "p aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	        on_answered, &c[1]), E_D_SEND_OK);
	e_d_req_free(&rq);
}

/* ---- how long a command may be ---- */

#define HEAD_PDB_CHDIR   "!__import__('os').chdir(b'"
#define TAIL_PDB_CHDIR   "')"
#define ESCAPED_BYTE     "\\x41"       /* one byte of a path as pdb's literal writes it */
#define HEAD_WORDS_MAX   28            /* the words around a path in the move command */
#define UNMEASURED_BYTES (E_D_REQ_CMD_UNMEASURED_MAX - 1)   /* the longest unmeasured command */

/* @n bytes of 'a' after the word @head. */
static void make_command(char *out, size_t sz, const char *head, size_t n)
{
	size_t at = strlen(head);

	snprintf(out, sz, "%s", head);
	memset(out + at, 'a', n - at);
	out[n] = '\0';
}

/* The move command of pdb for a path of @bytes bytes, every one written as
   \xNN, the widest way any command writes a path byte. */
static void make_widest_move(char *out, size_t sz, size_t bytes)
{
	size_t i;

	snprintf(out, sz, "%s", HEAD_PDB_CHDIR);
	for (i = 0; i < bytes; i++)
		snprintf(out + strlen(out), sz - strlen(out), "%s", ESCAPED_BYTE);
	snprintf(out + strlen(out), sz - strlen(out), "%s", TAIL_PDB_CHDIR);
}

/* One send of @cmd to a debugger named @name; the engine is left empty. */
static int send_to(const char *name, const char *cmd)
{
	static FAKE f;
	static E_D_REQ_QUEUE rq;
	E_D_REQ_IO io;
	CALL c;
	int rc;

	world_init(&f, &io, 0);
	io.name = name;
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	rc = e_d_req_send(&rq, &io, cmd, on_answered, &c);
	if (rc == E_D_SEND_OK) {
		chk_int("send_to: the command went out whole", (long)strlen(f.sent[0]),
		        (long)strlen(cmd));
		chk_int("send_to: it went out once", f.nsent, 1);
	}
	e_d_req_free(&rq);
	return rc;
}

/* @scenario SC-debug-125 */
static void test_the_length_of_a_command_is_the_owners(void)
{
	static char cmd[E_D_REQ_CMD_MAX + 16];
	size_t widest = E_D_REQ_PATH_MAX - 1;

	/* the derivation: a path at the system's limit, four bytes per byte */
	chk_int("length: a command holds a path in its widest form",
	        E_D_REQ_CMD_MAX >= E_D_REQ_PATH_MAX * E_D_REQ_PATH_ESCAPE_MAX + 64, 1);

	/* a path of the longest length, in its widest form, with its words */
	make_widest_move(cmd, sizeof cmd, widest);
	chk_int("length: the widest move is under the owner's number",
	        (long)strlen(cmd) < E_D_REQ_CMD_MAX, 1);
	chk_int("length: it is far over the old 255", (long)strlen(cmd) > 16000, 1);
	chk_int("length: and it is the widest the words may make", (long)strlen(cmd) <=
	        (long)(widest * E_D_REQ_PATH_ESCAPE_MAX + HEAD_WORDS_MAX), 1);
	chk_int("length: gdb takes it", send_to("gdb", cmd), E_D_SEND_OK);
	chk_int("length: pdb takes it", send_to("pdb", cmd), E_D_SEND_OK);
	chk_int("length: jdb takes it", send_to("jdb", cmd), E_D_SEND_OK);
	chk_int("length: lldb takes it", send_to("lldb", cmd), E_D_SEND_OK);

	/* the number itself: one byte under is sent, the number is not */
	make_command(cmd, sizeof cmd, "p ", E_D_REQ_CMD_MAX - 1);
	chk_int("length: E_D_REQ_CMD_MAX - 1 bytes are sent", send_to("gdb", cmd), E_D_SEND_OK);
	make_command(cmd, sizeof cmd, "p ", E_D_REQ_CMD_MAX);
	chk_int("length: E_D_REQ_CMD_MAX bytes are refused", send_to("gdb", cmd), E_D_SEND_TOO_LONG);
	chk_int("length: also for pdb", send_to("pdb", cmd), E_D_SEND_TOO_LONG);
	make_command(cmd, sizeof cmd, "p ", E_D_REQ_CMD_MAX + 10);
	chk_int("length: and more", send_to("gdb", cmd), E_D_SEND_TOO_LONG);

	/* the debuggers nobody measured, and an engine that is none, keep 255 */
	make_command(cmd, sizeof cmd, "p ", UNMEASURED_BYTES);
	chk_int("length: sdb takes 255", send_to("sdb", cmd), E_D_SEND_OK);
	chk_int("length: xdb takes 255", send_to("xdb", cmd), E_D_SEND_OK);
	chk_int("length: dbx takes 255", send_to("dbx", cmd), E_D_SEND_OK);
	chk_int("length: a68g takes 255", send_to("a68g", cmd), E_D_SEND_OK);
	chk_int("length: an engine with no name takes 255", send_to(NULL, cmd), E_D_SEND_OK);
	make_command(cmd, sizeof cmd, "p ", UNMEASURED_BYTES + 1);
	chk_int("length: sdb refuses 256", send_to("sdb", cmd), E_D_SEND_TOO_LONG);
	chk_int("length: xdb refuses 256", send_to("xdb", cmd), E_D_SEND_TOO_LONG);
	chk_int("length: dbx refuses 256", send_to("dbx", cmd), E_D_SEND_TOO_LONG);
	chk_int("length: a68g refuses 256", send_to("a68g", cmd), E_D_SEND_TOO_LONG);
	chk_int("length: the build server's engine refuses 256", send_to("the build server", cmd),
	        E_D_SEND_TOO_LONG);
	chk_int("length: an engine with no name refuses 256", send_to(NULL, cmd), E_D_SEND_TOO_LONG);
	chk_int("length: but gdb takes 256", send_to("gdb", cmd), E_D_SEND_OK);

	/* the predicate is the same rule */
	chk_int("fits: gdb, 256 bytes", e_d_req_cmd_fits("gdb", cmd), 1);
	chk_int("fits: dbx, 256 bytes", e_d_req_cmd_fits("dbx", cmd), 0);
}

/* ---- a long command through the whole of a request ---- */

static void test_a_long_command_runs_a_whole_request(void)
{
	static FAKE f;
	static E_D_REQ_QUEUE rq;
	static char cmd[E_D_REQ_CMD_MAX];
	E_D_REQ_IO io;
	CALL c, d;
	char msg[TEXT_ROOM];

	make_widest_move(cmd, sizeof cmd, E_D_REQ_PATH_MAX - 1);
	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	call_init(&d, 2);
	chk_int("long: sent", e_d_req_send(&rq, &io, cmd, on_answered, &c), E_D_SEND_OK);
	chk_int("long: queued behind it, whole", e_d_req_send(&rq, &io, cmd, on_answered, &d), E_D_SEND_OK);
	chk_int("long: only the first is out", f.nsent, 1);
	chk_int("long: the first went out whole", (long)strlen(f.sent[0]), (long)strlen(cmd));
	feed(&rq, &io, "ok\n" PROMPT);
	chk_int("long: answered", c.how, E_D_REQ_DONE);
	chk_int("long: the second went out whole", (long)strlen(f.sent[1]), (long)strlen(cmd));

	/* given up on while it is in flight: the owed command is kept whole,
	   so the resync and the sentence name it */
	e_d_req_cancel(&rq, &io, E_D_REQ_CANCELLED);
	chk_int("long: the cancelled request is called", d.how, E_D_REQ_CANCELLED);
	chk_int("long: one notice", f.nnotice, 1);
	chk_has("long: the notice names the command's start", f.notice[0], HEAD_PDB_CHDIR);
	e_d_req_busy_message(&rq, &io, msg, sizeof msg);
	chk_has("long: the busy sentence names it too", msg, HEAD_PDB_CHDIR);
	chk_int("long: the settle command went out after it", f.nsent, 3);
	chk_str("long: it is the settle command", f.sent[2], SETTLE_CMD);
	feed(&rq, &io, SETTLE_MARKER "\n" PROMPT);
	chk_int("long: settled", e_d_req_pending(&rq), 0);
	e_d_req_free(&rq);
}

/* ---- test 13 ---- */

static void test_request_cancel_empties_the_queue_and_names_the_command(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c[3];
	CALL late;
	char msg[TEXT_ROOM];
	int i;

	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	ncall_order = 0;
	for (i = 0; i < 3; i++) {
		call_init(&c[i], i);
		e_d_req_send(&rq, &io, i == 0 ? "p x" : (i == 1 ? "p y" : "p z"),
		             on_answered, &c[i]);
	}
	ncall_order = 0;
	feed(&rq, &io, "partial\n");
	e_d_req_cancel(&rq, &io, E_D_REQ_CANCELLED);
	for (i = 0; i < 3; i++) {
		chk_int("cancel: each request called once", c[i].calls, 1);
		chk_int("cancel: each ended cancelled", c[i].how, E_D_REQ_CANCELLED);
		chk_int("cancel: callback order", call_order[i], i);
	}
	chk_str("cancel: the one in flight keeps what came", c[0].text, "partial\n");
	chk_int("cancel: only the first command ever went out", f.nsent, 2);
	chk_str("cancel: then the settle command", f.sent[1], SETTLE_CMD);
	chk_int("cancel: notice shown", f.nnotice, 1);
	chk_has("cancel: notice names the command", f.notice[0], "p x");
	chk_int("cancel: timers disarmed", earliest_due(&f, f.now + 1000000L), -1);

	chk_int("cancel: engine still owns the output", e_d_req_pending(&rq), 1);
	call_init(&late, 9);
	chk_int("cancel: a command waits for the settle",
	        e_d_req_send(&rq, &io, "p w", on_answered, &late), E_D_SEND_RESYNC);
	chk_int("cancel: refused request not called", late.calls, 0);
	e_d_req_busy_message(&rq, &io, msg, sizeof msg);
	chk_has("cancel: busy message names the owed command", msg, "p x");
	chk_has("cancel: busy message names the debugger", msg, "gdb");
	chk_has("cancel: busy message says how to leave", msg, "Quit");

	feed(&rq, &io, "$1 = 5\n" PROMPT);
	chk_int("cancel: old answer is dropped, still waiting", e_d_req_pending(&rq), 1);
	feed(&rq, &io, "SETTLED\n" PROMPT);
	chk_int("cancel: settled", e_d_req_pending(&rq), 0);
	chk_int("cancel: no request got the old answer", c[1].calls, 1);
	chk_int("cancel: a command goes out again",
	        e_d_req_send(&rq, &io, "p w", on_answered, &late), E_D_SEND_OK);
	chk_str("cancel: and it is that command", f.sent[2], "p w");
	feed(&rq, &io, "$2 = 7\n" PROMPT);
	chk_str("cancel: it gets its own answer", late.text, "$2 = 7\n");
	e_d_req_free(&rq);

	/* the session ending: every request is told, nothing is sent after */
	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	call_init(&c[0], 0);
	call_init(&c[1], 1);
	e_d_req_send(&rq, &io, "p x", on_answered, &c[0]);
	e_d_req_send(&rq, &io, "p y", on_answered, &c[1]);
	e_d_req_cancel(&rq, &io, E_D_REQ_GONE);
	chk_int("gone: first told", c[0].how, E_D_REQ_GONE);
	chk_int("gone: second told", c[1].how, E_D_REQ_GONE);
	chk_int("gone: no settle command", f.nsent, 1);
	chk_int("gone: nothing pending", e_d_req_pending(&rq), 0);
	chk_int("gone: send refused", e_d_req_send(&rq, &io, "p z", on_answered, &late),
	        E_D_SEND_GONE);
	e_d_req_free(&rq);

	/* a debugger that matches by token owes nothing: no settle */
	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	call_init(&c[0], 0);
	e_d_req_send(&rq, &io, "p x", on_answered, &c[0]);
	e_d_req_cancel(&rq, &io, E_D_REQ_CANCELLED);
	chk_int("no settle: nothing pending", e_d_req_pending(&rq), 0);
	chk_int("no settle: no extra command", f.nsent, 1);
	e_d_req_free(&rq);
}

/* ---- test 14 ---- */

static int late_calls;
static char late_text[TEXT_ROOM];

static void on_late_error(const E_D_ANSWER *err, void *ctx)
{
	(void)ctx;
	late_calls++;
	snprintf(late_text, sizeof late_text, "%s", err->text ? err->text : "");
}

static void test_async_line_during_a_request_goes_to_the_session(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c;

	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	chk_int("async: idle leaves the bytes",
	        (long)e_d_req_feed(&rq, &io, "x\n", 2), 0);
	chk_int("async: idle delivers nothing", f.nasync, 0);

	e_d_req_send(&rq, &io, "p x", on_answered, &c);
	chk_int("async: the engine takes the bytes",
	        (long)e_d_req_feed(&rq, &io, "$1 = 1\n", 7), 7);
	feed(&rq, &io, ASYNC_STOP ",reason=\"x\"\n");
	chk_int("async: line delivered", f.nasync, 1);
	chk_str("async: line without its end", f.async[0], ASYNC_STOP ",reason=\"x\"");
	chk_int("async: the request is not ended by it", c.calls, 0);
	feed(&rq, &io, "$2 = 2\n" PROMPT);
	chk_int("async: request answered", c.calls, 1);
	chk_str("async: answer has no async line", c.text, "$1 = 1\n$2 = 2\n");
	chk_int("async: delivered once", f.nasync, 1);

	/* a line split across two reads is claimed whole, and a prompt without
	   its line end still ends the answer */
	call_init(&c, 2);
	e_d_req_send(&rq, &io, "p y", on_answered, &c);
	feed(&rq, &io, "$3 = 3\n*stop");
	chk_int("async: half a line is kept back", f.nasync, 1);
	feed(&rq, &io, "ped,reason=\"y\"\n(gd");
	chk_int("async: whole line delivered", f.nasync, 2);
	chk_str("async: the line", f.async[1], ASYNC_STOP ",reason=\"y\"");
	chk_int("async: half a prompt does not end it", c.calls, 0);
	feed(&rq, &io, "b) ");
	chk_int("async: prompt ends it", c.calls, 1);
	chk_str("async: answer", c.text, "$3 = 3\n");

	/* the error stream joins the request in flight */
	call_init(&c, 3);
	e_d_req_send(&rq, &io, "break 99", on_answered, &c);
	e_d_req_feed_errors(&rq, &io, "No line 99.\n", 12);
	feed(&rq, &io, PROMPT);
	chk_int("errors: answered", c.calls, 1);
	chk_int("errors: error flag set", c.error, 1);
	chk_has("errors: words in the answer", c.text, "No line 99.");

	/* an error line after the prompt goes to the hook of the request */
	call_init(&c, 4);
	late_calls = 0;
	late_text[0] = 0;
	e_d_req_send(&rq, &io, "break 98", on_answered, &c);
	e_d_req_set_late_error(&rq, on_late_error, NULL);
	feed(&rq, &io, "Breakpoint 2 at 0x1\n" PROMPT);
	chk_int("late: answered", c.calls, 1);
	chk_int("late: not yet", late_calls, 0);
	e_d_req_feed_errors(&rq, &io, "warning: late\n", 14);
	chk_int("late: hook called", late_calls, 1);
	chk_has("late: hook has the words", late_text, "warning: late");
	e_d_req_free(&rq);
}

/* ---- chains ---- */

typedef struct
{
	int watches_sent;
	int watches_answered;
	int finished_calls;
	int finished_ok;
	int never_ran;
	E_D_STEP_END last;                    /* the record the chain ended with */
	char last_cmd[E_D_REQ_CMD_MAX + 1];   /* its command, kept past the chain */
	int said;
	int failed_calls;                     /* how often a step's failed was told */
	int failed_verdict;                   /* what it says: E_D_STEP_* */
	int refused_marks;                    /* steps whose failed took the refusal */
	E_D_STEP_END seen;                    /* the first record a failed was told */
	char seen_cmd[E_D_REQ_CMD_MAX + 1];
	char seen_words[E_D_REQ_MSG_MAX];
	char seen_reason[E_D_REQ_REASON_MAX];
	unsigned long frame_low, frame_high;  /* where the command builders ran */
	int refusals;                         /* refused steps left to build */
} CHAIN;

static int step_cmd_flush(char *cmd, size_t n, void *ctx)
{
	(void)ctx;
	snprintf(cmd, n, "flush");
	return 1;
}

static int step_answered_next(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
	(void)how; (void)ans; (void)ctx;
	return E_D_STEP_NEXT;
}

static int step_cmd_skipped(char *cmd, size_t n, void *ctx)
{
	(void)cmd; (void)n; (void)ctx;
	return 0;
}

static int step_cmd_watch(char *cmd, size_t n, void *ctx)
{
	CHAIN *c = ctx;

	if (c->watches_sent >= CHAIN_WATCHES)
		return 0;
	snprintf(cmd, n, "watch %d", c->watches_sent++);
	return 1;
}

static int step_answered_watch(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
	CHAIN *c = ctx;

	(void)how; (void)ans;
	c->watches_answered++;
	return E_D_STEP_AGAIN;
}

static int step_cmd_last(char *cmd, size_t n, void *ctx)
{
	(void)ctx;
	snprintf(cmd, n, "last");
	return 1;
}

static int step_answered_stop(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
	(void)how; (void)ans; (void)ctx;
	return E_D_STEP_STOP;
}

static int step_cmd_never(char *cmd, size_t n, void *ctx)
{
	CHAIN *c = ctx;

	(void)cmd; (void)n;
	c->never_ran++;
	return 1;
}

static void chain_finished(const E_D_CHAIN_END *end, void *ctx)
{
	CHAIN *c = ctx;

	c->finished_calls++;
	c->finished_ok = end->ok;
	c->last = end->last;
	snprintf(c->last_cmd, sizeof c->last_cmd, "%s", end->last.cmd);
	c->last.cmd = c->last_cmd;
	c->said = end->said;
}

static const E_D_STEP chain_steps[] = {
	{ step_cmd_flush,   step_answered_next },
	{ step_cmd_skipped, step_answered_next },
	{ step_cmd_watch,   step_answered_watch },
	{ step_cmd_last,    step_answered_stop },
	{ step_cmd_never,   step_answered_next },
	{ NULL, NULL }
};

static void test_chain_runs_steps_in_order_and_ends_on_cancel(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	E_D_REQ_LINK link;
	E_D_REQUESTER via;
	CHAIN ch;
	int i;
	const char *want[] = { "flush", "watch 0", "watch 1", "watch 2", "last" };

	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	link.rq = &rq;
	link.io = &io;
	memset(&via, 0, sizeof via);
	via.request = e_d_req_request;
	via.engine = &link;
	memset(&ch, 0, sizeof ch);
	chk_int("chain: started", e_d_chain_run(&via, chain_steps, &ch, chain_finished), 1);
	for (i = 0; i < 5; i++) {
		chk_int("chain: one command in flight", f.nsent, i + 1);
		chk_str("chain: step order", f.sent[i], want[i]);
		chk_int("chain: not finished yet", ch.finished_calls, 0);
		feed(&rq, &io, "ok\n" PROMPT);
	}
	chk_int("chain: no further command", f.nsent, 5);
	chk_int("chain: watches answered", ch.watches_answered, CHAIN_WATCHES);
	chk_int("chain: stop step ended it", ch.finished_calls, 1);
	chk_int("chain: ended well", ch.finished_ok, 1);
	chk_int("chain: step after stop never ran", ch.never_ran, 0);
	chk_int("chain: nothing pending", e_d_req_pending(&rq), 0);

	/* a cancel in the middle ends the chain quietly, once */
	memset(&ch, 0, sizeof ch);
	f.nsent = 0;
	e_d_chain_run(&via, chain_steps, &ch, chain_finished);
	feed(&rq, &io, "ok\n" PROMPT);
	e_d_req_cancel(&rq, &io, E_D_REQ_CANCELLED);
	chk_int("chain cancel: finished once", ch.finished_calls, 1);
	chk_int("chain cancel: not ok", ch.finished_ok, 0);
	chk_int("chain cancel: no step ran after", ch.watches_answered, 0);

	/* while the debugger still owes the cancelled answer a chain cannot
	   start: it is told so exactly once, and nothing is left running */
	memset(&ch, 0, sizeof ch);
	chk_int("chain refused: reports failure",
	        e_d_chain_run(&via, chain_steps, &ch, chain_finished), 0);
	chk_int("chain refused: finished once", ch.finished_calls, 1);
	chk_int("chain refused: not ok", ch.finished_ok, 0);
	feed(&rq, &io, SETTLE_MARKER "\n" PROMPT);
	chk_int("chain refused: settled, nothing pending", e_d_req_pending(&rq), 0);
	chk_int("chain refused: still finished once", ch.finished_calls, 1);

	/* the deadline in the middle of a chain ends it once, not ok */
	memset(&ch, 0, sizeof ch);
	e_d_chain_run(&via, chain_steps, &ch, chain_finished);
	advance(&f, &rq, &io, (long)E_D_REQ_TIMEOUT_S * MS_PER_S);
	chk_int("chain deadline: finished once", ch.finished_calls, 1);
	chk_int("chain deadline: not ok", ch.finished_ok, 0);
	feed(&rq, &io, "ok\n" PROMPT SETTLE_MARKER "\n" PROMPT);
	chk_int("chain deadline: the late answer runs no step",
	        ch.watches_answered, 0);
	chk_int("chain deadline: still finished once", ch.finished_calls, 1);
	chk_int("chain deadline: settled", e_d_req_pending(&rq), 0);
	e_d_req_free(&rq);
}

/* ---- a step's command is never cut and sent ---- */

#define LONG_STEP_BYTES   300        /* more than the old 255, far under the owner's number */
#define OVERLONG_EXTRA    10         /* how far a step passes the owner's number */

static size_t step_bytes;            /* how many bytes the next step writes */

static int step_cmd_sized(char *cmd, size_t n, void *ctx)
{
	size_t at;

	(void)ctx;
	snprintf(cmd, n, "%s", "b ");
	at = strlen(cmd);
	for (; at + 1 < n && at < step_bytes; at++)
		cmd[at] = 'f';
	cmd[at] = '\0';
	return 1;
}

static const E_D_STEP sized_steps[] = {
	{ step_cmd_sized, step_answered_stop },
	{ NULL, NULL }
};

/* Run a one-step chain whose command is @bytes long against a debugger named
   @name; returns what the chain finished with, -1 if it did not finish. */
static int run_sized_chain(const char *name, size_t bytes, int *sent, size_t *sent_len)
{
	static FAKE f;
	static E_D_REQ_QUEUE rq;
	E_D_REQ_IO io;
	E_D_REQ_LINK link;
	E_D_REQUESTER via;
	CHAIN ch;

	world_init(&f, &io, 0);
	io.name = name;
	e_d_req_init(&rq, NULL);
	link.rq = &rq;
	link.io = &io;
	memset(&via, 0, sizeof via);
	via.request = e_d_req_request;
	via.engine = &link;
	memset(&ch, 0, sizeof ch);
	step_bytes = bytes;
	e_d_chain_run(&via, sized_steps, &ch, chain_finished);
	*sent = f.nsent;
	*sent_len = f.nsent > 0 ? strlen(f.sent[0]) : 0;
	if (f.nsent > 0)
		feed(&rq, &io, "ok\n" PROMPT);
	e_d_req_free(&rq);
	return ch.finished_calls == 1 ? ch.finished_ok : -1;
}

static void test_a_step_is_sent_whole_or_not_at_all(void)
{
	int sent;
	size_t len;

	/* a step of 300 bytes was cut to 255 and sent: gdb then answered about
	   a file that does not exist */
	chk_int("step: a 300-byte step ends well", run_sized_chain("gdb", LONG_STEP_BYTES, &sent, &len), 1);
	chk_int("step: it was sent", sent, 1);
	chk_int("step: whole", (long)len, LONG_STEP_BYTES);

	/* the longest command the owner takes is sent whole too */
	chk_int("step: the longest command ends well",
	        run_sized_chain("gdb", E_D_REQ_CMD_MAX - 1, &sent, &len), 1);
	chk_int("step: the longest was sent whole", (long)len, E_D_REQ_CMD_MAX - 1);

	/* a step that writes more than the owner takes is cut to the owner's
	   number, which every debugger refuses: nothing is sent and the chain
	   ends failed */
	chk_int("step: an overlong step fails the chain",
	        run_sized_chain("gdb", E_D_REQ_CMD_MAX + OVERLONG_EXTRA, &sent, &len), 0);
	chk_int("step: and nothing was sent", sent, 0);

	/* a debugger nobody measured: a step over 255 is refused, as before */
	chk_int("step: dbx refuses a 300-byte step", run_sized_chain("dbx", LONG_STEP_BYTES, &sent, &len), 0);
	chk_int("step: and nothing was sent for it", sent, 0);
}

/* ---- how a step ended, and the words that say it ---- */

/* @scenario SC-debug-136 */
/* D1, D2, D3, D5, D7, D11 and the rows after them: every end of a step
   reaches its owner as a record, the words are written in one place, and a
   command refused for its length is a hollow mark and a sentence, not a
   silent end. */

#define REFUSED_MANY      60        /* refused steps in a row (D3) */
#define SHOWN_FILLER_BYTES 62        /* "b " plus these fill E_D_REQ_CMD_SHOWN bytes */
#define FRAME_SLACK        4096      /* a loop keeps its builders in one frame */
#define UTF8_E_ACUTE       "\xc3\xa9"

/* A world with a requester that says its sentences through the engine's
   notice, as the editor's does. */
typedef struct
{
	FAKE f;
	E_D_REQ_QUEUE rq;
	E_D_REQ_IO io;
	E_D_REQ_LINK link;
	E_D_REQUESTER via;
} WORLD;

static void world_make(WORLD *w, const char *name)
{
	world_init(&w->f, &w->io, 1);
	w->io.name = name;
	e_d_req_init(&w->rq, NULL);
	w->link.rq = &w->rq;
	w->link.io = &w->io;
	memset(&w->via, 0, sizeof w->via);
	w->via.request = e_d_req_request;
	w->via.notice = e_d_req_notice;
	w->via.name = name;
	w->via.engine = &w->link;
}

/* The expected first E_D_REQ_CMD_SHOWN bytes of the sized command. */
static void shown_sized(char *out, size_t n)
{
	size_t i;

	snprintf(out, n, "b ");
	for (i = 0; i < SHOWN_FILLER_BYTES; i++)
		strcat(out, "f");
}

/* A step whose failed keeps the record it is told, and takes a refusal for
   its length the way a breakpoint does: the mark turns hollow, the chain goes
   on. */
static int step_failed_mark(const E_D_STEP_END *end, void *ctx)
{
	CHAIN *c = ctx;

	c->failed_calls++;
	if (c->failed_calls == 1) {
		c->seen = *end;
		snprintf(c->seen_cmd, sizeof c->seen_cmd, "%s", end->cmd);
		snprintf(c->seen_words, sizeof c->seen_words, "%s", end->words);
		snprintf(c->seen_reason, sizeof c->seen_reason, "%s", end->reason);
		c->seen.cmd = c->seen_cmd;
		c->seen.words[0] = '\0';
	}
	if (end->how == E_D_REQ_NOT_SENT && end->code == E_D_SEND_TOO_LONG) {
		c->refused_marks++;
		return E_D_STEP_AGAIN;
	}
	return E_D_STEP_STOP;
}

/* The same, going on to the next step rather than staying on this one. */
static int step_failed_next(const E_D_STEP_END *end, void *ctx)
{
	step_failed_mark(end, ctx);
	return E_D_STEP_NEXT;
}

/* A builder that is refused REFUSED_MANY times, then has nothing left; it
   notes where on the stack it ran. */
static int step_cmd_refused_many(char *cmd, size_t n, void *ctx)
{
	CHAIN *c = ctx;
	char here;
	unsigned long at = (unsigned long)(void *)&here;

	if (c->frame_low == 0 || at < c->frame_low)
		c->frame_low = at;
	if (at > c->frame_high)
		c->frame_high = at;
	if (c->refusals <= 0)
		return 0;
	c->refusals--;
	step_bytes = LONG_STEP_BYTES;
	return step_cmd_sized(cmd, n, ctx);
}

static const E_D_STEP refused_next_steps[] = {
	{ step_cmd_sized, step_answered_next, step_failed_next },
	{ step_cmd_last,  step_answered_stop },
	{ NULL, NULL }
};

static const E_D_STEP refused_mark_steps[] = {
	{ step_cmd_sized, step_answered_next, step_failed_mark },
	{ step_cmd_last,  step_answered_stop },
	{ NULL, NULL }
};

static const E_D_STEP refused_many_steps[] = {
	{ step_cmd_refused_many, step_answered_next, step_failed_mark },
	{ step_cmd_last,         step_answered_stop },
	{ NULL, NULL }
};

static void test_a_refused_step_reaches_its_owner_and_the_chain_goes_on(void)
{
	static WORLD w;
	CHAIN ch;
	char shown[E_D_REQ_CMD_SHOWN + 1];

	/* D1: the engine refuses a command for its length; the step's owner
	   takes it (a breakpoint turns hollow) and the next step goes out */
	world_make(&w, "dbx");
	memset(&ch, 0, sizeof ch);
	step_bytes = LONG_STEP_BYTES;
	chk_int("D1: started", e_d_chain_run(&w.via, refused_next_steps, &ch, chain_finished), 1);
	chk_int("D1: the owner heard it once", ch.failed_calls, 1);
	chk_int("D1: as not sent", ch.seen.how, E_D_REQ_NOT_SENT);
	chk_int("D1: for its length", ch.seen.code, E_D_SEND_TOO_LONG);
	chk_str("D1: the reason alone", ch.seen_reason,
	        "the command is longer than dbx takes");
	chk_has("D1: the words hold the reason", ch.seen_words,
	        "the command is longer than dbx takes");
	shown_sized(shown, sizeof shown);
	chk_has("D1: the words show the start of the command", ch.seen_words, shown);
	chk_has("D1: and mark that the rest is left out", ch.seen_words, "...`");
	chk_int("D1: the command is not in the words in full",
	        strlen(ch.seen_words) < E_D_REQ_MSG_MAX && !strstr(ch.seen_words, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"), 1);
	chk_int("D1: the next step's command went out", w.f.nsent, 1);
	chk_str("D1: it is the next step's", w.f.sent[0], "last");
	feed(&w.rq, &w.io, "ok\n" PROMPT);
	chk_int("D1: finished once", ch.finished_calls, 1);
	chk_int("D1: ended well", ch.finished_ok, 1);
	chk_int("D1: nothing was said, the owner took it", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

static void test_a_refused_step_nobody_took_is_said_once(void)
{
	static WORLD w;
	CHAIN ch;
	char shown[E_D_REQ_CMD_SHOWN + 1], want[TEXT_ROOM];

	/* D2: no failed in the table: the chain ends not ok, with the record,
	   and says it once */
	world_make(&w, "dbx");
	memset(&ch, 0, sizeof ch);
	step_bytes = LONG_STEP_BYTES;
	chk_int("D2: reports failure", e_d_chain_run(&w.via, sized_steps, &ch, chain_finished), 0);
	chk_int("D2: finished once", ch.finished_calls, 1);
	chk_int("D2: not ok", ch.finished_ok, 0);
	chk_int("D2: last is not sent", ch.last.how, E_D_REQ_NOT_SENT);
	chk_int("D2: last is for its length", ch.last.code, E_D_SEND_TOO_LONG);
	chk_int("D2: said", ch.said, 1);
	chk_int("D2: one notice", w.f.nnotice, 1);
	shown_sized(shown, sizeof shown);
	snprintf(want, sizeof want,
	         "dbx was not given `%s...`: the command is longer than dbx takes"
	         E_D_REQ_QUIT_TAIL, shown);
	chk_str("D2: the sentence", w.f.notice[0], want);
	chk_int("D2: nothing was sent", w.f.nsent, 0);
	e_d_req_free(&w.rq);

	/* the same for the longest command the editor can build: the reason is
	   in the sentence however long the command is */
	world_make(&w, "gdb");
	memset(&ch, 0, sizeof ch);
	step_bytes = E_D_REQ_CMD_MAX + OVERLONG_EXTRA;
	e_d_chain_run(&w.via, sized_steps, &ch, chain_finished);
	chk_int("D2 long: one notice", w.f.nnotice, 1);
	chk_has("D2 long: the reason is there", w.f.notice[0],
	        "the command is longer than gdb takes" E_D_REQ_QUIT_TAIL);
	chk_has("D2 long: so is the debugger's name", w.f.notice[0], "gdb was not given");
	chk_int("D2 long: the sentence fits the row", strlen(w.f.notice[0]) < E_D_REQ_MSG_MAX, 1);
	e_d_req_free(&w.rq);

	/* a requester with no notice says nothing, and the record says so */
	world_make(&w, "dbx");
	w.via.notice = NULL;
	memset(&ch, 0, sizeof ch);
	step_bytes = LONG_STEP_BYTES;
	e_d_chain_run(&w.via, sized_steps, &ch, chain_finished);
	chk_int("D2 quiet: no notice", w.f.nnotice, 0);
	chk_int("D2 quiet: not said", ch.said, 0);
	chk_int("D2 quiet: still not ok", ch.finished_ok, 0);
	e_d_req_free(&w.rq);
}

static void test_sixty_refused_steps_run_in_one_frame(void)
{
	static WORLD w;
	CHAIN ch;

	/* D3: each refusal is taken by its owner; the chain loops, it does not
	   nest, so the builders of the first and the last ran in one frame */
	world_make(&w, "dbx");
	memset(&ch, 0, sizeof ch);
	ch.refusals = REFUSED_MANY;
	e_d_chain_run(&w.via, refused_many_steps, &ch, chain_finished);
	chk_int("D3: every refusal reached its owner", ch.refused_marks, REFUSED_MANY);
	chk_int("D3: the last step went out", w.f.nsent, 1);
	feed(&w.rq, &w.io, "ok\n" PROMPT);
	chk_int("D3: finished once", ch.finished_calls, 1);
	chk_int("D3: ended well", ch.finished_ok, 1);
	chk_int("D3: one frame", ch.frame_high - ch.frame_low < FRAME_SLACK, 1);
	chk_int("D3: nothing said", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

static void test_the_engine_says_a_deadline_once_and_the_chain_not_at_all(void)
{
	static WORLD w;
	CHAIN ch;

	/* D5: the deadline in the middle of a chain: the engine says it, the
	   chain adds nothing */
	world_make(&w, "gdb");
	memset(&ch, 0, sizeof ch);
	e_d_chain_run(&w.via, chain_steps, &ch, chain_finished);
	advance(&w.f, &w.rq, &w.io, (long)E_D_REQ_TIMEOUT_S * MS_PER_S);
	chk_int("D5: finished once", ch.finished_calls, 1);
	chk_int("D5: timed out", ch.last.how, E_D_REQ_TIMED_OUT);
	chk_int("D5: said", ch.said, 1);
	chk_int("D5: exactly one notice", w.f.nnotice, 1);
	chk_has("D5: it is the engine's", w.f.notice[0], "did not answer `flush`");
	e_d_req_free(&w.rq);
}

static void test_a_chain_answered_throughout_is_quiet(void)
{
	static WORLD w;
	CHAIN ch;
	int i;

	/* D7: the control: every step answered */
	world_make(&w, "gdb");
	memset(&ch, 0, sizeof ch);
	e_d_chain_run(&w.via, chain_steps, &ch, chain_finished);
	for (i = 0; i < 5; i++)
		feed(&w.rq, &w.io, "ok\n" PROMPT);
	chk_int("D7: finished once", ch.finished_calls, 1);
	chk_int("D7: ok", ch.finished_ok, 1);
	chk_int("D7: the record says answered", ch.last.how, E_D_REQ_DONE);
	chk_int("D7: not said", ch.said, 0);
	chk_int("D7: no notice", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

static void test_a_refusal_that_passes_is_not_a_final_mark(void)
{
	static WORLD w;
	CHAIN ch;
	static CALL c;

	/* D11: the debugger still owes an old answer: the step is refused as
	   RESYNC; a step like a breakpoint's marks only what is final, so the
	   mark stays unsent, the chain ends not ok and says it once */
	world_make(&w, "pdb");
	call_init(&c, 1);
	e_d_req_send(&w.rq, &w.io, "step", on_answered, &c);
	e_d_req_cancel(&w.rq, &w.io, E_D_REQ_CANCELLED);
	w.f.nnotice = 0;
	memset(&ch, 0, sizeof ch);
	step_bytes = LONG_STEP_BYTES;
	e_d_chain_run(&w.via, refused_mark_steps, &ch, chain_finished);
	chk_int("D11: the owner heard it", ch.failed_calls, 1);
	chk_int("D11: as a resync", ch.seen.code, E_D_SEND_RESYNC);
	chk_int("D11: and marked nothing", ch.refused_marks, 0);
	chk_int("D11: the chain ended not ok", ch.finished_ok, 0);
	chk_int("D11: one notice", w.f.nnotice, 1);
	chk_has("D11: it names the old answer", w.f.notice[0],
	        "pdb still owes an old answer" E_D_REQ_QUIT_TAIL);
	e_d_req_free(&w.rq);
}

static int step_cmd_stops(char *cmd, size_t n, void *ctx)
{
	(void)cmd; (void)n; (void)ctx;
	return E_D_STEP_CMD_STOP;
}

static const E_D_STEP cmd_stop_steps[] = {
	{ step_cmd_flush, step_answered_next },
	{ step_cmd_stops, step_answered_next, step_failed_mark },
	{ step_cmd_never, step_answered_next },
	{ NULL, NULL }
};

static void test_a_builder_that_cannot_go_on_ends_the_chain(void)
{
	static WORLD w;
	CHAIN ch;

	/* a builder says the step cannot go on: its owner hears it with the
	   chain's last command, the chain ends not ok, and the owner, which
	   has the words, is the only one to say them */
	world_make(&w, "pdb");
	memset(&ch, 0, sizeof ch);
	e_d_chain_run(&w.via, cmd_stop_steps, &ch, chain_finished);
	feed(&w.rq, &w.io, "ok\n" PROMPT);
	chk_int("stop: the owner heard it", ch.failed_calls, 1);
	chk_int("stop: as another prompt", ch.seen.how, E_D_REQ_OTHER_PROMPT);
	chk_str("stop: with the last command", ch.seen_cmd, "flush");
	chk_int("stop: finished once", ch.finished_calls, 1);
	chk_int("stop: not ok", ch.finished_ok, 0);
	chk_int("stop: no later step ran", ch.never_ran, 0);
	chk_int("stop: the owner said it, the chain did not", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

static void test_a_chain_that_cannot_start_hands_the_unstarted_record(void)
{
	E_D_CHAIN_END end;

	memset(&end, 0xA5, sizeof end);
	e_d_chain_end_unstarted(&end);
	chk_int("unstarted: not ok", end.ok, 0);
	chk_int("unstarted: not sent", end.last.how, E_D_REQ_NOT_SENT);
	chk_int("unstarted: nobody to ask", end.last.code, E_D_SEND_GONE);
	chk_int("unstarted: not said", end.said, 0);
	chk_str("unstarted: no command", end.last.cmd, "");
	/* a box made from it never reads an empty line */
	chk_str("unstarted: words, not an empty box", end.last.words,
	        "the debugger could not run its commands: the session is over");
}

static int step_failed_says_itself(const E_D_STEP_END *end, void *ctx)
{
	(void)end; (void)ctx;
	return E_D_STEP_STOP_SAID;
}

static const E_D_STEP said_by_owner_steps[] = {
	{ step_cmd_sized, step_answered_next, step_failed_says_itself },
	{ step_cmd_last,  step_answered_stop },
	{ NULL, NULL }
};

static void test_a_stopped_chain_whose_owner_says_it_is_said_once(void)
{
	static WORLD w;
	CHAIN ch;

	/* an owner that keeps a box for a refused command says it itself: the
	   chain adds no sentence on the status row, so one stopped start reads
	   its reason once */
	world_make(&w, "dbx");
	memset(&ch, 0, sizeof ch);
	step_bytes = LONG_STEP_BYTES;
	e_d_chain_run(&w.via, said_by_owner_steps, &ch, chain_finished);
	chk_int("said once: the chain ended not ok", ch.finished_ok, 0);
	chk_int("said once: told once", ch.finished_calls, 1);
	chk_int("said once: said", ch.said, 1);
	chk_int("said once: nothing on the status row", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

/* A refusal whose words begin with a long path keeps the cause that ends
   them: gdb's "cd <path>" answer names the path first and the reason last. */
static void test_a_long_refusal_keeps_its_cause(void)
{
	E_D_STEP_END end;
	char words[600], *p = words;
	size_t i, len;

	memset(&end, 0, sizeof end);
	*p++ = '/';
	for (i = 0; i < 100; i++) {           /* "/\xc3\xa9\xc3\xa9..." 200 bytes */
		*p++ = (char)0xc3;
		*p++ = (char)0xa9;
	}
	snprintf(p, sizeof words - (size_t)(p - words), "%s",
	         "/ws: No such file or directory.");
	e_d_req_set_reason(&end, words);
	len = strlen(end.reason);
	chk_int("long reason: fits", len < sizeof end.reason, 1);
	chk_has("long reason: the cause is kept", end.reason,
	        "ws: No such file or directory.");
	chk_has("long reason: the cut is shown", end.reason, "...");
	chk_int("long reason: begins with the path", end.reason[0], '/');
	for (i = 1; end.reason[i] != '.'; i += 2)   /* whole characters before the cut */
		chk_int("long reason: no character cut", (unsigned char)end.reason[i], 0xc3);
	end.how = E_D_REQ_REFUSED;
	end.cmd = "cd /x";
	end.prompt = "";
	e_d_req_end_words(&end, "gdb");
	chk_has("long reason: the sentence ends with the cause", end.words,
	        "No such file or directory.");
	e_d_req_set_reason(&end, "short words");
	chk_str("long reason: short words are kept whole", end.reason, "short words");
}

static void test_the_words_of_an_end_are_written_in_one_place(void)
{
	E_D_STEP_END end;
	char want[TEXT_ROOM], longname[TEXT_ROOM];

	memset(&end, 0, sizeof end);
	end.how = E_D_REQ_NOT_SENT;
	end.code = E_D_SEND_FULL;
	end.cmd = "p x";
	end.where = "./.pdbrc";
	e_d_req_end_words(&end, "pdb");
	chk_str("words: the reason", end.reason, "eight commands are already waiting");
	chk_str("words: the sentence", end.words,
	        "pdb was not given `p x` (./.pdbrc): eight commands are already waiting");

	/* a command cut at 64 bytes never ends inside a character */
	{
		char cmd[TEXT_ROOM];
		size_t i;

		for (i = 0; i < E_D_REQ_CMD_SHOWN - 1; i++)
			cmd[i] = 'a';
		cmd[i] = '\0';
		strcat(cmd, UTF8_E_ACUTE "zzzz");
		end.where = NULL;
		end.cmd = cmd;
		end.code = E_D_SEND_GONE;
		e_d_req_end_words(&end, "pdb");
		snprintf(want, sizeof want, "pdb was not given `%.*s...`: the session is over",
		         E_D_REQ_CMD_SHOWN - 1, cmd);
		chk_str("words: a character is not cut", end.words, want);
	}

	/* a long file name cannot push the reason out of the sentence */
	memset(longname, 'n', sizeof longname - 1);
	longname[sizeof longname - 1] = '\0';
	end.cmd = "b x";
	end.where = longname;
	end.code = E_D_SEND_RESYNC;
	e_d_req_end_words(&end, "gdb");
	chk_has("words: the reason survives a long file", end.words,
	        "gdb still owes an old answer");
	chk_int("words: and the sentence fits", strlen(end.words) < E_D_REQ_MSG_MAX, 1);

	/* another prompt names the prompt */
	end.how = E_D_REQ_OTHER_PROMPT;
	end.where = NULL;
	end.prompt = "(com)";
	e_d_req_end_words(&end, "pdb");
	chk_str("words: another prompt", end.words,
	        "pdb answered `b x` with its (com) prompt, not its own");
}

static void test_a_stranded_debugger_refuses_every_sender(void)
{
	static WORLD w;
	char msg[TEXT_ROOM];
	CALL c;

	/* the gate every writer asks: the engine says it is stranded, refuses
	   with its own code, names the command it owes, and a cancel for the
	   session's end lifts it */
	world_make(&w, "pdb");
	chk_int("stranded: not at first", e_d_req_stranded(&w.rq), 0);
	w.rq.stranded = 1;
	w.rq.at_prompt = &other_rows[OTHER_INTERACT];
	snprintf(w.rq.owed_cmd, sizeof w.rq.owed_cmd, "%s", "interact");
	call_init(&c, 1);
	chk_int("stranded: asked", e_d_req_stranded(&w.rq), 1);
	chk_int("stranded: a send is refused",
	        e_d_req_send(&w.rq, &w.io, "p 1", on_answered, &c), E_D_SEND_STRANDED);
	e_d_req_busy_message(&w.rq, &w.io, msg, sizeof msg);
	chk_str("stranded: the user reads why", msg,
	        "pdb is at its >>> prompt after `interact`" E_D_REQ_QUIT_TAIL);
	e_d_req_cancel(&w.rq, &w.io, E_D_REQ_GONE);
	chk_int("stranded: the session's end lifts it", e_d_req_stranded(&w.rq), 0);
	e_d_req_free(&w.rq);
}

/* ---- edges the request engine must hold ---- */

#define MI_PROMPT "(gdb) \n"

/* An MI-shaped reader: the prompt is a whole line, so an answer can end in
   the middle of a read and what follows is not the engine's. */
static int mi_answer_ended(const E_D_ANSWER *a, size_t *prompt_len, void *io)
{
	size_t n = strlen(MI_PROMPT);

	(void)io;
	*prompt_len = n;
	return a->text && a->len >= n && strcmp(a->text + a->len - n, MI_PROMPT) == 0;
}

static void test_request_engine_edges(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c, d;
	const char *two = "^done\n" MI_PROMPT "*stopped,reason=\"x\"\n";
	size_t took, big = (size_t)E_D_REQ_ANSWER_MAX + 4096, i;
	char *blob;

	/* an answer that ends inside a read: the rest is handed back */
	world_init(&f, &io, 0);
	io.answer_ended = mi_answer_ended;
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	e_d_req_send(&rq, &io, "-stack-list-frames", on_answered, &c);
	took = e_d_req_feed(&rq, &io, two, strlen(two));
	chk_int("edge: answered", c.calls, 1);
	chk_str("edge: answer without its prompt", c.text, "^done\n");
	chk_int("edge: only the answer's bytes taken", (long)took,
	        (long)(strlen("^done\n") + strlen(MI_PROMPT)));
	chk_int("edge: the stop is left for the controller", f.nasync, 0);
	e_d_req_free(&rq);

	/* a stop while the second request of the queue waits: it is delivered,
	   the first answer goes to the first request, the second to the second */
	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	call_init(&d, 2);
	e_d_req_send(&rq, &io, "p a", on_answered, &c);
	e_d_req_send(&rq, &io, "p b", on_answered, &d);
	feed(&rq, &io, "$1");
	feed(&rq, &io, " = 1\n" ASYNC_STOP ",reason=\"z\"\n");
	feed(&rq, &io, "(g");
	feed(&rq, &io, "db");
	feed(&rq, &io, ") ");
	chk_int("edge: first answered once", c.calls, 1);
	chk_str("edge: first gets its answer", c.text, "$1 = 1\n");
	chk_int("edge: the stop is not lost", f.nasync, 1);
	chk_int("edge: second now out", f.nsent, 2);
	chk_int("edge: second not yet answered", d.calls, 0);
	feed(&rq, &io, "$2 = 2\n" PROMPT);
	chk_str("edge: second gets its own answer", d.text, "$2 = 2\n");
	chk_int("edge: second answered once", d.calls, 1);
	e_d_req_free(&rq);

	/* the deadline while half a line is held: the owner sees it */
	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	e_d_req_send(&rq, &io, "bt", on_answered, &c);
	feed(&rq, &io, "#0 main\n#1 ha");
	advance(&f, &rq, &io, (long)E_D_REQ_TIMEOUT_S * MS_PER_S);
	chk_int("edge: timed out once", c.calls, 1);
	chk_int("edge: timed out", c.how, E_D_REQ_TIMED_OUT);
	chk_str("edge: partial answer kept", c.text, "#0 main\n#1 ha");
	/* a stale timer after the end does nothing */
	e_d_req_timer(&rq, &io, E_D_TIMER_DEADLINE);
	e_d_req_timer(&rq, &io, E_D_TIMER_QUIET);
	chk_int("edge: stale timers do nothing", c.calls, 1);
	chk_int("edge: one notice", f.nnotice, 1);
	/* the error stream is not mixed into the resync, and a stop during the
	   resync still reaches the controller */
	e_d_req_feed_errors(&rq, &io, "oops\n", 5);
	feed(&rq, &io, "lf\n" ASYNC_STOP ",reason=\"r\"\n" PROMPT);
	chk_int("edge: stop during resync delivered", f.nasync, 1);
	chk_int("edge: still resyncing", e_d_req_pending(&rq), 1);
	/* an old answer longer than an answer's bound does not hide the marker */
	blob = malloc(big + 1);
	for (i = 0; i < big; i++)
		blob[i] = (i % 80 == 79) ? '\n' : 'x';
	blob[big] = '\0';
	feed(&rq, &io, blob);
	feed(&rq, &io, "\n" PROMPT SETTLE_MARKER "\n" PROMPT);
	chk_int("edge: settled after a long old answer", e_d_req_pending(&rq), 0);
	e_d_req_free(&rq);

	/* an answer longer than its bound still ends at its prompt */
	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	e_d_req_send(&rq, &io, "info functions", on_answered, &c);
	feed(&rq, &io, blob);
	feed(&rq, &io, "\n" PROMPT);
	chk_int("edge: long answer ends at its prompt", c.calls, 1);
	chk_int("edge: long answer done", c.how, E_D_REQ_DONE);
	chk_int("edge: no deadline needed", f.nnotice, 0);
	e_d_req_free(&rq);
	free(blob);

	/* the error hook is forgotten once the next request goes out */
	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	late_calls = 0;
	e_d_req_send(&rq, &io, "break 5", on_answered, &c);
	e_d_req_set_late_error(&rq, on_late_error, NULL);
	feed(&rq, &io, PROMPT);
	e_d_req_send(&rq, &io, "p q", on_answered, &d);
	feed(&rq, &io, PROMPT);
	e_d_req_feed_errors(&rq, &io, "late\n", 5);
	chk_int("edge: old hook not called", late_calls, 0);
	e_d_req_free(&rq);
}

/* ---- the error stream and the prompt read together ---- */

#define REFUSAL "No source file named q.c.\n"

static void test_error_line_before_the_prompt_stays_with_its_answer(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c, d;

	/* the refusal was written before the prompt, the prompt is read first:
	   the refusal is still the answer's, and the next request's answer is
	   its own */
	world_init(&f, &io, 0);
	io.errors_waiting = fake_errors_waiting;
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	call_init(&d, 2);
	e_d_req_send(&rq, &io, "b q.c:2", on_answered, &c);
	e_d_req_send(&rq, &io, "p g", on_answered, &d);
	fake_write_error(&f, REFUSAL);
	feed(&rq, &io, PROMPT);
	chk_int("stderr first: answered", c.calls, 1);
	chk_int("stderr first: answer is an error", c.error, 1);
	chk_str("stderr first: the refusal is in it", c.text, REFUSAL);
	chk_int("stderr first: nothing left waiting", (long)f.nerrors, 0);
	feed(&rq, &io, "$1 = 42\n" PROMPT);
	chk_int("stderr first: next answered", d.calls, 1);
	chk_int("stderr first: next is no error", d.error, 0);
	chk_str("stderr first: next has its own answer", d.text, "$1 = 42\n");
	e_d_req_free(&rq);

	/* an answer with words on its output keeps them first */
	world_init(&f, &io, 0);
	io.errors_waiting = fake_errors_waiting;
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	e_d_req_send(&rq, &io, "frame 9", on_answered, &c);
	fake_write_error(&f, "No frame at level 9.\n");
	feed(&rq, &io, "#0 main\n" PROMPT);
	chk_str("both streams: output then error", c.text,
	        "#0 main\nNo frame at level 9.\n");
	chk_int("both streams: an error", c.error, 1);
	e_d_req_free(&rq);

	/* a debugger without the hook is answered as before */
	world_init(&f, &io, 0);
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	e_d_req_send(&rq, &io, "b 2", on_answered, &c);
	fake_write_error(&f, REFUSAL);
	feed(&rq, &io, PROMPT);
	chk_int("no hook: not an error", c.error, 0);
	chk_int("no hook: the stream is left alone", (long)f.nerrors,
	        (long)strlen(REFUSAL));
	e_d_req_free(&rq);

	/* the refusal of a command given up on ends with the resync: it never
	   joins the next answer */
	world_init(&f, &io, 1);
	io.errors_waiting = fake_errors_waiting;
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	call_init(&d, 2);
	e_d_req_send(&rq, &io, "b q.c:2", on_answered, &c);
	e_d_req_cancel(&rq, &io, E_D_REQ_CANCELLED);
	fake_write_error(&f, REFUSAL);
	feed(&rq, &io, PROMPT SETTLE_MARKER "\n" PROMPT);
	chk_int("resync: over", e_d_req_pending(&rq), 0);
	chk_int("resync: the old refusal is dropped", (long)f.nerrors, 0);
	e_d_req_send(&rq, &io, "p g", on_answered, &d);
	feed(&rq, &io, "$1 = 42\n" PROMPT);
	chk_int("resync: next is no error", d.error, 0);
	chk_str("resync: next has its own answer", d.text, "$1 = 42\n");
	e_d_req_free(&rq);
}

/* ---- a resync for numbered settle commands ---- */

#define NUMBERED_SETTLE_FMT "echo " SETTLE_MARKER "-%d"
#define NUMBERED_MARKER_FMT SETTLE_MARKER "-%d"
#define OWED_ROUND          2     /* the settle round the user gave up on */

/* The world's next numbered settle command: one past the round owed. */
static void fake_resync_settle(char *cmd, size_t cmd_n, char *marker,
                               size_t marker_n, void *io)
{
	(void)io;
	snprintf(marker, marker_n, NUMBERED_MARKER_FMT, OWED_ROUND + 1);
	snprintf(cmd, cmd_n, NUMBERED_SETTLE_FMT, OWED_ROUND + 1);
}

static void test_resync_waits_for_its_own_numbered_echo(void)
{
	static FAKE f;
	E_D_REQ_IO io;
	static E_D_REQ_QUEUE rq;
	CALL c, d;
	static char owed[E_D_REQ_CMD_MAX];
	const char *text = "SETTLED-12\nSETTLED-1\n";

	chk_int("marker: a longer number is not it",
	        e_d_req_marker_in("SETTLED-12\n", "SETTLED-1") == NULL, 1);
	chk_int("marker: its own number is found after a longer one",
	        (long)(e_d_req_marker_in(text, "SETTLED-1") - text),
	        (long)strlen("SETTLED-12\n"));
	chk_int("marker: an empty marker is never found",
	        e_d_req_marker_in(text, "") == NULL, 1);

	world_init(&f, &io, 1);
	io.resync_settle = fake_resync_settle;
	e_d_req_init(&rq, NULL);
	call_init(&c, 1);
	call_init(&d, 2);
	snprintf(owed, sizeof owed, NUMBERED_SETTLE_FMT, OWED_ROUND);
	e_d_req_send(&rq, &io, owed, on_answered, &c);
	e_d_req_cancel(&rq, &io, E_D_REQ_CANCELLED);
	chk_str("numbered: the resync asks for its own echo", f.sent[1],
	        "echo SETTLED-3");
	feed(&rq, &io, "SETTLED-2\n" PROMPT);
	chk_int("numbered: the owed echo does not end the resync",
	        e_d_req_pending(&rq), 1);
	feed(&rq, &io, "SETTLED-31\n" PROMPT);
	chk_int("numbered: nor one whose number only starts with its own",
	        e_d_req_pending(&rq), 1);
	feed(&rq, &io, "SETTLED-3\n" PROMPT);
	chk_int("numbered: its own echo ends it", e_d_req_pending(&rq), 0);
	e_d_req_send(&rq, &io, "p g", on_answered, &d);
	feed(&rq, &io, "$1 = 42\n" PROMPT);
	chk_str("numbered: the next command has its own answer", d.text,
	        "$1 = 42\n");
	e_d_req_free(&rq);
}

/* ---- the debugger's other prompts ---- */

/* @scenario SC-debug-138 */
/* D4, D6, D8, D9, D10, D12, D13: an answer that ends at another prompt of the
   debugger is told by the engine's quiet timer and never by a byte boundary;
   a step may take the prompt, or the debugger is left stranded there and
   every sender is refused. */

static int step_cmd_interact(char *cmd, size_t n, void *ctx)
{
	(void)ctx;
	snprintf(cmd, n, "interact");
	return 1;
}

/* What the step owners below say to an other prompt. */
static int prompt_verdict = E_D_STEP_STOP;

static int step_failed_prompt(const E_D_STEP_END *end, void *ctx)
{
	CHAIN *c = ctx;

	c->failed_calls++;
	c->seen = *end;
	snprintf(c->seen_cmd, sizeof c->seen_cmd, "%s", end->cmd);
	c->seen.cmd = c->seen_cmd;
	if (end->how == E_D_REQ_OTHER_PROMPT && end->ans && end->ans->text)
		snprintf(c->seen_words, sizeof c->seen_words, "%s", end->ans->text);
	if (end->how == E_D_REQ_OTHER_PROMPT)
		return prompt_verdict;
	return E_D_STEP_STOP;
}

static const E_D_STEP interact_steps[] = {
	{ step_cmd_flush,    step_answered_next },
	{ step_cmd_interact, step_answered_next, step_failed_prompt },
	{ step_cmd_never,    step_answered_next },
	{ NULL, NULL }
};

static void test_another_prompt_is_told_by_the_quiet_timer_and_strands(void)
{
	static WORLD w;
	CHAIN ch;
	CALL c;
	char msg[TEXT_ROOM];

	/* D4 */
	world_make(&w, "pdb");
	memset(&ch, 0, sizeof ch);
	prompt_verdict = E_D_STEP_STOP;
	e_d_chain_run(&w.via, interact_steps, &ch, chain_finished);
	feed(&w.rq, &w.io, "ok\n" PROMPT);
	feed(&w.rq, &w.io, "*pdb interact start*\n>>> ");
	chk_int("D4: nothing is decided at the byte", ch.failed_calls, 0);
	chk_int("D4: the verdict waits for the quiet time",
	        w.f.armed[E_D_TIMER_QUIET], w.f.now + E_D_REQ_PROMPT_MS);
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
	chk_int("D4: the owner heard it", ch.failed_calls, 1);
	chk_int("D4: as another prompt", ch.seen.how, E_D_REQ_OTHER_PROMPT);
	chk_str("D4: named", ch.seen.prompt, ">>>");
	chk_str("D4: with its command", ch.seen_cmd, "interact");
	chk_str("D4: the answer holds the banner", ch.seen_words, "*pdb interact start*\n");
	chk_int("D4: finished once", ch.finished_calls, 1);
	chk_int("D4: not ok", ch.finished_ok, 0);
	chk_int("D4: said", ch.said, 1);
	chk_int("D4: no later step ran", ch.never_ran, 0);
	chk_int("D4: one notice", w.f.nnotice, 1);
	chk_str("D4: the sentence", w.f.notice[0],
	        "pdb answered `interact` with its >>> prompt, not its own" E_D_REQ_QUIT_TAIL);
	call_init(&c, 1);
	chk_int("D4: stranded", e_d_req_stranded(&w.rq), 1);
	chk_int("D4: every sender is refused",
	        e_d_req_send(&w.rq, &w.io, "p 1", on_answered, &c), E_D_SEND_STRANDED);
	e_d_req_busy_message(&w.rq, &w.io, msg, sizeof msg);
	chk_str("D4: the busy sentence names the prompt", msg,
	        "pdb is at its >>> prompt after `interact`" E_D_REQ_QUIT_TAIL);
	e_d_req_cancel(&w.rq, &w.io, E_D_REQ_GONE);
	chk_int("D4: the session's end lifts it", e_d_req_stranded(&w.rq), 0);
	e_d_req_free(&w.rq);

	/* the owner that says it itself leaves the engine silent, still stranded */
	world_make(&w, "pdb");
	memset(&ch, 0, sizeof ch);
	prompt_verdict = E_D_STEP_STOP_SAID;
	e_d_chain_run(&w.via, interact_steps, &ch, chain_finished);
	feed(&w.rq, &w.io, "ok\n" PROMPT);
	feed(&w.rq, &w.io, ">>> ");
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
	chk_int("D4b: the owner said it, nobody else", w.f.nnotice, 0);
	chk_int("D4b: still stranded", e_d_req_stranded(&w.rq), 1);
	chk_int("D4b: the chain knows it was said", ch.said, 1);
	e_d_req_free(&w.rq);
}

static void test_each_other_prompt_of_a_table_is_told(void)
{
	static const struct { const char *feed; const char *name; } rows[] = {
		{ "(com) ",        "(com)" },
		{ "((Pdb)) ",      "((Pdb))" },
		{ ">>> ",          ">>>" },
		{ "... ",          "..." },
		{ "...   ",        "..." },
		{ "define\n >",    ">" },
		{ "~\">\"\n",      ">" },
	};
	static WORLD w;
	CALL c;
	size_t i;

	/* D6 */
	for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
		char what[64];

		world_make(&w, "pdb");
		call_init(&c, 1);
		c.verdict = E_D_STEP_STOP;
		e_d_req_send(&w.rq, &w.io, "x", on_answered, &c);
		feed(&w.rq, &w.io, rows[i].feed);
		snprintf(what, sizeof what, "D6: %s waits", rows[i].name);
		chk_int(what, c.calls, 0);
		advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
		snprintf(what, sizeof what, "D6: row %d ends as another prompt", (int)i);
		chk_int(what, c.how, E_D_REQ_OTHER_PROMPT);
		chk_str("D6: named", c.prompt, rows[i].name);
		chk_int("D6: stranded", e_d_req_stranded(&w.rq), 1);
		e_d_req_free(&w.rq);
	}

	/* the own prompt ends DONE at once, no timer */
	world_make(&w, "pdb");
	call_init(&c, 1);
	e_d_req_send(&w.rq, &w.io, "x", on_answered, &c);
	feed(&w.rq, &w.io, "1\n" PROMPT);
	chk_int("D6: the own prompt is DONE", c.how, E_D_REQ_DONE);
	chk_int("D6: and nothing is stranded", e_d_req_stranded(&w.rq), 0);
	e_d_req_free(&w.rq);

	/* a settle command swallowed by a block: the resync strands, naming the
	   command the debugger still owes */
	world_make(&w, "pdb");
	call_init(&c, 1);
	e_d_req_send(&w.rq, &w.io, "commands 1", on_answered, &c);
	e_d_req_cancel(&w.rq, &w.io, E_D_REQ_CANCELLED);
	w.f.nnotice = 0;
	feed(&w.rq, &w.io, "(com) ");
	chk_int("D6: the resync waits", e_d_req_stranded(&w.rq), 0);
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
	chk_int("D6: a swallowed settle command strands", e_d_req_stranded(&w.rq), 1);
	chk_int("D6: the resync is over", e_d_req_pending(&w.rq), 0);
	chk_int("D6: one sentence", w.f.nnotice, 1);
	chk_has("D6: it names the owed command", w.f.notice[0],
	        "pdb answered `commands 1` with its (com) prompt");
	e_d_req_free(&w.rq);
}

static void test_the_error_stream_is_kept_apart_in_the_answer(void)
{
	static WORLD w;
	CALL c;

	/* D8 */
	world_make(&w, "gdb");
	w.io.errors_waiting = fake_errors_waiting;
	call_init(&c, 1);
	e_d_req_send(&w.rq, &w.io, "cd /x", on_answered, &c);
	fake_write_error(&w.f, "no such folder\n");
	feed(&w.rq, &w.io, PROMPT);
	chk_str("D8: err holds exactly the error bytes", c.err, "no such folder\n");
	chk_str("D8: the text still holds them", c.text, "no such folder\n");
	chk_int("D8: flagged", c.error, 1);

	/* a plain answer has no err */
	call_init(&c, 2);
	e_d_req_send(&w.rq, &w.io, "p 1", on_answered, &c);
	feed(&w.rq, &w.io, "$1 = 1\n" PROMPT);
	chk_str("D8: a plain answer has no err", c.err, "");
	e_d_req_free(&w.rq);
}

static void test_program_output_that_looks_like_a_prompt_is_not_one(void)
{
	static WORLD w;
	CALL c;

	/* D9: a program's "Loading... " is read as the end of a read, and the
	   debugger's own prompt follows within the quiet time */
	world_make(&w, "pdb");
	call_init(&c, 1);
	e_d_req_send(&w.rq, &w.io, "p x", on_answered, &c);
	feed(&w.rq, &w.io, "Loading... ");
	chk_int("D9: a candidate re-arms the quiet timer",
	        w.f.armed[E_D_TIMER_QUIET], w.f.now + E_D_REQ_PROMPT_MS);
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS / 2);
	feed(&w.rq, &w.io, "\n");
	chk_int("D9: later bytes take the candidate back",
	        w.f.armed[E_D_TIMER_QUIET], w.f.now + E_D_REQ_QUIET_MS);
	feed(&w.rq, &w.io, PROMPT);
	chk_int("D9: the request ended DONE", c.how, E_D_REQ_DONE);
	chk_int("D9: no notice", w.f.nnotice, 0);
	chk_int("D9: not stranded", e_d_req_stranded(&w.rq), 0);
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS * 4);
	chk_int("D9: and nothing fires later", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

static void test_mi_console_record_then_its_result_is_one_answer(void)
{
	static WORLD w;
	CALL c;

	/* D10: ~">" is a candidate at its line end, and 1^done and the prompt
	   follow in the same read */
	world_make(&w, "gdb");
	w.io.answer_ended = mi_answer_ended;
	call_init(&c, 1);
	e_d_req_send(&w.rq, &w.io, "3-interpreter-exec console \"python\"", on_answered, &c);
	feed(&w.rq, &w.io, "~\">\"\n1^done\n" MI_PROMPT);
	chk_int("D10: DONE", c.how, E_D_REQ_DONE);
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS * 4);
	chk_int("D10: no notice", w.f.nnotice, 0);
	chk_int("D10: not stranded", e_d_req_stranded(&w.rq), 0);
	e_d_req_free(&w.rq);
}

/* A step that speaks the lines of a block: "commands 1", "silent", "end". */
static int step_cmd_block(char *cmd, size_t n, void *ctx)
{
	CHAIN *c = ctx;
	static const char *lines[] = { "commands 1", "silent", "end" };

	if (c->watches_sent >= 3)
		return 0;
	snprintf(cmd, n, "%s", lines[c->watches_sent++]);
	return 1;
}

static int step_answered_again(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
	(void)how; (void)ans; (void)ctx;
	return E_D_STEP_AGAIN;
}

static const E_D_STEP block_steps[] = {
	{ step_cmd_block, step_answered_again, step_failed_prompt },
	{ step_cmd_last,  step_answered_stop },
	{ NULL, NULL }
};

static void test_a_step_may_take_the_prompt_of_a_block(void)
{
	static WORLD w;
	CHAIN ch;

	/* D12: pdb's (com) while a .pdbrc block is spoken, until its end */
	world_make(&w, "pdb");
	memset(&ch, 0, sizeof ch);
	prompt_verdict = E_D_STEP_AGAIN;
	e_d_chain_run(&w.via, block_steps, &ch, chain_finished);
	chk_str("D12: the block's first line", w.f.sent[0], "commands 1");
	feed(&w.rq, &w.io, "(com) ");
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
	chk_int("D12: taken, nobody says it", w.f.nnotice, 0);
	chk_str("D12: the next line goes out", w.f.sent[1], "silent");
	chk_int("D12: the writers are refused meanwhile", e_d_req_stranded(&w.rq), 1);
	feed(&w.rq, &w.io, "(com) ");
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
	chk_str("D12: and the leave", w.f.sent[2], "end");
	chk_int("D12: still refused", e_d_req_stranded(&w.rq), 1);
	feed(&w.rq, &w.io, PROMPT);
	chk_str("D12: the chain goes on past the block", w.f.sent[3], "last");
	chk_int("D12: the debugger is at its own prompt", e_d_req_stranded(&w.rq), 0);
	feed(&w.rq, &w.io, "x\n" PROMPT);
	chk_int("D12: finished once", ch.finished_calls, 1);
	chk_int("D12: well", ch.finished_ok, 1);
	chk_int("D12: no notice at all", w.f.nnotice, 0);
	e_d_req_free(&w.rq);
}

static int step_cmd_block_then_stop(char *cmd, size_t n, void *ctx)
{
	CHAIN *c = ctx;

	if (c->watches_sent >= 1)
		return E_D_STEP_CMD_STOP;
	c->watches_sent++;
	snprintf(cmd, n, "commands 1");
	return 1;
}

static const E_D_STEP block_open_steps[] = {
	{ step_cmd_block_then_stop, step_answered_again, step_failed_prompt },
	{ step_cmd_never,           step_answered_next },
	{ NULL, NULL }
};

static void test_a_file_that_ends_inside_a_block_stops_the_chain(void)
{
	static WORLD w;
	CHAIN ch;

	/* D13 */
	world_make(&w, "pdb");
	memset(&ch, 0, sizeof ch);
	prompt_verdict = E_D_STEP_AGAIN;
	e_d_chain_run(&w.via, block_open_steps, &ch, chain_finished);
	feed(&w.rq, &w.io, "(com) ");
	advance(&w.f, &w.rq, &w.io, E_D_REQ_PROMPT_MS);
	chk_int("D13: the owner heard the builder's stop too", ch.failed_calls, 2);
	chk_int("D13: as another prompt", ch.seen.how, E_D_REQ_OTHER_PROMPT);
	chk_int("D13: the chain ended not ok", ch.finished_ok, 0);
	chk_int("D13: finished once", ch.finished_calls, 1);
	chk_str("D13: with the last command", ch.last_cmd, "commands 1");
	chk_str("D13: and the prompt", ch.last.prompt, "(com)");
	chk_int("D13: no later step ran", ch.never_ran, 0);
	chk_int("D13: nothing more was sent", w.f.nsent, 1);
	e_d_req_free(&w.rq);
}

int main(void)
{
	test_request_deadline_fires_without_an_answer();
	test_requests_go_out_one_at_a_time_and_the_queue_refuses_past_its_bound();
	test_request_cancel_empties_the_queue_and_names_the_command();
	test_the_length_of_a_command_is_the_owners();
	test_a_long_command_runs_a_whole_request();
	test_async_line_during_a_request_goes_to_the_session();
	test_chain_runs_steps_in_order_and_ends_on_cancel();
	test_a_step_is_sent_whole_or_not_at_all();
	test_a_refused_step_reaches_its_owner_and_the_chain_goes_on();
	test_a_refused_step_nobody_took_is_said_once();
	test_sixty_refused_steps_run_in_one_frame();
	test_the_engine_says_a_deadline_once_and_the_chain_not_at_all();
	test_a_chain_answered_throughout_is_quiet();
	test_a_refusal_that_passes_is_not_a_final_mark();
	test_a_builder_that_cannot_go_on_ends_the_chain();
	test_a_chain_that_cannot_start_hands_the_unstarted_record();
	test_a_stopped_chain_whose_owner_says_it_is_said_once();
	test_the_words_of_an_end_are_written_in_one_place();
	test_a_long_refusal_keeps_its_cause();
	test_a_stranded_debugger_refuses_every_sender();
	test_request_engine_edges();
	test_error_line_before_the_prompt_stays_with_its_answer();
	test_resync_waits_for_its_own_numbered_echo();
	test_another_prompt_is_told_by_the_quiet_timer_and_strands();
	test_each_other_prompt_of_a_table_is_told();
	test_the_error_stream_is_kept_apart_in_the_answer();
	test_program_output_that_looks_like_a_prompt_is_not_one();
	test_mi_console_record_then_its_result_is_one_answer();
	test_a_step_may_take_the_prompt_of_a_block();
	test_a_file_that_ends_inside_a_block_stops_the_chain();
	if (fails) {
		printf("test_debreq: %d check(s) failed\n", fails);
		return 1;
	}
	printf("test_debreq: all checks passed\n");
	return 0;
}
