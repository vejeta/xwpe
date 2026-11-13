/* we_debreq.c -- the request engine: one command in flight to the debugger,
 * the rest waiting behind it, a deadline, cancel, and chains of steps.
 *
 * Plain C: no curses, no X11, no Wayland, no editor header, no descriptor.
 * The engine reads the clock, writes to the debugger and shows words only
 * through the functions of E_D_REQ_IO, so tests/test_debreq.c drives it with
 * a fake world and no time passes.
 *
 * Why it is shaped as it is: the editor must keep taking keys while the
 * debugger thinks, so nothing here waits.  Bytes arrive in e_d_req_feed from
 * the loop's descriptor callback, time arrives in e_d_req_timer from the
 * loop's timers, and every request ends in exactly one call of its owner.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "we_debreq.h"

#define GROW_MIN        256       /* first size of a text buffer */
#define ERRORS_READ_MAX 4096      /* error-stream bytes taken per ask */
#define NO_ELAPSED      0L
#define MS_PER_S        1000L
#define DIGIT_BASE      10
#define CANCEL_FMT      E_D_REQ_CANCEL_FMT
#define TIMEOUT_FMT     E_D_REQ_TIMEOUT_FMT
#define DEFAULT_NAME    "debugger"
#define ELLIPSIS        "..."
#define UTF8_CONT_MASK  0xC0      /* the top bits of a UTF-8 continuation byte */
#define UTF8_CONT_BITS  0x80

/* ---- text buffers ---- */

/**
 * buf_reserve - Make room for @need bytes in a text buffer that grows.
 * Return: 1 on success, 0 when memory ran out (the buffer is unchanged).
 */
static int buf_reserve(char **text, size_t *size, size_t need)
{
	size_t room = *size ? *size : GROW_MIN;
	char *p;

	if (need <= *size)
		return 1;
	while (room < need)
		room *= 2;
	p = realloc(*text, room);
	if (!p)
		return 0;
	*text = p;
	*size = room;
	return 1;
}

/**
 * answer_append_upto - Add bytes to an answer, up to @cap bytes in all.
 * The rest is dropped, so a debugger that never stops writing cannot take
 * the editor's memory before the deadline gives it up.
 */
static void answer_append_upto(E_D_ANSWER *a, const char *bytes, size_t n,
                               size_t cap)
{
	size_t room = a->len < cap ? cap - a->len : 0;

	if (n > room)
		n = room;
	if (n == 0 || !buf_reserve(&a->text, &a->size, a->len + n + 1))
		return;
	memcpy(a->text + a->len, bytes, n);
	a->len += n;
	a->text[a->len] = '\0';
}

/** answer_append - Add whole lines to an answer, up to what it may keep. */
static void answer_append(E_D_ANSWER *a, const char *bytes, size_t n)
{
	answer_append_upto(a, bytes, n, E_D_REQ_ANSWER_MAX);
}

/**
 * answer_append_unfinished - Add the line that has no end yet to an answer
 * only to look at it.  It may pass the answer's bound by one line, so the
 * prompt that ends an answer which filled its bound is still seen and the
 * request ends when the debugger is done, not at the deadline.
 */
static void answer_append_unfinished(E_D_ANSWER *a, const char *bytes, size_t n)
{
	answer_append_upto(a, bytes, n, E_D_REQ_ANSWER_MAX + E_D_REQ_LINE_MAX);
}

/** answer_clear - Forget an answer's text and flags. */
static void answer_clear(E_D_ANSWER *a)
{
	free(a->text);
	memset(a, 0, sizeof *a);
}

/**
 * answer_drop_prompt - Cut the debugger's prompt off the end of an answer.
 * An answer of nothing but a prompt has no text at all.
 */
static void answer_drop_prompt(E_D_ANSWER *a, size_t prompt_len)
{
	if (!a->text)
		return;
	a->len = prompt_len < a->len ? a->len - prompt_len : 0;
	a->text[a->len] = '\0';
	if (a->len == 0) {
		free(a->text);
		a->text = NULL;
		a->size = 0;
	}
}

/** tail_clear - The line that had no end yet is no more. */
static void tail_clear(E_D_REQ_QUEUE *rq)
{
	rq->tail_len = 0;
	if (rq->tail)
		rq->tail[0] = '\0';
}

/** tail_text - The line that has no end yet, "" before any byte came. */
static const char *tail_text(const E_D_REQ_QUEUE *rq)
{
	return rq->tail ? rq->tail : "";
}

/** tail_add - Add one byte to the line that has no end yet. */
static void tail_add(E_D_REQ_QUEUE *rq, char c)
{
	if (!buf_reserve(&rq->tail, &rq->tail_size, rq->tail_len + 2))
		return;
	rq->tail[rq->tail_len++] = c;
	rq->tail[rq->tail_len] = '\0';
}

/** tail_move_to_answer - The unfinished line joins the answer as it is. */
static void tail_move_to_answer(E_D_REQ_QUEUE *rq)
{
	answer_append(&rq->ans, rq->tail, rq->tail_len);
	tail_clear(rq);
}

/* ---- the queue ---- */

static E_D_REQ *queue_head(E_D_REQ_QUEUE *rq)
{
	return &rq->q[rq->head];
}

