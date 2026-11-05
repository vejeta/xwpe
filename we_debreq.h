#ifndef WPE_DEBREQ_H
#define WPE_DEBREQ_H

/* A command to the debugger, and the answer it gives.

   The answer is one type for everyone who handles it: the request engine
   that collects it from the debugger's output, the session state
   (we_debsess.h) that reads what it means, and the editor while it still
   reads answers itself.  One shape means a reader written for the editor's
   line readers today keeps working unchanged when the answer comes from the
   engine instead.

   Plain C: no curses, no X11, no Wayland, no editor header. */

#include <stddef.h>

/* The debugger's answer to one command.  @text holds its lines, joined,
   '\0'-terminated, or is NULL when the debugger gave nothing before its
   prompt.  An owner that grows the text sets @size to the room it has; a
   reader handed text it does not own sees @size 0 and never grows or frees
   it.  @error is set when an error record or a line of the debugger's error
   stream was part of the answer. */
typedef struct
{
 char   *text;
 size_t  len, size;
 int     error;
} E_D_ANSWER;

/* ---- the request engine ----
   One command is in flight at a time; the ones typed meanwhile wait behind
   it.  The engine owns no descriptor, no clock and no screen: everything it
   needs from the world comes through E_D_REQ_IO, so tests/test_debreq.c
   drives it with a fake clock and a transcript. */

/* Sizes and times. */
#define E_D_REQ_CMD_MAX      256      /* one command */
#define E_D_REQ_QUEUE_MAX    8        /* in flight plus waiting; more is refused */
#define E_D_REQ_TIMEOUT_S    30       /* seconds before a silent debugger is given up on */
#define E_D_REQ_TIMEOUT_MAX_S 86400   /* the override is cut here */
#define E_D_REQ_QUIET_MS     1000     /* silence before the waiting line is shown */
#define E_D_REQ_SECOND_MS    1000     /* the waiting line counts seconds */
#define E_D_REQ_ANSWER_MAX   1048576  /* an answer keeps no more than this */
#define E_D_REQ_LINE_MAX     65536    /* a line without an end is cut here */
#define E_D_REQ_MSG_MAX      512      /* a notice for the status row */
#define E_D_REQ_NAME_MAX     32       /* a debugger's name in a notice */
#define E_D_REQ_TIMEOUT_ENV  "XWPE_DEBUG_ANSWER_TIMEOUT"

/* How a request ended. */
typedef enum
{
 E_D_REQ_DONE,        /* the debugger answered */
 E_D_REQ_TIMED_OUT,   /* it did not, within the deadline */
 E_D_REQ_CANCELLED,   /* the user cancelled */
 E_D_REQ_GONE         /* the debugger or the session is gone */
} E_D_REQ_END;

/* What e_d_req_send says. */
#define E_D_SEND_OK       0
#define E_D_SEND_FULL    (-1)   /* E_D_REQ_QUEUE_MAX requests are waiting */
#define E_D_SEND_RESYNC  (-2)   /* the debugger still owes an old answer */
#define E_D_SEND_GONE    (-3)   /* the session is over */
#define E_D_SEND_FAILED  (-4)   /* the transport would not take the command */
#define E_D_SEND_TOO_LONG (-5)  /* the command does not fit E_D_REQ_CMD_MAX */

/* The three timers the engine asks the loop for. */
#define E_D_TIMER_QUIET     0   /* shows the waiting line */
#define E_D_TIMER_DEADLINE  1   /* gives the request up */
#define E_D_TIMER_SECOND    2   /* counts the waiting line's seconds */
#define E_D_TIMERS          3
#define E_D_TIMER_OFF       (-1)

/* What a chain's step says after an answer. */
#define E_D_STEP_NEXT   0   /* go on to the next step of the chain */
#define E_D_STEP_AGAIN  1   /* run this step once more (the next watch) */
#define E_D_STEP_STOP   2   /* the chain ends here */

typedef int (*e_d_answered_fn)(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx);

/* Error-stream lines that come after their request ended. */
typedef void (*e_d_late_error_fn)(const E_D_ANSWER *err, void *ctx);

