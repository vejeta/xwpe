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
	int error;
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
	if (ncall_order < LOG_MAX)
		call_order[ncall_order++] = c->id;
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
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
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
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
	CALL c[E_D_REQ_QUEUE_MAX + 1];
	char cmd[E_D_REQ_CMD_MAX];
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
	chk_int("queue: command too long", e_d_req_send(&rq, &io,
	        "p aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	        on_answered, &c[1]), E_D_SEND_TOO_LONG);
	e_d_req_free(&rq);
}

/* ---- test 13 ---- */

static void test_request_cancel_empties_the_queue_and_names_the_command(void)
{
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
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
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
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

static void chain_finished(int ok, void *ctx)
{
	CHAIN *c = ctx;

	c->finished_calls++;
	c->finished_ok = ok;
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
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
	E_D_REQ_LINK link;
	E_D_REQUESTER via;
	CHAIN ch;
	int i;
	const char *want[] = { "flush", "watch 0", "watch 1", "watch 2", "last" };

	world_init(&f, &io, 1);
	e_d_req_init(&rq, NULL);
	link.rq = &rq;
	link.io = &io;
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
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
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
	FAKE f;
	E_D_REQ_IO io;
	E_D_REQ_QUEUE rq;
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

int main(void)
{
	test_request_deadline_fires_without_an_answer();
	test_requests_go_out_one_at_a_time_and_the_queue_refuses_past_its_bound();
	test_request_cancel_empties_the_queue_and_names_the_command();
	test_async_line_during_a_request_goes_to_the_session();
	test_chain_runs_steps_in_order_and_ends_on_cancel();
	test_request_engine_edges();
	test_error_line_before_the_prompt_stays_with_its_answer();
	if (fails) {
		printf("test_debreq: %d check(s) failed\n", fails);
		return 1;
	}
	printf("test_debreq: all checks passed\n");
	return 0;
}