static E_D_REQ *queue_newest(E_D_REQ_QUEUE *rq)
{
	return &rq->q[(rq->head + rq->n - 1) % E_D_REQ_QUEUE_MAX];
}

static void queue_push(E_D_REQ_QUEUE *rq, const char *cmd,
                       e_d_answered_fn answered, void *ctx)
{
	E_D_REQ *r = &rq->q[(rq->head + rq->n) % E_D_REQ_QUEUE_MAX];

	memset(r, 0, sizeof *r);
	snprintf(r->cmd, sizeof r->cmd, "%s", cmd);
	r->answered = answered;
	r->ctx = ctx;
	rq->n++;
}

static void queue_pop(E_D_REQ_QUEUE *rq)
{
	rq->head = (rq->head + 1) % E_D_REQ_QUEUE_MAX;
	rq->n--;
}

/* ---- the world, with the optional parts optional ---- */

static void world_arm(const E_D_REQ_IO *io, int which, long ms)
{
	if (io->arm_timer)
		io->arm_timer(which, ms, io->io);
}

static void world_disarm_all(const E_D_REQ_IO *io)
{
	int which;

	for (which = 0; which < E_D_TIMERS; which++)
		world_arm(io, which, E_D_TIMER_OFF);
}

static void world_notice(const E_D_REQ_IO *io, const char *msg)
{
	if (io->notice)
		io->notice(msg, io->io);
}

static const char *world_name(const E_D_REQ_IO *io)
{
	return io->name ? io->name : DEFAULT_NAME;
}

static int world_can_settle(const E_D_REQ_IO *io)
{
	return io->settle_cmd && io->settle_marker;
}

static int world_claims_line(const E_D_REQ_IO *io, const char *line)
{
	return io->line_is_async && io->line_is_async(line, io->io);
}

/* ---- the deadline ---- */

int e_d_req_timeout_from_text(const char *text)
{
	long secs = 0;

	if (!text || !*text)
		return E_D_REQ_TIMEOUT_S;
	for (; *text; text++) {
		if (*text < '0' || *text > '9')
			return E_D_REQ_TIMEOUT_S;
		secs = secs * DIGIT_BASE + (*text - '0');
		if (secs > E_D_REQ_TIMEOUT_MAX_S)
			return E_D_REQ_TIMEOUT_MAX_S;
	}
	return secs > 0 ? (int)secs : E_D_REQ_TIMEOUT_S;
}

void e_d_req_init(E_D_REQ_QUEUE *rq, const char *timeout_text)
{
	memset(rq, 0, sizeof *rq);
	rq->deadline_s = e_d_req_timeout_from_text(timeout_text);
}

void e_d_req_free(E_D_REQ_QUEUE *rq)
{
	answer_clear(&rq->ans);
	free(rq->tail);
	memset(rq, 0, sizeof *rq);
}

int e_d_req_pending(const E_D_REQ_QUEUE *rq)
{
	return rq->n > 0 || rq->resync;
}

void e_d_req_busy_message(const E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                          char *out, size_t n)
{
	const char *cmd;

	if (n == 0)
		return;
	out[0] = '\0';
	if (rq->stranded) {
		snprintf(out, n, E_D_REQ_AT_PROMPT_FMT E_D_REQ_QUIT_TAIL,
		         world_name(io), rq->owed_cmd);
		return;
	}
	if (rq->resync)
		cmd = rq->owed_cmd;
	else if (rq->n > 0)
		cmd = rq->q[rq->head].cmd;
	else
		return;
	snprintf(out, n, E_D_REQ_BUSY_FMT E_D_REQ_QUIT_TAIL, world_name(io), cmd);
}

int e_d_req_stranded(const E_D_REQ_QUEUE *rq)
{
	return rq->stranded;
}

/* ---- the error stream at a prompt ---- */

/**
 * answer_join_waiting_errors - Add to an answer what the debugger had
 * already written on its error stream when its prompt was read.
 * @a: the answer, its prompt already cut; @io: the world.
 *
 * gdb, sdb, dbx and xdb write a refusal on their error stream and only
 * then their prompt on their output, from one thread, so once the prompt
 * has been read every byte of the refusal is already waiting: taking it
 * now, without waiting, keeps "No source file named q.c." with the
 * breakpoint it refuses, however the editor's loop ordered the two wakes
 * (a busy editor finds both ready at once and may read the prompt first).
 */
static void answer_join_waiting_errors(E_D_ANSWER *a, const E_D_REQ_IO *io)
{
	char buf[ERRORS_READ_MAX];
	size_t n, taken = 0;

	if (!io->errors_waiting)
		return;
	while (taken < E_D_REQ_ANSWER_MAX &&
	       (n = io->errors_waiting(buf, sizeof buf, io->io)) > 0) {
		answer_append(a, buf, n);
		a->error = 1;
		taken += n;
	}
}

/**
 * resync_drop_waiting_errors - The old answer is over: what the debugger
 * wrote on its error stream for it is dropped with it, so the user never
 * reads a refusal of a command given up on as the next command's.
 * @io: the world.
 */
static void resync_drop_waiting_errors(const E_D_REQ_IO *io)
{
	E_D_ANSWER old = { NULL, 0, 0, 0 };

	answer_join_waiting_errors(&old, io);
	answer_clear(&old);
}

/* ---- sending ---- */