/* What the engine needs from the world.  @send writes a command and its
   line end (0 when it went out); @answer_ended says whether the text so far
   is the whole answer (the prompt is last) and, when it is, sets
   @prompt_len to the bytes of that prompt at the end of the text, which the
   answer does not keep.  It is asked at every line end and at the end of
   every read, so a prompt with no line end is seen when it is the last
   thing read; a debugger that writes more after its prompt in the same
   read (an event behind jdb's prompt) leaves that prompt inside the answer,
   which ends at the next prompt instead.  @line_is_async claims a line that
   is not part of any answer (a stop, a breakpoint record, under MI a result
   whose token is not the one in flight) and @deliver_async takes it;
   @now_ms reads the clock; @arm_timer asks the loop to call e_d_req_timer
   for @which in @ms ms, replacing any earlier timer of that @which,
   E_D_TIMER_OFF in @ms asking for no call; @waiting shows the waiting line;
   @notice shows a sentence on the status row.  @errors_waiting copies into
   @buf up to @n bytes the debugger has already written on its error stream,
   without waiting, and returns how many (0 when none): the engine asks it
   when a prompt ends an answer, since a debugger that refuses on its error
   stream writes the refusal before that prompt (NULL for a debugger whose
   refusals are on its output).  @settle_cmd (no line end)
   and @settle_marker are the command that makes the debugger echo
   @settle_marker after every answer it owes (NULL for a debugger that
   matches by token).  Only @send, @answer_ended and @now_ms are required. */
typedef struct
{
 int  (*send)(const char *cmd, void *io);
 int  (*answer_ended)(const E_D_ANSWER *ans, size_t *prompt_len, void *io);
 int  (*line_is_async)(const char *line, void *io);
 void (*deliver_async)(const char *line, void *io);
 long (*now_ms)(void *io);
 void (*arm_timer)(int which, long ms, void *io);
 void (*waiting)(const char *cmd, long secs, void *io);
 void (*notice)(const char *msg, void *io);
 size_t (*errors_waiting)(char *buf, size_t n, void *io);
 const char *name;           /* "gdb", for the notices */
 const char *settle_cmd;
 const char *settle_marker;
 void *io;
} E_D_REQ_IO;

typedef struct
{
 char cmd[E_D_REQ_CMD_MAX];
 e_d_answered_fn answered;
 void *ctx;
 e_d_late_error_fn late_fn;      /* set by e_d_req_set_late_error */
 void *late_ctx;
} E_D_REQ;

typedef struct
{
 E_D_REQ q[E_D_REQ_QUEUE_MAX];   /* q[head] is the one in flight */
 int head, n;
 int inflight;                   /* q[head] has been sent */
 int resync;                     /* an old answer is still being dropped */
 int closing;                    /* the queue is being ended */
 int gone;                       /* the session is over */
 int waiting_shown;
 E_D_ANSWER ans;                 /* what has come for the one in flight */
 char *tail;                     /* the line that has no end yet */
 size_t tail_len, tail_size;
 long sent_ms;
 int deadline_s;
 char owed_cmd[E_D_REQ_CMD_MAX]; /* the command whose answer is still owed */
 e_d_late_error_fn late_fn;      /* for the last request that ended */
 void *late_ctx;
} E_D_REQ_QUEUE;

/**
 * e_d_req_timeout_from_text - The seconds a debugger may stay silent.
 * @text: the value of E_D_REQ_TIMEOUT_ENV, or NULL.
 *
 * A user whose program computes for minutes before the debugger answers can
 * raise the wait; a value that is not a positive number leaves the default.
 * Return: seconds, E_D_REQ_TIMEOUT_S when @text is not usable.
 */
int e_d_req_timeout_from_text(const char *text);

/**
 * e_d_req_init - Start an engine with nothing in flight.
 * @rq: the engine; @timeout_text: the environment's value or NULL.
 */
void e_d_req_init(E_D_REQ_QUEUE *rq, const char *timeout_text);

/**
 * e_d_req_free - Release what the engine holds.  Requests still queued are
 * dropped without being called: end them first with e_d_req_cancel.
 */
void e_d_req_free(E_D_REQ_QUEUE *rq);

/**
 * e_d_req_send - Ask the debugger @cmd, to be told of the answer.
 * @rq, @io: the engine and its world; @cmd: the command, no line end;
 * @answered, @ctx: called when the request ends, however it ends.
 *
 * Commands typed while the debugger is answering go out one by one, in
 * order, so the user never sees one answer taken for another's.
 * Return: E_D_SEND_OK, or why it was refused (then @answered is not called).
 */
int e_d_req_send(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, const char *cmd,
                 e_d_answered_fn answered, void *ctx);

/**
 * e_d_req_set_late_error - Name who hears of error lines that come after
 * the request just sent has ended.
 * @rq: the engine; @fn, @ctx: the hook (NULL for none).
 *
 * A breakpoint the debugger refused in a line of its error stream, written
 * just after its prompt, turns hollow with the debugger's words instead of
 * staying solid.  Heard until the next request goes out, so @ctx must live
 * that long; then the engine forgets it.
 */
void e_d_req_set_late_error(E_D_REQ_QUEUE *rq, e_d_late_error_fn fn,
                            void *ctx);