/**
 * request_start - Put the command of the head of the queue on the wire and
 * start its clocks.
 * Return: 1 when it went out, 0 when the transport refused it.
 */
static int request_start(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	if (io->send(queue_head(rq)->cmd, io->io) != 0)
		return 0;
	rq->inflight = 1;
	rq->waiting_shown = 0;
	rq->late_fn = NULL;             /* the last request's hook may be freed now */
	rq->late_ctx = NULL;
	rq->sent_ms = io->now_ms(io->io);
	answer_clear(&rq->ans);
	tail_clear(rq);
	world_arm(io, E_D_TIMER_QUIET, E_D_REQ_QUIET_MS);
	world_arm(io, E_D_TIMER_DEADLINE, (long)rq->deadline_s * MS_PER_S);
	return 1;
}

/**
 * request_end_head - End the request in flight: take it off the queue and
 * tell its owner how it ended, with the answer that came.
 * @prompt_len: bytes of prompt to cut from the answer.
 *
 * The request is off the queue and the engine is idle before the owner is
 * called, so an owner that sends the next command of its chain from here
 * finds an engine that takes it at once.
 */
static void request_end_head(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                             E_D_REQ_END how, size_t prompt_len)
{
	const E_D_REQ *head = queue_head(rq);
	e_d_answered_fn answered = head->answered;
	void *ctx = head->ctx;
	E_D_ANSWER ans = rq->ans;

	memset(&rq->ans, 0, sizeof rq->ans);
	tail_clear(rq);
	rq->late_fn = head->late_fn;        /* the command's text is not needed */
	rq->late_ctx = head->late_ctx;
	queue_pop(rq);
	rq->inflight = 0;
	rq->waiting_shown = 0;
	world_disarm_all(io);
	answer_drop_prompt(&ans, prompt_len);
	if (how == E_D_REQ_DONE)
		answer_join_waiting_errors(&ans, io);
	if (answered)
		answered(how, &ans, ctx);
	answer_clear(&ans);
}

/**
 * queue_dispatch - Send the command at the head when nothing is in flight.
 * A command the transport refuses ends with E_D_REQ_GONE, so its owner is
 * told and the next one is tried.
 */
static void queue_dispatch(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	while (rq->n > 0 && !rq->inflight && !rq->closing && !rq->gone) {
		if (request_start(rq, io))
			return;
		request_end_head(rq, io, E_D_REQ_GONE, 0);
	}
}

/* How long a command may be to each debugger the editor measured, and to the
   ones it did not.  A debugger that is in neither list, or no debugger at
   all (the build server, the language server), keeps the unmeasured length.
   gdb, pdb and jdb: measured over a pipe, see we_debreq.h.  lldb: measured
   the same way; the editor reaches it through its adapter, never this
   engine, so the row only says what was measured.  sdb, xdb, dbx and a68g
   have no host here: their row says so and keeps what they always had. */
typedef struct
{
	const char *debugger;
	size_t max;                     /* bytes with the terminator */
} CMD_LIMIT;

static const CMD_LIMIT cmd_limits[] = {
	{ "gdb",  E_D_REQ_CMD_MAX },
	{ "pdb",  E_D_REQ_CMD_MAX },
	{ "jdb",  E_D_REQ_CMD_MAX },
	{ "lldb", E_D_REQ_CMD_MAX },
	{ "sdb",  E_D_REQ_CMD_UNMEASURED_MAX },
	{ "xdb",  E_D_REQ_CMD_UNMEASURED_MAX },
	{ "dbx",  E_D_REQ_CMD_UNMEASURED_MAX },
	{ "a68g", E_D_REQ_CMD_UNMEASURED_MAX },
	{ NULL,   E_D_REQ_CMD_UNMEASURED_MAX }
};

/**
 * cmd_limit_of - How many bytes a command to this debugger may take.
 * @debugger: the debugger's name, or NULL.
 * Return: the row's length; the unmeasured one for a name not in the table.
 */
static size_t cmd_limit_of(const char *debugger)
{
	const CMD_LIMIT *row;

	for (row = cmd_limits; row->debugger; row++)
		if (debugger && strcmp(row->debugger, debugger) == 0)
			return row->max;
	return row->max;
}

int e_d_req_cmd_fits(const char *debugger, const char *cmd)
{
	return strlen(cmd) < cmd_limit_of(debugger);
}

/** send_refusal - Why a command cannot be queued now, or E_D_SEND_OK. */
static int send_refusal(const E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                        const char *cmd)
{
	if (rq->gone || rq->closing)
		return E_D_SEND_GONE;
	if (rq->stranded)
		return E_D_SEND_STRANDED;
	if (rq->resync)
		return E_D_SEND_RESYNC;
	if (!e_d_req_cmd_fits(io->name, cmd))
		return E_D_SEND_TOO_LONG;
	if (rq->n >= E_D_REQ_QUEUE_MAX)
		return E_D_SEND_FULL;
	return E_D_SEND_OK;
}

int e_d_req_send(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, const char *cmd,
                 e_d_answered_fn answered, void *ctx)
{
	int refused = send_refusal(rq, io, cmd);

	if (refused != E_D_SEND_OK)
		return refused;
	queue_push(rq, cmd, answered, ctx);
	if (rq->n == 1 && !rq->inflight && !request_start(rq, io)) {
		queue_pop(rq);
		return E_D_SEND_FAILED;
	}
	queue_dispatch(rq, io);
	return E_D_SEND_OK;
}

void e_d_req_set_late_error(E_D_REQ_QUEUE *rq, e_d_late_error_fn fn, void *ctx)
{
	if (rq->n > 0) {
		queue_newest(rq)->late_fn = fn;
		queue_newest(rq)->late_ctx = ctx;
	} else {
		rq->late_fn = fn;
		rq->late_ctx = ctx;
	}
}

/* ---- reading ---- */

/**
 * answer_is_whole - Ask the debugger's reader whether the answer, with the
 * line that has no end yet counted, is complete.
 * @prompt_len: the prompt's bytes when it is.
 *
 * The unfinished line is where a prompt is, since a prompt has no line end.
 */
static int answer_is_whole(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                           size_t *prompt_len)
{
	size_t keep = rq->ans.len;
	int whole;

	answer_append_unfinished(&rq->ans, rq->tail, rq->tail_len);
	*prompt_len = 0;
	whole = io->answer_ended(&rq->ans, prompt_len, io->io);
	if (whole) {
		tail_clear(rq);
		return 1;
	}
	rq->ans.len = keep;
	if (rq->ans.text)
		rq->ans.text[keep] = '\0';
	return 0;
}

const char *e_d_req_marker_in(const char *text, const char *marker)
{
	size_t n;
	const char *at = text;

	if (!text || !marker || !*marker)
		return NULL;
	n = strlen(marker);
	while ((at = strstr(at, marker)) != NULL) {
		if (!isdigit((unsigned char)at[n]))
			return at;
		at += n;
	}
	return NULL;
}

/**
 * resync_marker - The echo the resync waits for: its own numbered marker,
 * or the debugger's plain settle marker.
 */
static const char *resync_marker(const E_D_REQ_QUEUE *rq,
                                 const E_D_REQ_IO *io)
{
	if (rq->resync_numbered)
		return rq->resync_marker;
	return io->settle_marker;
}

/**
 * resync_marker_at - Where the resync's echo first stands in @text, or NULL.
 *
 * A numbered echo counts only with its whole number, so the echo of a
 * settle command the user gave up on, which holds the same words with an
 * earlier number, never ends the resync before the debugger is in step.
 */
static const char *resync_marker_at(const E_D_REQ_QUEUE *rq,
                                    const E_D_REQ_IO *io, const char *text)
{
	if (rq->resync_numbered)
		return e_d_req_marker_in(text, rq->resync_marker);
	return strstr(text, io->settle_marker);
}

/**
 * text_after_last - What follows the resync's last echo in the answer and
 * the unfinished line, as an answer to hand to the reader; the text is a
 * copy the caller frees through the returned answer.
 * Return: 1 when the echo is there.
 */
static int text_after_last(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                           E_D_ANSWER *out)
{
	size_t mlen = strlen(resync_marker(rq, io));
	size_t keep = rq->ans.len;
	const char *p, *last = NULL;

	answer_append_unfinished(&rq->ans, rq->tail, rq->tail_len);
	if (rq->ans.text) {
		for (p = resync_marker_at(rq, io, rq->ans.text); p;
		     p = resync_marker_at(rq, io, p + 1))
			last = p;
	}
	memset(out, 0, sizeof *out);
	if (last)
		answer_append(out, last + mlen, strlen(last + mlen));
	rq->ans.len = keep;
	if (rq->ans.text)
		rq->ans.text[keep] = '\0';
	return last != NULL;
}

/**
 * resync_is_over - Has the debugger echoed the settle marker and given its
 * prompt after it?  Everything before that is the old answer, dropped.
 */
static int resync_is_over(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	E_D_ANSWER after;
	size_t prompt_len = 0;
	int over = 0;

	if (text_after_last(rq, io, &after))
		over = io->answer_ended(&after, &prompt_len, io->io);
	answer_clear(&after);
	return over;
}

/**
 * resync_drop_old_lines - Forget the lines of the old answer as they come,
 * until the settle marker is among them.  An old answer longer than an
 * answer's bound would otherwise push the marker past it, and the debugger
 * would stay "still answering" until the user quits.
 */
static void resync_drop_old_lines(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	if (!rq->ans.text || !resync_marker_at(rq, io, rq->ans.text))
		answer_clear(&rq->ans);
}

/** resync_finish - The debugger is back in step: the next command may go. */
static void resync_finish(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	rq->resync = 0;
	rq->owed_cmd[0] = '\0';
	answer_clear(&rq->ans);
	tail_clear(rq);
	resync_drop_waiting_errors(io);
}

/**
 * line_complete - One whole line has come: an editor line goes to the
 * editor, anything else is part of the answer.
 * Return: 1 when the line joined the answer, 0 when the editor took it.
 */
static int line_complete(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	if (world_claims_line(io, tail_text(rq))) {
		if (io->deliver_async)
			io->deliver_async(tail_text(rq), io->io);
		tail_clear(rq);
		return 0;
	}
	tail_add(rq, '\n');
	tail_move_to_answer(rq);
	return 1;
}

/**
 * take_line_end - Read the lines the byte just added to the tail completed,
 * and say whether the engine is done with the current request or resync.
 * Return: 1 when the request (or the old answer) ended, 0 when it goes on.
 */