/**
 * e_d_req_feed - Take bytes the debugger wrote on its output.
 * @rq, @io: the engine and its world; @bytes, @n: what was read.
 *
 * Lines the editor claims as its own (a stop) go to it, the rest build the
 * answer; when the answer is whole the request is called and the next one
 * goes out.  With nothing in flight the bytes are left alone.
 * Return: how many bytes the engine took; fewer than @n means nothing was
 * in flight for the rest, which the caller handles as unsolicited.
 */
size_t e_d_req_feed(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, const char *bytes,
                    size_t n);

/**
 * e_d_req_feed_errors - Take bytes the debugger wrote on its error stream.
 * @rq, @io: the engine and its world; @bytes, @n: what was read.
 *
 * The user is told why the debugger refused, in the answer it belongs to,
 * even when the words arrive after the prompt.  Words written before the
 * prompt that the editor had not read yet when it read the prompt are taken
 * with the answer through @io's errors_waiting, so they never reach here
 * for the next request.
 */
void e_d_req_feed_errors(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                         const char *bytes, size_t n);

/**
 * e_d_req_timer - A timer the engine armed has gone off.
 * @rq, @io: the engine and its world; @which: E_D_TIMER_*.
 *
 * After a quiet second the user sees what the editor waits for; past the
 * deadline the request is given up with a sentence saying how to leave.
 */
void e_d_req_timer(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, int which);

/**
 * e_d_req_cancel - End the request in flight and every one waiting.
 * @rq, @io: the engine and its world; @why: E_D_REQ_CANCELLED, _TIMED_OUT
 * or _GONE.
 *
 * Esc gives the user back the editor at once.  Every request is called with
 * @why, so none leaks.  A debugger that still owes an answer is made to
 * settle before the next command; E_D_REQ_GONE ends the session for good.
 */
void e_d_req_cancel(E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io, E_D_REQ_END why);

/**
 * e_d_req_pending - Does the engine own the debugger's output?
 * Return: 1 while a request is in flight, waiting, or an old answer is
 * being dropped; else 0.
 */
int e_d_req_pending(const E_D_REQ_QUEUE *rq);

/**
 * e_d_req_busy_message - What the user is told when a command is refused.
 * @rq, @io: the engine and its world; @out, @n: the sentence.
 */
void e_d_req_busy_message(const E_D_REQ_QUEUE *rq, const E_D_REQ_IO *io,
                          char *out, size_t n);

/* ---- chains of steps ---- */

/* One step: @command builds what to send (0 when there is nothing, the step
   is skipped); @answered reads the answer and says E_D_STEP_*.  A table of
   steps ends with a step whose two functions are NULL. */
typedef struct
{
 int (*command)(char *cmd, size_t n, void *ctx);
 int (*answered)(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx);
} E_D_STEP;

/* Called once when the chain ends: @ok is 0 when a request ended any way but
   answered, or a command could not go out. */
typedef void (*e_d_chain_done_fn)(int ok, void *ctx);

/* What a chain sends its commands through.  @request queues @cmd on
   @engine and returns 0 (E_D_SEND_OK) when it did, after which @answered
   is called exactly once; any other value means it was refused and
   @answered is never called.  The line engine below is one such engine; a
   debugger that frames its answers itself (a DAP adapter, matching answers
   by seq) gives its own, so every backend runs the same tables of steps. */
typedef int (*e_d_request_fn)(void *engine, const char *cmd,
                              e_d_answered_fn answered, void *ctx);

typedef struct
{
 e_d_request_fn request;
 void *engine;              /* must outlive every chain run through it */
} E_D_REQUESTER;

/* The line engine as a requester: an engine and its world, together. */
typedef struct
{
 E_D_REQ_QUEUE *rq;
 const E_D_REQ_IO *io;
} E_D_REQ_LINK;

/**
 * e_d_req_request - e_d_req_send for a chain: @engine is an E_D_REQ_LINK.
 * Return: what e_d_req_send returns.
 */
int e_d_req_request(void *engine, const char *cmd, e_d_answered_fn answered,
                    void *ctx);

/**
 * e_d_chain_run - Run a table of steps, one request at a time.
 * @via: the engine the commands go through (copied; its @engine must
 * outlive the chain); @steps: the table; @ctx: handed to every step;
 * @finished: told exactly once, also when the chain cannot start.
 *
 * The user sees the stop's watches and stack arrive one after the other
 * while keys still reach the editor; a cancel ends the chain quietly.
 * Return: 1 while the chain runs or when it ended well at once, 0 when it
 * failed (@finished has then already been told 0).
 */
int e_d_chain_run(const E_D_REQUESTER *via, const E_D_STEP *steps, void *ctx,
                  e_d_chain_done_fn finished);

#endif /* WPE_DEBREQ_H */