static int take_line_end(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	size_t prompt_len;

	if (!line_complete(rq, io))
		return 0;                           /* the answer is unchanged */
	if (rq->resync) {
		if (!resync_is_over(rq, io)) {
			resync_drop_old_lines(rq, io);
			return 0;
		}
		resync_finish(rq, io);
		return 1;
	}
	if (!answer_is_whole(rq, io, &prompt_len))
		return 0;
	request_end_head(rq, io, E_D_REQ_DONE, prompt_len);
	queue_dispatch(rq, io);
	return 1;
}

/**
 * take_unfinished - After the last byte of a read: the line with no end may
 * be the prompt that ends the answer.
 */
static void take_unfinished(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	size_t prompt_len;

	if (rq->tail_len == 0)
		return;
	if (rq->resync) {
		if (resync_is_over(rq, io))
			resync_finish(rq, io);
		return;
	}
	if (!answer_is_whole(rq, io, &prompt_len))
		return;
	request_end_head(rq, io, E_D_REQ_DONE, prompt_len);
	queue_dispatch(rq, io);
}

static int engine_owns_output(const E_D_REQ_QUEUE *rq)
{
	return !rq->gone && (rq->inflight || rq->resync);
}

size_t e_d_req_feed(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, const char *bytes,
                    size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (!engine_owns_output(rq))
			return i;
		if (bytes[i] == '\n') {
			take_line_end(rq, io);          /* the line end is not text */
			continue;
		}
		tail_add(rq, bytes[i]);
		if (rq->tail_len >= E_D_REQ_LINE_MAX)
			tail_move_to_answer(rq);
	}
	if (engine_owns_output(rq))
		take_unfinished(rq, io);
	return n;
}

void e_d_req_feed_errors(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                         const char *bytes, size_t n)
{
	E_D_ANSWER err;

	if (rq->gone || rq->resync || n == 0)
		return;
	if (rq->inflight) {
		answer_append(&rq->ans, bytes, n);
		rq->ans.error = 1;
		return;
	}
	if (!rq->late_fn)
		return;
	memset(&err, 0, sizeof err);
	answer_append(&err, bytes, n);
	err.error = 1;
	rq->late_fn(&err, rq->late_ctx);
	answer_clear(&err);
	(void)io;
}

/* ---- timers ---- */

static long elapsed_s(const E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	long ms = io->now_ms(io->io) - rq->sent_ms;

	return ms > 0 ? ms / MS_PER_S : NO_ELAPSED;
}

/** waiting_show - Show the user what the editor is waiting for, and count. */
static void waiting_show(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	if (io->waiting)
		io->waiting(queue_head(rq)->cmd, elapsed_s(rq, io), io->io);
	rq->waiting_shown = 1;
	world_arm(io, E_D_TIMER_SECOND, E_D_REQ_SECOND_MS);
}

void e_d_req_timer(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, int which)
{
	if (!rq->inflight || rq->gone)
		return;
	if (which == E_D_TIMER_QUIET)
		waiting_show(rq, io);
	else if (which == E_D_TIMER_SECOND && rq->waiting_shown)
		waiting_show(rq, io);
	else if (which == E_D_TIMER_DEADLINE)
		e_d_req_cancel(rq, io, E_D_REQ_TIMED_OUT);
}

/* ---- cancel ---- */

/**
 * cancel_say - The sentence the status row shows for the request given up.
 * A deadline names the wait and how to leave; a cancel names the command.
 */
static void cancel_say(const E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                       E_D_REQ_END why, const char *cmd)
{
	char msg[E_D_REQ_MSG_MAX];

	if (why == E_D_REQ_TIMED_OUT)
		snprintf(msg, sizeof msg, TIMEOUT_FMT, world_name(io), cmd,
		         rq->deadline_s);
	else if (why == E_D_REQ_CANCELLED)
		snprintf(msg, sizeof msg, CANCEL_FMT, world_name(io), cmd);
	else
		return;
	world_notice(io, msg);
}

/**
 * resync_settle_command - The settle command a resync sends, and the echo
 * it then waits for.
 * @rq, @io: the engine and its world; @cmd, @n: room for a numbered one.
 *
 * A debugger whose settle commands are numbered may still owe the echo of
 * one the user gave up on, in the same words as the plain marker: the
 * resync then asks for an echo of its own, numbered past those, so the
 * user's next command is answered in step and not by a settle echo.
 * Return: the command to send.
 */
static const char *resync_settle_command(E_D_REQ_QUEUE *rq,
                                         const E_D_REQ_IO *io,
                                         char *cmd, size_t n)
{
	if (!io->resync_settle) {
		rq->resync_numbered = 0;
		return io->settle_cmd;
	}
	io->resync_settle(cmd, n, rq->resync_marker, sizeof rq->resync_marker,
	                  io->io);
	rq->resync_numbered = 1;
	return cmd;
}

/**
 * resync_begin - A debugger still owes the answer to a command given up:
 * make it echo the settle marker, and drop everything up to it.
 * A transport that cannot even take the settle command ends the session.
 */
static void resync_begin(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io)
{
	char cmd[E_D_REQ_CMD_MAX];
	const char *settle = resync_settle_command(rq, io, cmd, sizeof cmd);

	if (io->send(settle, io->io) != 0) {
		rq->gone = 1;
		return;
	}
	rq->resync = 1;
	answer_clear(&rq->ans);
	tail_clear(rq);
}

/* What a cancel keeps of each request: who to tell, not the command, so the
   copy on the stack is two pointers per request and not a command's room. */
typedef struct
{
	e_d_answered_fn answered;
	void *ctx;
} CANCELLED;

void e_d_req_cancel(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, E_D_REQ_END why)
{
	CANCELLED list[E_D_REQ_QUEUE_MAX];
	E_D_ANSWER first = { NULL, 0, 0, 0 };
	E_D_ANSWER empty = { NULL, 0, 0, 0 };
	int count = rq->n, was_inflight = rq->inflight, i;

	if (why == E_D_REQ_GONE) {
		rq->gone = 1;
		rq->resync = 0;
		rq->stranded = 0;
	}
	if (count == 0)
		return;
	for (i = 0; i < count; i++) {
		const E_D_REQ *r = &rq->q[(rq->head + i) % E_D_REQ_QUEUE_MAX];

		list[i].answered = r->answered;
		list[i].ctx = r->ctx;
	}
	if (was_inflight) {
		const char *head_cmd = queue_head(rq)->cmd;

		snprintf(rq->owed_cmd, sizeof rq->owed_cmd, "%s", head_cmd);
		cancel_say(rq, io, why, head_cmd);
		tail_move_to_answer(rq);        /* the owner sees all that came */
		first = rq->ans;
		memset(&rq->ans, 0, sizeof rq->ans);
	}
	world_disarm_all(io);
	rq->head = rq->n = rq->inflight = rq->waiting_shown = 0;
	rq->late_fn = NULL;
	tail_clear(rq);
	rq->closing = 1;
	if (was_inflight && why != E_D_REQ_GONE && world_can_settle(io))
		resync_begin(rq, io);
	for (i = 0; i < count; i++)
		if (list[i].answered)
			list[i].answered(why, i == 0 ? &first : &empty, list[i].ctx);
	rq->closing = 0;
	answer_clear(&first);
}

/* ---- chains ---- */

int e_d_req_request(void *engine, const char *cmd, e_d_answered_fn answered,
                    void *ctx)
{
	E_D_REQ_LINK *link = engine;

	return e_d_req_send(link->rq, link->io, cmd, answered, ctx);
}

typedef struct
{
	E_D_REQUESTER via;
	const E_D_STEP *steps;
	void *ctx;
	e_d_chain_done_fn finished;
	int idx;
	char last_cmd[E_D_REQ_CMD_MAX + 1];   /* the command of the step in flight */
} E_D_CHAIN;

#define CHAIN_FAILED   0
#define CHAIN_RUNNING  1
#define CHAIN_ENDED    2
#define WHERE_SHOWN    128                /* bytes of a file name a sentence shows */
#define CMD_TEXT_MAX   (E_D_REQ_CMD_SHOWN + sizeof ELLIPSIS + WHERE_SHOWN + 16)
#define NO_PROMPT      ""

static int chain_step_is_end(const E_D_STEP *s)
{
	return !s->command && !s->answered && !s->failed;
}

/* ---- how a step ended, and the words that say it ---- */

/**
 * cmd_cut_shown - The longest start of @cmd a sentence shows, ending where a
 * character does, so a name written in UTF-8 is never cut in the middle.
 * Return: the bytes to show.
 */
static size_t cmd_cut_shown(const char *cmd)
{
	size_t n = strlen(cmd);

	if (n <= E_D_REQ_CMD_SHOWN)
		return n;
	n = E_D_REQ_CMD_SHOWN;
	while (n > 0 && ((unsigned char)cmd[n] & UTF8_CONT_MASK) == UTF8_CONT_BITS)
		n--;
	return n;
}

/**
 * cmd_text - How a sentence names a command: in quotes, cut to its first
 * E_D_REQ_CMD_SHOWN bytes and "..." when it is longer, and the file it is a
 * line of in brackets when it has one.
 * @out, @n: the text.
 */
static void cmd_text(const E_D_STEP_END *end, char *out, size_t n)
{
	const char *cmd = end->cmd ? end->cmd : "";
	size_t shown = cmd_cut_shown(cmd);
	const char *more = cmd[shown] ? ELLIPSIS : "";

	if (end->where && end->where[0])
		snprintf(out, n, "`%.*s%s` (%.*s)", (int)shown, cmd, more,
		         WHERE_SHOWN, end->where);
	else
		snprintf(out, n, "`%.*s%s`", (int)shown, cmd, more);
}

/**
 * send_reason - Why the engine did not take a command.
 * @code: E_D_SEND_*; @name: the debugger's; @out, @n: the reason alone.
 */
static void send_reason(int code, const char *name, char *out, size_t n)
{
	switch (code) {
	case E_D_SEND_TOO_LONG:
		snprintf(out, n, E_D_REQ_TOO_LONG_REASON, name);
		break;
	case E_D_SEND_FULL:
		snprintf(out, n, "%s", E_D_REQ_FULL_REASON);
		break;
	case E_D_SEND_RESYNC:
		snprintf(out, n, E_D_REQ_RESYNC_REASON, name);
		break;
	case E_D_SEND_GONE:
		snprintf(out, n, "%s", E_D_REQ_GONE_REASON);
		break;
	case E_D_SEND_FAILED:
		snprintf(out, n, E_D_REQ_FAILED_REASON, name);
		break;
	case E_D_SEND_STRANDED:
		snprintf(out, n, E_D_REQ_STRANDED_REASON, name);
		break;
	default:
		snprintf(out, n, "%s", E_D_REQ_UNKNOWN_REASON);
		break;
	}
}

void e_d_req_end_words(E_D_STEP_END *end, const char *name)
{
	char cmdtxt[CMD_TEXT_MAX];

	if (!name)
		name = "the debugger";
	cmd_text(end, cmdtxt, sizeof cmdtxt);
	end->words[0] = '\0';
	if (end->how == E_D_REQ_NOT_SENT) {
		send_reason(end->code, name, end->reason, sizeof end->reason);
		snprintf(end->words, sizeof end->words, E_D_REQ_NOT_SENT_FMT, name,
		         cmdtxt, end->reason);
	} else if (end->how == E_D_REQ_OTHER_PROMPT) {
		end->reason[0] = '\0';
		snprintf(end->words, sizeof end->words, E_D_REQ_OTHER_PROMPT_FMT,
		         name, cmdtxt, end->prompt ? end->prompt : NO_PROMPT);
	} else if (end->how == E_D_REQ_REFUSED) {
		snprintf(end->words, sizeof end->words, E_D_REQ_REFUSED_FMT, name,
		         cmdtxt, end->reason);
	} else {
		end->reason[0] = '\0';   /* a request given up on is said by the engine */
	}
}

/**
 * step_end_make - The record of how the step of @ch ended.
 * @end: receives it; @how, @code: the end and, for a request not sent, why;
 * @ans: what came or NULL.
 *
 * The command is the chain's own copy, which lives as long as the chain, so
 * the record stays valid for the owner's @finished.
 */
static void step_end_make(const E_D_CHAIN *ch, E_D_STEP_END *end,
                          E_D_REQ_END how, int code, const E_D_ANSWER *ans)
{
	memset(end, 0, sizeof *end);
	end->how = how;
	end->code = code;
	end->cmd = ch->last_cmd;
	end->prompt = NO_PROMPT;
	end->ans = ans;
	e_d_req_end_words(end, ch->via.name);
}

void e_d_chain_end_unstarted(E_D_CHAIN_END *end)
{
	memset(end, 0, sizeof *end);
	end->last.how = E_D_REQ_NOT_SENT;
	end->last.code = E_D_SEND_GONE;
	end->last.cmd = "";
	end->last.prompt = NO_PROMPT;
}

void e_d_req_notice(const char *msg, void *engine)
{
	E_D_REQ_LINK *link = engine;

	world_notice(link->io, msg);
}

/**
 * chain_say_not_sent - Say, once, a step the chain could not give to the
 * debugger and that no step owner took.
 * @ch: the chain; @end: the record.
 * Return: 1 when a sentence was said (the requester has a @notice), else 0.
 */
static int chain_say_not_sent(const E_D_CHAIN *ch, const E_D_STEP_END *end)
{
	char msg[E_D_REQ_MSG_MAX];

	if (!ch->via.notice)
		return 0;
	snprintf(msg, sizeof msg, "%.*s%s",
	         (int)(sizeof msg - sizeof E_D_REQ_QUIT_TAIL), end->words,
	         E_D_REQ_QUIT_TAIL);
	ch->via.notice(msg, ch->via.engine);
	return 1;
}

/**
 * chain_end - The chain is over: tell its owner once, with the record, and
 * free it.
 * @ok: 1 when it ended well; @last: the record of the step that ended it, or
 * NULL when it ended well; @said: a sentence was said for @last.
 * Return: CHAIN_ENDED when @ok, else CHAIN_FAILED.
 *
 * The owner hears it before the chain is freed, since the record points at
 * the chain's copy of the command.
 */
static int chain_end(E_D_CHAIN *ch, int ok, const E_D_STEP_END *last, int said)
{
	e_d_chain_done_fn finished = ch->finished;
	E_D_CHAIN_END end;

	memset(&end, 0, sizeof end);
	end.ok = ok;
	end.said = said;
	if (last) {
		end.last = *last;
	} else {
		end.last.how = E_D_REQ_DONE;
		end.last.cmd = "";
		end.last.prompt = NO_PROMPT;
	}
	ch->finished = NULL;
	if (finished)
		finished(&end, ch->ctx);
	free(ch);
	return ok ? CHAIN_ENDED : CHAIN_FAILED;
}

static int chain_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx);

/**
 * chain_not_taken - A step's command did not go out: ask the step's owner
 * what it means, and say it when nobody took it.
 * @ch: the chain; @s: the step; @how, @code: E_D_REQ_NOT_SENT and the
 * engine's refusal, or E_D_REQ_OTHER_PROMPT with code 0 when the builder
 * said E_D_STEP_CMD_STOP.
 * Return: E_D_STEP_NEXT or E_D_STEP_AGAIN when the owner took it and the
 * chain goes on, E_D_STEP_STOP when the chain has ended.
 */
static int chain_not_taken(E_D_CHAIN *ch, const E_D_STEP *s, E_D_REQ_END how,
                           int code)
{
	E_D_STEP_END end;
	int verdict = E_D_STEP_STOP;

	step_end_make(ch, &end, how, code, NULL);
	if (s->failed)
		verdict = s->failed(&end, ch->ctx);
	if (how == E_D_REQ_OTHER_PROMPT) {
		/* The builder said the step cannot go on: whoever owns it has the
		   words (a box); with no owner the chain says them. */
		chain_end(ch, 0, &end, s->failed ? 0 : chain_say_not_sent(ch, &end));
		return E_D_STEP_STOP;
	}
	if (verdict != E_D_STEP_STOP)
		return verdict;
	chain_end(ch, 0, &end, chain_say_not_sent(ch, &end));
	return E_D_STEP_STOP;
}

/**
 * chain_send_step - Send the command of the step at the chain's position.
 * Return: CHAIN_RUNNING with a request out, or the chain's end.
 *
 * A step whose command the engine refuses is told to its owner; when the
 * owner takes it (a breakpoint marked refused) this loop goes on to the
 * next step in the same frame, so sixty refused breakpoints do not nest
 * sixty frames.
 */
static int chain_send_step(E_D_CHAIN *ch)
{
	/* One byte more than any command may take: a step that writes more is
	   cut to E_D_REQ_CMD_MAX bytes, which every debugger's limit refuses.
	   The step then ends as an E_D_SEND_TOO_LONG record, and a different,
	   shorter command is never sent in its place. */
	char cmd[E_D_REQ_CMD_MAX + 1];

	for (;;) {
		const E_D_STEP *s = &ch->steps[ch->idx];
		int built, sent, verdict;

		if (chain_step_is_end(s))
			return chain_end(ch, 1, NULL, 0);
		built = s->command ? s->command(cmd, sizeof cmd, ch->ctx)
		                   : E_D_STEP_CMD_NONE;
		if (built == E_D_STEP_CMD_STOP) {
			chain_not_taken(ch, s, E_D_REQ_OTHER_PROMPT, 0);
			return CHAIN_FAILED;
		}
		if (built == E_D_STEP_CMD_NONE) {
			ch->idx++;                      /* nothing to send: skipped */
			continue;
		}
		snprintf(ch->last_cmd, sizeof ch->last_cmd, "%s", cmd);
		sent = ch->via.request(ch->via.engine, cmd, chain_answered, ch);
		if (sent == E_D_SEND_OK)
			return CHAIN_RUNNING;
		verdict = chain_not_taken(ch, s, E_D_REQ_NOT_SENT, sent);
		if (verdict == E_D_STEP_STOP)
			return CHAIN_FAILED;
		if (verdict == E_D_STEP_NEXT)
			ch->idx++;
	}
}

/** chain_given_up - Did a request end because the user or the clock gave up? */
static int chain_given_up(E_D_REQ_END how)
{
	return how == E_D_REQ_TIMED_OUT || how == E_D_REQ_CANCELLED ||
	       how == E_D_REQ_GONE;
}

/** chain_engine_said - Did the engine say how this request ended? */
static int chain_engine_said(E_D_REQ_END how)
{
	return how == E_D_REQ_TIMED_OUT || how == E_D_REQ_CANCELLED;
}

/**
 * chain_answered - A step's request ended: read it, then go where it says.
 *
 * A request that did not end with the debugger's own prompt reaches its
 * step's @failed as a record.  The chain ends there when the owner says stop
 * and always when the request was given up on.
 */
static int chain_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
	E_D_CHAIN *ch = ctx;
	const E_D_STEP *s = &ch->steps[ch->idx];
	E_D_STEP_END end;
	int verdict;

	if (how != E_D_REQ_DONE) {
		step_end_make(ch, &end, how, how == E_D_REQ_NOT_SENT ? E_D_SEND_STRANDED : 0, ans);
		verdict = s->failed ? s->failed(&end, ch->ctx) : E_D_STEP_STOP;
		if (chain_given_up(how) || verdict == E_D_STEP_STOP) {
			chain_end(ch, 0, &end, chain_engine_said(how));
			return E_D_STEP_STOP;
		}
		if (verdict == E_D_STEP_NEXT)
			ch->idx++;
		chain_send_step(ch);
		return verdict;
	}
	verdict = s->answered ? s->answered(how, ans, ch->ctx) : E_D_STEP_NEXT;
	if (verdict == E_D_STEP_STOP) {
		chain_end(ch, 1, NULL, 0);
		return verdict;
	}
	if (verdict == E_D_STEP_NEXT)
		ch->idx++;
	chain_send_step(ch);
	return verdict;
}

int e_d_chain_run(const E_D_REQUESTER *via, const E_D_STEP *steps, void *ctx,
                  e_d_chain_done_fn finished)
{
	E_D_CHAIN *ch = calloc(1, sizeof *ch);

	if (!ch) {
		E_D_CHAIN_END end;

		e_d_chain_end_unstarted(&end);
		end.last.code = E_D_SEND_FAILED;
		if (finished)
			finished(&end, ctx);
		return 0;
	}
	ch->via = *via;
	ch->steps = steps;
	ch->ctx = ctx;
	ch->finished = finished;
	return chain_send_step(ch) != CHAIN_FAILED;
}
