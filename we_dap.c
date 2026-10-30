/* we_dap.c -- DAP debug session engine.  See we_dap.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#if defined(__linux__)
# include <sys/syscall.h>
#endif
#if defined(HAVE_KQUEUE) && defined(HAVE_SYS_EVENT_H)
# include <sys/event.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
#if defined(HAVE_PTY_H)
# include <pty.h>          /* glibc: openpty/forkpty */
#elif defined(HAVE_UTIL_H)
# include <util.h>         /* macOS, *BSD */
#elif defined(HAVE_LIBUTIL_H)
# include <libutil.h>      /* some BSDs */
#endif
#include <termios.h>
#include <sys/ioctl.h>

#include "we_dap.h"
#include "we_dap_proto.h"

#define DAP_MAX_BP   64
#define DAP_ACCEPT_MS 10000
#define DAP_READ_CHUNK 4096
#define DAP_NO_FD    (-1)
#define DAP_NO_SEQ   (-1)
#define DAP_POLL_FOREVER (-1)
#define DAP_NO_DEADLINE  (-1L)
#define DAP_MS_PER_SEC   1000L
#define DAP_NS_PER_MS    1000000L
/* How long Quit waits for the adapter to answer `disconnect` and exit
   before it is killed.  The adapters measured answer within milliseconds. */
#define DAP_DISCONNECT_MS 2000
/* After its answer, how long the adapter gets to exit by itself (lldb-dap
   takes about a second), and after SIGTERM, how long before SIGKILL. */
#define DAP_EXIT_MS      2000
#define DAP_TERM_MS      1000
#define DAP_REASON_MAX   32
#define DAP_PENDING_MAX  64     /* requests whose answer has an owner      */
#define DAP_BP_IDS_MAX   256    /* adapter breakpoint ids remembered       */
#define DAP_NO_ID        (-1)
/* The reason a stop is reported with when the adapter refused to resume:
   the program is where it was, and the host marks it there again. */
#define DAP_REASON_REFUSED "refused"
#define DAP_REASON_PAUSE   "pause"

/* What a wait found (dap_wait_readable). */
#define DAP_WAIT_READY     0    /* the descriptor is readable           */
#define DAP_WAIT_TIMED_OUT 1    /* the deadline passed                  */
#define DAP_WAIT_GAVE_UP   2    /* poll failed or the editor is ending  */

/* Who owns the answer to a request: the dispatcher hands each response to
   the owner its seq was registered with.  A response whose seq has no
   owner (gdb's late launch answer, a resume's plain success) is dropped. */
#define DAP_REQ_FREE        0   /* table slot unused                       */
#define DAP_REQ_WAITED      1   /* a synchronous call waits for it         */
#define DAP_REQ_BREAKPOINTS 2   /* setBreakpoints: told to on_breakpoints  */
#define DAP_REQ_LOCATE      3   /* stackTrace after a stop: on_stopped     */
#define DAP_REQ_RESUME      4   /* continue/next/...: a refusal is a stop  */
#define DAP_REQ_PAUSE       5   /* pause: a refusal forgets the pause      */
#define DAP_REQ_DISCONNECT  6   /* disconnect: the adapter is going        */

/* Where the program stands, as the run pump sees it. */
#define DAP_RUN_STARTING      0 /* the start is not done; a stop waits     */
#define DAP_RUN_STOPPED       1 /* stopped and reported to the host        */
#define DAP_RUN_GOING         2 /* resumed; waits for a stop or the end    */
#define DAP_RUN_LOCATING      3 /* stopped; its stackTrace not answered    */
#define DAP_RUN_DISCONNECTING 4 /* disconnect sent; waits for answer, exit */

/* The start of a session, one step per answer it waits for.  The order is
   the handshake's: initialize, then launch, setBreakpoints,
   setFunctionBreakpoints and configurationDone; the session is ready when
   configurationDone has been answered. */
#define DAP_STEP_ACCEPT      0   /* reverse TCP: the adapter has not dialled in */
#define DAP_STEP_INITIALIZE  1   /* initialize sent                             */
#define DAP_STEP_OPENED      2   /* initialize answered; nothing else sent yet  */
#define DAP_STEP_LAUNCH      3   /* launch sent; dlv answers once it has built  */
#define DAP_STEP_INITIALIZED 4   /* launch sent; the `initialized` event awaited */
#define DAP_STEP_BREAKPOINTS 5   /* setBreakpoints sent                         */
#define DAP_STEP_ENTRY       6   /* setFunctionBreakpoints sent                 */
#define DAP_STEP_CONFIGURED  7   /* configurationDone sent                      */
#define DAP_STEP_READY       8   /* configurationDone answered                  */

/* A request whose answer has an owner. */
typedef struct {
 int   kind;                  /* DAP_REQ_*                                   */
 int   seq;                   /* the request's seq                           */
 struct json_object *answer;  /* DAP_REQ_WAITED: the response, once it came  */
 char *file;                  /* DAP_REQ_BREAKPOINTS: the file and the lines */
 int  *lines;                 /*   asked for, to name them in the answer     */
 int   n;
} dap_pending;

/* An adapter breakpoint id and the line the host asked for, so a later
   `breakpoint` event that names only the id reaches the host's line. */
typedef struct {
 int   id;
 int   line;
 char *file;
} dap_bp_id;

/* Two transports.  Reverse-TCP (Delve): DAP on a socket, the debuggee's output
   on a separate pty we read.  Stdio (gdb/lldb -dap): DAP flows over the adapter's
   own stdin/stdout, so there is no pty and program output arrives as DAP "output"
   events instead. */
struct e_dap_session {
 int           sock;          /* TCP transport: connected adapter socket, else -1 */
 int           in_fd;         /* stdio transport: write to adapter stdin, else -1  */
 int           out_fd;        /* stdio transport: read from adapter stdout, else -1 */
 int           pty_master;    /* TCP only: master of the adapter's controlling pty;
                                 the debuggee's stdout/stderr arrive here.  -1 in
                                 stdio mode (output comes via DAP events). */
 int           forward_output;/* stdio: forward stdout/stderr "output" events to
                                 the host (set once the program is running, so the
                                 adapter's own startup banner is not echoed). */
 int           launch_mode_debug;/* launch arg "mode":"debug" (dlv builds the pkg);
                                 0 for adapters launched on a prebuilt binary (gdb). */
 int           minimal_launch; /* BSP/Bloop: launch carries no program/mode (the
                                 build server already bound the main class).      */
 int           wait_init;      /* BSP/Bloop: wait the `initialized` event after
                                 launch, before setBreakpoints (strict DAP order).*/
 int           listen_fd;     /* reverse TCP: waiting for the adapter to dial
                                 in, else -1                                    */
 const char   *adapter_id;    /* the initialize request's adapterID              */
 int           step;          /* DAP_STEP_*: what the start waits for next       */
 int           goal;          /* the step at which the start is done             */
 int           await_seq;     /* the request whose response the step waits for   */
 int           start_failed;  /* the adapter hung up, never dialled in, or
                                 refused a request                              */
 char         *start_error;   /* why the adapter refused, or NULL                */
 int           spawned;       /* the adapter is this process's own child         */
 pid_t         pid;           /* adapter process                    */
 int           seq;           /* request sequence counter           */
 e_dap_reader  rd;            /* incoming frame reassembly          */
 e_dap_host    host;
 char         *program;
 char         *entry_func;    /* e.g. "main.main", or NULL          */
 int           thread_id;     /* current stopped thread             */
 int           frame_id;      /* current top frame (for evaluate)   */
 int           ended;
 int           exit_known;    /* the `exited` event named a code      */
 int           exit_code;     /* that code                           */
 char          bp_file[512];
 int           bp_lines[DAP_MAX_BP];
 int           bp_n;
 int           eof;           /* the transport reached its end              */
 int           run;           /* DAP_RUN_*                                  */
 int           stop_pending;  /* a stop came while the start was not done   */
 int           stop_reported; /* on_stopped fired; the pump says STOPPED     */
 int           pause_pending; /* pause sent: the next stop is the pause      */
 int           disconnect_done;/* disconnect answered, nothing left to wait */
 int           quitting;      /* e_dap_close's bounded waits run: the end
                                 request does not cut them short           */
 char          stop_reason[DAP_REASON_MAX];
 dap_pending   pending[DAP_PENDING_MAX];
 dap_bp_id     bp_ids[DAP_BP_IDS_MAX];
 int           bp_ids_n;
};

/* The fd to write requests to / read messages from, abstracting the transport. */
static int dap_wfd(e_dap_session *s) { return s->sock >= 0 ? s->sock : s->in_fd; }
static int dap_rfd(e_dap_session *s) { return s->sock >= 0 ? s->sock : s->out_fd; }

/* ---- low-level I/O ----------------------------------------------------- */

static int write_all(int fd, const char *buf, size_t n)
{
 size_t off = 0;
 while (off < n)
 {
  ssize_t w = write(fd, buf + off, n - off);
  if (w < 0)
  {
   if (errno == EINTR)
    continue;
   return -1;
  }
  off += (size_t)w;
 }
 return 0;
}

/* Send a request; takes ownership of `args` (may be NULL).  Returns its seq. */
static int dap_send(e_dap_session *s, const char *command, struct json_object *args)
{
 int seq = ++s->seq;
 size_t len = 0;
 char *frame = e_dap_build_request(seq, command, args, &len);
 if (args)
  json_object_put(args);
 if (frame)
 {
  write_all(dap_wfd(s), frame, len);
  free(frame);
 }
 return seq;
}

/**
 * e_dap_drain_output - Read what the debuggee wrote to the adapter's pty.
 * @s: the session.
 *
 * Reads once and hands the text to the host, so the program's output lands
 * in Messages and the program never stalls on a full pty.  When the pty is
 * gone (the adapter ended) it is closed and e_dap_output_fd answers -1.
 */
void e_dap_drain_output(e_dap_session *s)
{
 char buf[DAP_READ_CHUNK];
 ssize_t n;

 if (!s || s->pty_master < 0)
  return;
 n = read(s->pty_master, buf, sizeof(buf) - 1);
 if (n > 0)
 {
  buf[n] = '\0';
  if (s->host.on_output)
   s->host.on_output(buf, s->host.ud);
  return;
 }
 if (n < 0 && errno == EINTR)
  return;
 close(s->pty_master);                /* slave gone (EIO/EOF) */
 s->pty_master = -1;
}

/* The editor's end request (see e_dap_set_end_watch): a descriptor that
   becomes readable when it comes, the question whether it came, and the
   save of the user's work, run once if it comes while Quit waits. */
static int dap_end_fd = DAP_NO_FD;
static int (*dap_end_requested)(void) = NULL;
static void (*dap_end_save)(void) = NULL;
static int dap_end_handled = 0;   /* Quit's waits saw it and saved */

void e_dap_set_end_watch(int fd, int (*requested)(void), void (*save)(void))
{
 dap_end_fd = fd;
 dap_end_requested = requested;
 dap_end_save = save;
 dap_end_handled = 0;
}

/**
 * dap_end_watch_add - Put the end request's descriptor in a wait's poll set.
 * @p: the set; @np: how many entries it has, raised by one when added.
 *
 * With it in the set, a SIGTERM or SIGHUP that lands just before the wait
 * starts still wakes it, so the user is never left with an editor that
 * waits for a program that runs for ever.
 * Return: its index in @p, or -1 when there is no end request to watch.
 */
static int dap_end_watch_add(struct pollfd *p, int *np)
{
 if (dap_end_fd == DAP_NO_FD)
  return -1;
 p[*np].fd = dap_end_fd;
 p[*np].events = POLLIN;
 p[*np].revents = 0;
 return (*np)++;
}

/**
 * dap_end_pending - Must a wait give up because the editor is ending?
 * Return: 1 when the end was asked for, else 0.
 */
static int dap_end_pending(void)
{
 if (!dap_end_requested)
  return 0;
 return dap_end_requested() != 0;
}

/**
 * dap_poll_fired - Did entry @i of a poll set wake the poll?
 * @p: the set; @i: the entry, or -1 when it was not in the set; @pret:
 * what poll() returned.
 */
static int dap_poll_fired(const struct pollfd *p, int i, int pret)
{
 if (i < 0 || pret <= 0)
  return 0;
 return p[i].revents != 0;
}

/**
 * dap_quit_end_watch_add - Watch the end request during Quit's waits,
 * until it has been handled.
 * @p, @np: the poll set, as for dap_end_watch_add.
 *
 * Once handled the descriptor stays readable (the editor's loop drains
 * it), so it is left out of the later polls, which would otherwise return
 * at once until their deadline.
 * Return: its index in @p, or -1 when it is not watched.
 */
static int dap_quit_end_watch_add(struct pollfd *p, int *np)
{
 if (dap_end_handled)
  return -1;
 return dap_end_watch_add(p, np);
}

/**
 * dap_quit_end_check - The end request came while Quit waits: save the
 * user's work now, then let Quit go on.
 * @fired: the end descriptor woke the poll.
 *
 * Quit's waits are bounded but can last seconds (an adapter that never
 * answers), and a second signal ends the editor at once without a save.
 * Saving first means the work is on disk whatever comes next, while the
 * adapter is still ended properly, so no program is left behind.  The save
 * runs once; the editor's own end later finds it done.
 */
static void dap_quit_end_check(int fired)
{
 if (dap_end_handled)
  return;
 if (!fired && !dap_end_pending())
  return;
 dap_end_handled = 1;
 if (dap_end_pending() && dap_end_save)
  dap_end_save();
}

/* Poll set size of a session wait: the transport, the pty, the end request. */
#define DAP_WAIT_FDS 3
/* Of the wait for the adapter's exit: its exit descriptor (pidfd or
   kqueue), the pty, the transport and the end request. */
#define DAP_EXIT_FDS 4

/**
 * dap_now_ms - The monotonic clock, in milliseconds.
 *
 * Deadlines count from it, so a clock change during Quit neither cuts the
 * adapter's time short nor stretches it.
 */
static long dap_now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * DAP_MS_PER_SEC + ts.tv_nsec / DAP_NS_PER_MS;
}

/**
 * dap_ms_left - What remains of @deadline, as a poll() timeout.
 * @deadline: a dap_now_ms() instant, or DAP_NO_DEADLINE.
 * Return: the milliseconds left (0 once passed), or DAP_POLL_FOREVER.
 */
static int dap_ms_left(long deadline)
{
 long left;

 if (deadline == DAP_NO_DEADLINE)
  return DAP_POLL_FOREVER;
 left = deadline - dap_now_ms();
 if (left < 0)
  return 0;
 return (int)left;
}

/**
 * dap_wait_readable - Wait until @fd is readable, passing on the program's
 * output meanwhile.
 * @s: the session; @fd: the descriptor waited on (the transport, or the
 * listening socket while dlv has not dialled in); @deadline: a dap_now_ms()
 * instant, or DAP_NO_DEADLINE.
 *
 * Used only where the caller has nothing else to do (a test, the
 * synchronous API, Quit).  dlv's pty is drained here, so a program that
 * prints a lot never stalls on a full pty while the editor waits; the end
 * request is watched, so a SIGTERM or SIGHUP ends the wait at once.  Quit's
 * own waits (s->quitting) are the exception: they are bounded, and cutting
 * them short sends lldb-dap SIGTERM before it has acted on `disconnect`,
 * which leaves its running program behind (measured: 5 of 5 times with the
 * end request already raised; SIGTERM or SIGKILL alone never ends it).
 * Return: DAP_WAIT_READY, DAP_WAIT_TIMED_OUT or DAP_WAIT_GAVE_UP.
 */
static int dap_wait_readable(e_dap_session *s, int fd, long deadline)
{
 for (;;)
 {
  struct pollfd p[DAP_WAIT_FDS];
  int np = 0, pty_i = -1, end_i, pret;

  if (fd < 0)
   return DAP_WAIT_GAVE_UP;
  if (deadline != DAP_NO_DEADLINE && dap_ms_left(deadline) == 0)
   return DAP_WAIT_TIMED_OUT;
  p[np].fd = fd; p[np].events = POLLIN; p[np].revents = 0; np++;
  if (s->pty_master >= 0)
  {
   pty_i = np;
   p[np].fd = s->pty_master; p[np].events = POLLIN; p[np].revents = 0;
   np++;
  }
  if (s->quitting)
   end_i = dap_quit_end_watch_add(p, &np);
  else
   end_i = dap_end_watch_add(p, &np);
  pret = poll(p, np, dap_ms_left(deadline));
  if (!s->quitting && dap_end_pending())
   return DAP_WAIT_GAVE_UP;
  if (s->quitting)
   dap_quit_end_check(dap_poll_fired(p, end_i, pret));
  if (pret < 0 && errno == EINTR)
   continue;
  if (pret < 0)
   return DAP_WAIT_GAVE_UP;
  if (pret == 0)
   return DAP_WAIT_TIMED_OUT;
  if (pty_i >= 0 && (p[pty_i].revents & (POLLIN | POLLHUP | POLLERR)))
   e_dap_drain_output(s);
  if (p[0].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
   return DAP_WAIT_READY;
 }
}

/**
 * dap_read_once - Read what the adapter sent, once, into the reader.
 * @s: the session, its transport readable.
 *
 * End of file or a read error ends the session: an adapter that dies in
 * the middle of a message leaves its half message in the reader, never a
 * wait for the rest.
 * Return: 1 while the transport is open, 0 once it reached its end.
 */
static int dap_read_once(e_dap_session *s)
{
 char buf[DAP_READ_CHUNK];
 ssize_t n;

 if (s->eof)
  return 0;
 n = read(dap_rfd(s), buf, sizeof(buf));
 if (n > 0)
 {
  e_dap_reader_push(&s->rd, buf, (size_t)n);
  return 1;
 }
 if (n < 0 && (errno == EINTR || errno == EAGAIN))
  return 1;
 s->eof = 1;
 s->ended = 1;
 return 0;
}

static int dap_fd_readable_now(int fd);

/**
 * dap_transport_readable_now - Would a read of the transport return at once?
 * @s: the session.
 *
 * Lets the run pump be called at any time, readable or not, without ever
 * blocking the editor: it then only answers what was already read.
 */
static int dap_transport_readable_now(e_dap_session *s)
{
 if (s->eof)
  return 0;
 return dap_fd_readable_now(dap_rfd(s));
}

/**
 * dap_fd_readable_now - Would a read (or accept) of @fd return at once?
 * @fd: a descriptor, or -1.
 */
static int dap_fd_readable_now(int fd)
{
 struct pollfd p;

 if (fd < 0)
  return 0;
 p.fd = fd;
 p.events = POLLIN;
 p.revents = 0;
 if (poll(&p, 1, 0) <= 0)
  return 0;
 return (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0;
}

/**
 * dap_discard_once - Read what the adapter sent, once, and drop it.
 * @s: the session, its transport readable, its answers no longer wanted
 * (Quit waits for the adapter's exit).
 *
 * Keeps an adapter that writes while it exits from blocking on a full
 * pipe, without keeping what it wrote: one that writes without end cannot
 * grow the editor's memory while Quit waits.
 */
static void dap_discard_once(e_dap_session *s)
{
 char buf[DAP_READ_CHUNK];
 ssize_t n = read(dap_rfd(s), buf, sizeof(buf));

 if (n > 0 || (n < 0 && (errno == EINTR || errno == EAGAIN)))
  return;
 s->eof = 1;
 s->ended = 1;
}

/**
 * dap_pop_message - The next complete message already read, or NULL.
 * @s: the session.
 *
 * A malformed frame is skipped (the reader drops it), so one bad frame
 * never hides the good ones behind it.
 */
static struct json_object *dap_pop_message(e_dap_session *s)
{
 for (;;)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);

  if (m || !err)
   return m;
 }
}

/* ---- message helpers --------------------------------------------------- */

static const char *obj_str(struct json_object *o, const char *k)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return json_object_get_string(v);
 return NULL;
}

static int obj_int(struct json_object *o, const char *k, int dflt)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return json_object_get_int(v);
 return dflt;
}

static struct json_object *obj_obj(struct json_object *o, const char *k)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return v;
 return NULL;
}

/**
 * dap_note_exit_code - Keep the exit code an `exited` event carries.
 * @s: the session; @body: the event's body.
 *
 * The user reads "Program exited with code N" at the end of an adapter
 * session as under gdb, so a failing program is told apart from one that
 * succeeded.  Only `exited` has the code; `terminated` never does.
 */
static void dap_note_exit_code(e_dap_session *s, struct json_object *body)
{
 struct json_object *v = NULL;

 if (!body || !json_object_object_get_ex(body, "exitCode", &v))
  return;
 s->exit_code = json_object_get_int(v);
 s->exit_known = 1;
}

static int obj_bool(struct json_object *o, const char *k)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return json_object_get_boolean(v);
 return 0;
}

/**
 * dap_response_refused - Is @m a response with "success": false?
 * @m: a message from the adapter.
 */
static int dap_response_refused(struct json_object *m)
{
 struct json_object *v = NULL;
 const char *type = obj_str(m, "type");

 if (!type || strcmp(type, "response"))
  return 0;
 if (!json_object_object_get_ex(m, "success", &v))
  return 0;
 return !json_object_get_boolean(v);
}

/**
 * dap_refusal_text - The adapter's own words for a refused request.
 * @m: the refusing response.
 *
 * dlv puts its build error in body.error.format, the others a short
 * "message"; the user reads that instead of a bare "failed".
 */
static const char *dap_refusal_text(struct json_object *m)
{
 const char *why = obj_str(obj_obj(obj_obj(m, "body"), "error"), "format");

 if (!why || !*why)
  why = obj_str(m, "message");
 if (!why || !*why)
  why = "the debug adapter refused the request";
 return why;
}

/* ---- the dispatcher: every answer reaches its owner ----------------------
 *
 * Each request whose answer matters is registered with its seq and what
 * the answer is for.  Every read, the run pump's or a synchronous wait's,
 * hands each message to dap_dispatch: a response goes to the owner of its
 * seq, an event to dap_handle_event.  So a setBreakpoints answer that comes
 * while the editor waits for a stackTrace or an evaluate at a stop reaches
 * on_breakpoints, and a stop that comes before the answer to the continue
 * that caused it (dlv's pause) is the stop.
 */

/**
 * dap_pending_find - The owner of the answer to request @seq, or NULL.
 * @s: the session; @seq: a request's seq.
 */
static dap_pending *dap_pending_find(e_dap_session *s, int seq)
{
 int i;

 if (seq == DAP_NO_SEQ)
  return NULL;
 for (i = 0; i < DAP_PENDING_MAX; i++)
  if (s->pending[i].kind != DAP_REQ_FREE && s->pending[i].seq == seq)
   return &s->pending[i];
 return NULL;
}

/**
 * dap_pending_release - Forget a request: its answer came or never will.
 * @p: the table slot.
 */
static void dap_pending_release(dap_pending *p)
{
 if (p->answer)
  json_object_put(p->answer);
 free(p->file);
 free(p->lines);
 memset(p, 0, sizeof(*p));
}

/**
 * dap_send_owned - Send a request and register who owns its answer.
 * @s: the session; @command, @args: the request (@args is taken over);
 * @kind: DAP_REQ_*, what the answer is for.
 *
 * A request is not sent when its answer could not be routed (the table is
 * full of unanswered requests, or the adapter is gone): the caller then
 * knows at once instead of waiting for an answer that would be dropped.
 * Return: the request's seq, or DAP_NO_SEQ when it was not sent.
 */
static int dap_send_owned(e_dap_session *s, const char *command,
                          struct json_object *args, int kind)
{
 dap_pending *p = NULL;
 int i;

 for (i = 0; !p && i < DAP_PENDING_MAX; i++)
  if (s->pending[i].kind == DAP_REQ_FREE)
   p = &s->pending[i];
 if (!p || s->eof)
 {
  if (args)
   json_object_put(args);
  return DAP_NO_SEQ;
 }
 p->kind = kind;
 p->seq = dap_send(s, command, args);
 return p->seq;
}

/**
 * dap_pending_free_all - Forget every unanswered request (the session ends).
 * @s: the session.
 */
static void dap_pending_free_all(e_dap_session *s)
{
 int i;

 for (i = 0; i < DAP_PENDING_MAX; i++)
  dap_pending_release(&s->pending[i]);
}

/* ---- breakpoint ids: the line the host asked for, by the adapter's id --- */

/**
 * dap_bp_ids_forget_file - setBreakpoints replaced one file's breakpoints:
 * forget the ids the adapter gave them before.
 * @s: the session; @file: the file's absolute path.
 */
static void dap_bp_ids_forget_file(e_dap_session *s, const char *file)
{
 int i = 0;

 while (i < s->bp_ids_n)
 {
  if (strcmp(s->bp_ids[i].file, file))
  {
   i++;
   continue;
  }
  free(s->bp_ids[i].file);
  s->bp_ids[i] = s->bp_ids[--s->bp_ids_n];
 }
}

/**
 * dap_bp_ids_find - The host's line for adapter breakpoint @id, or NULL.
 * @s: the session; @id: the id from a setBreakpoints answer.
 */
static dap_bp_id *dap_bp_ids_find(e_dap_session *s, int id)
{
 int i;

 if (id == DAP_NO_ID)
  return NULL;
 for (i = 0; i < s->bp_ids_n; i++)
  if (s->bp_ids[i].id == id)
   return &s->bp_ids[i];
 return NULL;
}

/**
 * dap_bp_ids_remember - Keep which host line the adapter's @id stands for.
 * @s: the session; @id: the adapter's id; @file, @line: the host's.
 *
 * gdb answers a breakpoint it cannot place yet as pending and later sends
 * a `breakpoint` event that names only the id; this lets that event reach
 * the line the user marked.
 */
static void dap_bp_ids_remember(e_dap_session *s, int id, const char *file,
                                int line)
{
 char *copy;

 if (id == DAP_NO_ID || s->bp_ids_n >= DAP_BP_IDS_MAX)
  return;
 if (dap_bp_ids_find(s, id))
  return;
 copy = strdup(file);
 if (!copy)
  return;
 s->bp_ids[s->bp_ids_n].id = id;
 s->bp_ids[s->bp_ids_n].line = line;
 s->bp_ids[s->bp_ids_n].file = copy;
 s->bp_ids_n++;
}

/**
 * dap_bp_ids_forget_id - The adapter removed breakpoint @id.
 * @s: the session; @id: its id.
 */
static void dap_bp_ids_forget_id(e_dap_session *s, int id)
{
 dap_bp_id *b = dap_bp_ids_find(s, id);

 if (!b)
  return;
 free(b->file);
 *b = s->bp_ids[--s->bp_ids_n];
}

/**
 * dap_bp_ids_free - Forget every breakpoint id (the session ends).
 * @s: the session.
 */
static void dap_bp_ids_free(e_dap_session *s)
{
 while (s->bp_ids_n > 0)
  free(s->bp_ids[--s->bp_ids_n].file);
}

/**
 * dap_bp_message - What the adapter said about one breakpoint.
 * @bp: the breakpoint as the adapter sent it; @verified: whether it took it.
 *
 * Its "message" when it gave one; for one it did not take, its "reason"
 * ("pending", "failed") otherwise, so the user can be told why.
 * Return: the text, or NULL.
 */
static const char *dap_bp_message(struct json_object *bp, int verified)
{
 const char *msg = obj_str(bp, "message");

 if (msg && *msg)
  return msg;
 if (!verified)
  return obj_str(bp, "reason");
 return NULL;
}

/**
 * dap_tell_breakpoints - Pass the adapter's word on breakpoints to the host.
 * @s: the session; @file, @lines, @verified, @msg, @n: see on_breakpoints.
 */
static void dap_tell_breakpoints(e_dap_session *s, const char *file,
                                 const int *lines, const int *verified,
                                 const char *const *msg, int n)
{
 if (!s->host.on_breakpoints || n <= 0)
  return;
 s->host.on_breakpoints(file, lines, verified, msg, n, s->host.ud);
}

/**
 * dap_bp_entry_read - Read the adapter's answer for one asked-for line.
 * @s: the session; @file, @line: the host's breakpoint; @bp: the
 * adapter's entry for it, or NULL when the answer has none;
 * @verified, @msg: receive what on_breakpoints passes on.
 */
static void dap_bp_entry_read(e_dap_session *s, const char *file, int line,
                              struct json_object *bp, int *verified,
                              const char **msg)
{
 *verified = 0;
 *msg = NULL;
 if (!bp)
  return;
 *verified = obj_bool(bp, "verified");
 *msg = dap_bp_message(bp, *verified);
 dap_bp_ids_remember(s, obj_int(bp, "id", DAP_NO_ID), file, line);
}

/**
 * dap_bp_list_item - Entry @i of the answer's breakpoint list, or NULL.
 * @list: body.breakpoints of a setBreakpoints answer (may be NULL); @i: it.
 */
static struct json_object *dap_bp_list_item(struct json_object *list, int i)
{
 if (!list || !json_object_is_type(list, json_type_array))
  return NULL;
 if (i >= (int)json_object_array_length(list))
  return NULL;
 return json_object_array_get_idx(list, i);
}

/**
 * dap_breakpoints_answered - setBreakpoints was answered: tell the host
 * which of the file's breakpoints the adapter took.
 * @s: the session; @file: the file's absolute path; @lines, @n: the lines
 * asked for; @m: the response.
 *
 * The answer lists the breakpoints in the order they were asked for.  A
 * refused request leaves every one of them untaken, with the adapter's
 * words as the reason.
 */
static void dap_breakpoints_answered(e_dap_session *s, const char *file,
                                     const int *lines, int n,
                                     struct json_object *m)
{
 int verified[DAP_MAX_BP];
 const char *msg[DAP_MAX_BP];
 struct json_object *list = obj_obj(obj_obj(m, "body"), "breakpoints");
 int refused = dap_response_refused(m);
 int i;

 if (!file || (!lines && n > 0))
  return;
 if (n > DAP_MAX_BP)
  n = DAP_MAX_BP;
 dap_bp_ids_forget_file(s, file);
 for (i = 0; i < n; i++)
  dap_bp_entry_read(s, file, lines[i], dap_bp_list_item(list, i),
                    &verified[i], &msg[i]);
 if (refused)
  for (i = 0; i < n; i++)
   msg[i] = dap_refusal_text(m);
 dap_tell_breakpoints(s, file, lines, verified, msg, n);
}

/**
 * dap_tell_one_breakpoint - A `breakpoint` event changed one breakpoint.
 * @s: the session; @file, @line: the host's breakpoint; @bp: the adapter's.
 */
static void dap_tell_one_breakpoint(e_dap_session *s, const char *file,
                                    int line, struct json_object *bp)
{
 int verified = obj_bool(bp, "verified");
 const char *msg = dap_bp_message(bp, verified);

 dap_tell_breakpoints(s, file, &line, &verified, &msg, 1);
}

/**
 * dap_breakpoint_event - The adapter changed a breakpoint on its own.
 * @s: the session; @body: the `breakpoint` event's body.
 *
 * gdb sends one when a pending breakpoint gets placed, which is when the
 * user's mark can turn from "not yet" to "set".  A breakpoint the adapter
 * did not get from us is told under the place the adapter gives it.
 */
static void dap_breakpoint_event(e_dap_session *s, struct json_object *body)
{
 struct json_object *bp = obj_obj(body, "breakpoint");
 const char *why = obj_str(body, "reason");
 const char *path = obj_str(obj_obj(bp, "source"), "path");
 int id = obj_int(bp, "id", DAP_NO_ID);
 int line = obj_int(bp, "line", 0);
 dap_bp_id *known;

 if (!bp)
  return;
 if (why && !strcmp(why, "removed"))
 {
  dap_bp_ids_forget_id(s, id);
  return;
 }
 known = dap_bp_ids_find(s, id);
 if (known)
 {
  dap_tell_one_breakpoint(s, known->file, known->line, bp);
  return;
 }
 if (!path || line <= 0)
  return;
 dap_bp_ids_remember(s, id, path, line);
 dap_tell_one_breakpoint(s, path, line, bp);
}

/* ---- the stop: located with one stackTrace, then told ------------------- */

/**
 * dap_tell_stopped - The stop is located: tell the host and end the run.
 * @s: the session; @file, @line: the top frame's source (NULL, 0 when it
 * has none, e.g. Go's pre-main entry stop).
 *
 * The program's own output is forwarded from the first stop on: gdb's
 * banner and pending-breakpoint chatter during the start would otherwise
 * clutter Messages.
 */
static void dap_tell_stopped(e_dap_session *s, const char *file, int line)
{
 s->run = DAP_RUN_STOPPED;
 s->stop_reported = 1;
 s->forward_output = 1;
 if (s->host.on_stopped)
  s->host.on_stopped(file, line, s->stop_reason, s->host.ud);
}

/**
 * dap_locate - Ask where the program stopped: stackTrace of the top frame.
 * @s: the session, its program just stopped.
 *
 * The answer comes through the dispatcher, so neither the pump nor the
 * editor waits for it.
 */
static void dap_locate(e_dap_session *s)
{
 struct json_object *args = json_object_new_object();

 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 json_object_object_add(args, "startFrame", json_object_new_int(0));
 json_object_object_add(args, "levels", json_object_new_int(1));
 s->run = DAP_RUN_LOCATING;
 if (dap_send_owned(s, "stackTrace", args, DAP_REQ_LOCATE) == DAP_NO_SEQ)
  dap_tell_stopped(s, NULL, 0);
}

/**
 * dap_locate_answered - The stop's stackTrace was answered: tell the host.
 * @s: the session; @m: the response.
 */
static void dap_locate_answered(e_dap_session *s, struct json_object *m)
{
 struct json_object *frames = obj_obj(obj_obj(m, "body"), "stackFrames");
 struct json_object *fr0;

 if (s->run != DAP_RUN_LOCATING)
  return;
 if (!frames || !json_object_is_type(frames, json_type_array) ||
     json_object_array_length(frames) == 0)
 {
  dap_tell_stopped(s, NULL, 0);
  return;
 }
 fr0 = json_object_array_get_idx(frames, 0);
 s->frame_id = obj_int(fr0, "id", 0);
 dap_tell_stopped(s, obj_str(obj_obj(fr0, "source"), "path"),
                  obj_int(fr0, "line", 0));
}

/**
 * dap_note_stop_reason - Keep why the program stopped, for on_stopped.
 * @s: the session; @why: the adapter's reason, or NULL.
 *
 * The stop that follows the user's pause is the pause, whatever the
 * adapter calls it: lldb-dap reports SIGSTOP as an "exception".
 */
static void dap_note_stop_reason(e_dap_session *s, const char *why)
{
 if (s->pause_pending)
  why = DAP_REASON_PAUSE;
 if (!why)
  why = "stopped";
 s->pause_pending = 0;
 snprintf(s->stop_reason, sizeof(s->stop_reason), "%s", why);
}

/**
 * dap_run_after_start - The start is done: the program runs now.
 * @s: the session.
 *
 * A stop that came before the start's last answer (it can, the program
 * starts at configurationDone) is located now instead of being lost.
 */
static void dap_run_after_start(e_dap_session *s)
{
 s->run = DAP_RUN_GOING;
 if (!s->stop_pending)
  return;
 s->stop_pending = 0;
 dap_locate(s);
}

/**
 * dap_stop_event - The adapter says the program stopped.
 * @s: the session; @body: the `stopped` event's body.
 *
 * Only a running program's stop ends the run.  At a stop, a second
 * thread's stop only moves the thread later requests name.  dlv sends a
 * pause stop with no threadId: the thread of the last stop is kept.
 */
static void dap_stop_event(e_dap_session *s, struct json_object *body)
{
 s->thread_id = obj_int(body, "threadId", s->thread_id);
 if (s->run != DAP_RUN_STARTING && s->run != DAP_RUN_GOING)
  return;
 dap_note_stop_reason(s, obj_str(body, "reason"));
 if (s->run == DAP_RUN_STARTING)
 {
  s->stop_pending = 1;
  return;
 }
 dap_locate(s);
}

/**
 * dap_resume_refused - The adapter refused to resume: the program is still
 * where it stopped.
 * @s: the session.
 *
 * Reported as a stop at the same place, so the session goes on instead of
 * waiting for a stop that will never come.
 */
static void dap_resume_refused(e_dap_session *s)
{
 if (s->run != DAP_RUN_GOING)
  return;
 snprintf(s->stop_reason, sizeof(s->stop_reason), "%s", DAP_REASON_REFUSED);
 dap_locate(s);
}

/**
 * dap_close_write_side - Tell the adapter nothing more will be asked.
 * @s: the session.
 *
 * The reading side stays open, so the adapter's exit is seen as the end of
 * its output.
 */
static void dap_close_write_side(e_dap_session *s)
{
 if (s->sock >= 0)
 {
  shutdown(s->sock, SHUT_WR);
  return;
 }
 if (s->in_fd >= 0)
  close(s->in_fd);
 s->in_fd = DAP_NO_FD;
}

/**
 * dap_disconnect_answered - The adapter answered Quit's `disconnect`.
 * @s: the session.
 *
 * The program is ended by then, and Quit is done as far as the adapter's
 * words go.  It is told nothing more will be asked (some adapters exit on
 * that), and e_dap_close waits for its exit before reaping it.
 */
static void dap_disconnect_answered(e_dap_session *s)
{
 dap_close_write_side(s);
 s->disconnect_done = 1;
}

/**
 * dap_answer_routed - Hand a response to the owner its kind names.
 * @s: the session; @kind: DAP_REQ_*; @m: the response.
 */
static void dap_answer_routed(e_dap_session *s, int kind, struct json_object *m)
{
 switch (kind)
 {
  case DAP_REQ_LOCATE:
   dap_locate_answered(s, m);
   break;
  case DAP_REQ_RESUME:
   if (dap_response_refused(m))
    dap_resume_refused(s);
   break;
  case DAP_REQ_PAUSE:
   if (dap_response_refused(m))
    s->pause_pending = 0;
   break;
  case DAP_REQ_DISCONNECT:
   dap_disconnect_answered(s);
   break;
  default:
   break;
 }
}

/**
 * dap_dispatch_response - Hand a response to the owner of its seq.
 * @s: the session; @m: the response (not freed here).
 *
 * A synchronous wait finds its answer kept in its slot; every other owner
 * is called now and its slot freed.  A response nobody owns is dropped.
 */
static void dap_dispatch_response(e_dap_session *s, struct json_object *m)
{
 dap_pending *p = dap_pending_find(s, obj_int(m, "request_seq", DAP_NO_SEQ));
 int kind;

 if (!p)
  return;
 kind = p->kind;
 if (kind == DAP_REQ_WAITED)
 {
  p->answer = json_object_get(m);
  return;
 }
 if (kind == DAP_REQ_BREAKPOINTS)
 {
  dap_breakpoints_answered(s, p->file, p->lines, p->n, m);
  dap_pending_release(p);
  return;
 }
 dap_pending_release(p);
 dap_answer_routed(s, kind, m);
}

static void dap_handle_event(e_dap_session *s, struct json_object *m);

/**
 * dap_dispatch - Take one message from the adapter.
 * @s: the session; @m: the message (not freed here).
 */
static void dap_dispatch(e_dap_session *s, struct json_object *m)
{
 const char *type = obj_str(m, "type");

 if (!type)
  return;
 if (!strcmp(type, "response"))
  dap_dispatch_response(s, m);
 else if (!strcmp(type, "event"))
  dap_handle_event(s, m);
}

/* Handle an unsolicited event (output / stopped / breakpoint / terminated /
   exited). */
static void dap_handle_event(e_dap_session *s, struct json_object *m)
{
 const char *ev = obj_str(m, "event");
 struct json_object *body = obj_obj(m, "body");

 if (!ev)
  return;
 if (!strcmp(ev, "output"))
 {
  /* TCP transport (dlv): the debuggee's output reaches us through the pty
     (see e_dap_drain_output), so the only "output" events are the adapter
     talking about itself -- drop them.  Stdio transport (gdb/lldb -dap):
     there is no pty, so the program's stdout/stderr DO arrive here --
     forward them, but only once the program is running (forward_output),
     or the adapter's startup banner (also category "stdout" for gdb) would
     be echoed into Messages. */
  const char *cat = obj_str(body, "category");
  const char *txt = obj_str(body, "output");
  int is_program = cat && (!strcmp(cat, "stdout") || !strcmp(cat, "stderr"));
  if (s->forward_output && is_program && txt && s->host.on_output)
   s->host.on_output(txt, s->host.ud);
 }
 else if (!strcmp(ev, "stopped"))
  dap_stop_event(s, body);
 else if (!strcmp(ev, "breakpoint"))
  dap_breakpoint_event(s, body);
 else if (!strcmp(ev, "terminated") || !strcmp(ev, "exited"))
 {
  if (!strcmp(ev, "exited"))
   dap_note_exit_code(s, body);
  s->ended = 1;
  if (s->host.on_terminated)
   s->host.on_terminated(s->host.ud);
 }
}

/* ---- reading: one shared path for the pump and the synchronous waits ---- */

/**
 * dap_dispatch_one - Take the next message already read, if there is one.
 * @s: the session.
 * Return: 1 when a message was taken, 0 when none was complete.
 */
static int dap_dispatch_one(e_dap_session *s)
{
 struct json_object *m = dap_pop_message(s);

 if (!m)
  return 0;
 dap_dispatch(s, m);
 json_object_put(m);
 return 1;
}

/**
 * dap_drain_queue - Take every message already read, before control goes
 * back to the host.
 * @s: the session.
 *
 * A message left in the reader would wait for the adapter to write again,
 * which at a stop may be never: a stop read together with the start's last
 * answer, or a breakpoint answer read together with an evaluate's, would
 * never reach the host.  Each engine call that returns to the host after
 * reading drains here, so the host never has to ask for what was read.
 */
static void dap_drain_queue(e_dap_session *s)
{
 while (dap_dispatch_one(s))
  ;
}

/**
 * dap_feed_one - One step of a synchronous wait: take a message already
 * read, or wait for the adapter and read once.
 * @s: the session; @deadline: a dap_now_ms() instant, or DAP_NO_DEADLINE.
 * Return: 1 to go on, 0 when the adapter is gone, the deadline passed or
 * the editor is ending.
 */
static int dap_feed_one(e_dap_session *s, long deadline)
{
 /* Checked first, so a bounded wait never dispatches past its deadline;
    the wait's own check before each poll is the one that ends it. */
 if (deadline != DAP_NO_DEADLINE && dap_ms_left(deadline) == 0)
  return 0;
 if (dap_dispatch_one(s))
  return 1;
 if (s->eof)
  return 0;
 if (dap_wait_readable(s, dap_rfd(s), deadline) != DAP_WAIT_READY)
  return 0;
 return dap_read_once(s);
}

/**
 * dap_wait_answer - Wait for the answer to request @seq, sent as
 * DAP_REQ_WAITED.
 * @s: the session; @seq: the request's seq, or DAP_NO_SEQ when it was not
 * sent.
 *
 * Every other message that comes meanwhile goes to its owner: output to
 * Messages, a setBreakpoints answer to on_breakpoints, a stop to the run.
 * Return: the response (the caller frees it), or NULL when the adapter
 * ended first or the editor is ending.
 */
static struct json_object *dap_wait_answer(e_dap_session *s, int seq)
{
 dap_pending *p = dap_pending_find(s, seq);
 struct json_object *m;

 if (!p)
  return NULL;
 while (!p->answer && dap_feed_one(s, DAP_NO_DEADLINE))
  ;
 m = p->answer;
 p->answer = NULL;
 dap_pending_release(p);
 dap_drain_queue(s);
 return m;
}

/**
 * dap_run_state - What the run pump has to say now, without taking it.
 * @s: the session.
 * Return: E_DAP_RUN_GOING, E_DAP_RUN_STOPPED or E_DAP_RUN_ENDED.
 */
static int dap_run_state(const e_dap_session *s)
{
 if (s->run == DAP_RUN_DISCONNECTING)
 {
  if (s->eof || s->disconnect_done)
   return E_DAP_RUN_ENDED;
  return E_DAP_RUN_GOING;
 }
 if (s->stop_reported)
  return E_DAP_RUN_STOPPED;
 if (s->ended)
  return E_DAP_RUN_ENDED;
 return E_DAP_RUN_GOING;
}

/**
 * dap_run_take - What the run pump answers; a stop is answered once.
 * @s: the session.
 */
static int dap_run_take(e_dap_session *s)
{
 int st = dap_run_state(s);

 if (st == E_DAP_RUN_STOPPED)
  s->stop_reported = 0;
 return st;
}

/**
 * dap_run_consume - Take every message already read, then say where the
 * run stands.
 * @s: the session.
 *
 * All of them, not only up to a stop: a message left behind would wait
 * until the adapter writes again, which at a stop may be never.
 */
static int dap_run_consume(e_dap_session *s)
{
 dap_drain_queue(s);
 return dap_run_take(s);
}

/**
 * dap_run_wait - Wait for the run to stop or end, for callers with nothing
 * else to do (the synchronous API, Quit).
 * @s: the session; @deadline: a dap_now_ms() instant, or DAP_NO_DEADLINE.
 * Return: E_DAP_RUN_STOPPED, E_DAP_RUN_ENDED, or E_DAP_RUN_GOING when the
 * wait gave up (deadline, end request).
 */
static int dap_run_wait(e_dap_session *s, long deadline)
{
 while (dap_run_state(s) == E_DAP_RUN_GOING && dap_feed_one(s, deadline))
  ;
 dap_drain_queue(s);
 return dap_run_take(s);
}

/**
 * dap_frame_from_json - Copy one stackTrace frame into @out.
 * @fr: the frame as the adapter sent it; @out: receives it.
 *
 * A frame without source (runtime code, a native function) keeps an empty
 * path and line 0, so the Stack window still names its function.
 */
static void dap_frame_from_json(struct json_object *fr, e_dap_frame *out)
{
 struct json_object *src = obj_obj(fr, "source");
 const char *name = obj_str(fr, "name");
 const char *path = obj_str(src, "path");

 out->id = obj_int(fr, "id", 0);
 out->line = obj_int(fr, "line", 0);
 snprintf(out->name, sizeof(out->name), "%s", name ? name : "??");
 snprintf(out->path, sizeof(out->path), "%s", path ? path : "");
}

int e_dap_stack_trace(e_dap_session *s, e_dap_frame *frames, int max)
{
 struct json_object *args, *resp, *body, *list;
 int seq, i, n = 0;

 if (!s || s->ended || max <= 0)
  return -1;
 args = json_object_new_object();
 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 json_object_object_add(args, "startFrame", json_object_new_int(0));
 json_object_object_add(args, "levels", json_object_new_int(max));
 seq = dap_send_owned(s, "stackTrace", args, DAP_REQ_WAITED);
 resp = dap_wait_answer(s, seq);
 if (!resp)
  return -1;
 body = obj_obj(resp, "body");
 list = body ? obj_obj(body, "stackFrames") : NULL;
 if (list && json_object_is_type(list, json_type_array))
 {
  n = (int)json_object_array_length(list);
  if (n > max)
   n = max;
  for (i = 0; i < n; i++)
   dap_frame_from_json(json_object_array_get_idx(list, i), &frames[i]);
 }
 json_object_put(resp);
 return n;
}

void e_dap_select_frame(e_dap_session *s, int frame_id)
{
 if (s)
  s->frame_id = frame_id;
}

int e_dap_exit_code(const e_dap_session *s, int *code)
{
 if (!s || !s->exit_known)
  return 0;
 *code = s->exit_code;
 return 1;
}

/* ---- adapter spawn (reverse-connect TCP) ------------------------------ */

/* Bind 127.0.0.1:0, listen, return the listen fd and chosen port. */
static int dap_listen_ephemeral(int *port)
{
 int fd = socket(AF_INET, SOCK_STREAM, 0);
 struct sockaddr_in a;
 socklen_t al = sizeof(a);
 int one = 1;

 if (fd < 0)
  return -1;
 setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
 memset(&a, 0, sizeof(a));
 a.sin_family = AF_INET;
 a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 a.sin_port = 0;
 if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
     listen(fd, 1) < 0 ||
     getsockname(fd, (struct sockaddr *)&a, &al) < 0)
 {
  close(fd);
  return -1;
 }
 *port = ntohs(a.sin_port);
 return fd;
}

/* Fork+exec the adapter with --client-addr=127.0.0.1:port appended.  `slave_fd`
   is the slave side of a pty created by the caller; the child makes it its
   controlling terminal and stdio.  DAP traffic flows over the reverse-TCP
   socket, so the adapter's stdio is free to be a pty -- and it MUST be: dlv runs
   the debuggee in foreground mode and calls tcsetpgrp() on its own stdin, which
   fails ("inappropriate ioctl for device") on a plain pipe.  The debuggee then
   inherits that pty, so its stdout/stderr land on the master the caller reads
   (xwpe's Messages window), exactly like the gdb backend's inferior pty. */
static pid_t dap_spawn(char *const argv[], int port, const char *cwd,
                       int listen_fd, int slave_fd)
{
 int argc = 0;
 char addr[64];
 char **av;
 pid_t pid;

 while (argv[argc])
  argc++;
 av = calloc(argc + 2, sizeof(char *));
 if (!av)
  return -1;
 for (int i = 0; i < argc; i++)
  av[i] = argv[i];
 snprintf(addr, sizeof(addr), "--client-addr=127.0.0.1:%d", port);
 av[argc] = addr;
 av[argc + 1] = NULL;

 pid = fork();
 if (pid == 0)
 {
  if (cwd)
   if (chdir(cwd) != 0)
    _exit(127);
  close(listen_fd);
  setsid();                       /* drop xwpe's controlling terminal */
  if (slave_fd >= 0)
  {
   ioctl(slave_fd, TIOCSCTTY, 0); /* the pty becomes our controlling tty */
   dup2(slave_fd, 0);
   dup2(slave_fd, 1);
   dup2(slave_fd, 2);
   if (slave_fd > 2)
    close(slave_fd);
  }
  execvp(av[0], av);
  _exit(127);
 }
 free(av);
 return pid;
}

/* Fork+exec a stdio adapter (gdb/lldb --interpreter=dap): DAP flows over the
   adapter's own stdin/stdout, which we wire to two pipes.  *in_fd receives the
   write end of the child's stdin; *out_fd the read end of the child's stdout.
   stderr is left inheriting xwpe's (adapter diagnostics, rarely used). */
static pid_t dap_spawn_stdio(char *const argv[], const char *cwd,
                             int *in_fd, int *out_fd)
{
 int to_child[2], from_child[2];
 pid_t pid;

 if (pipe(to_child) != 0)
  return -1;
 if (pipe(from_child) != 0)
 {  close(to_child[0]); close(to_child[1]); return -1;  }

 pid = fork();
 if (pid == 0)
 {
  if (cwd)
   if (chdir(cwd) != 0)
    _exit(127);
  dup2(to_child[0], 0);            /* child stdin  <- to_child read end  */
  dup2(from_child[1], 1);          /* child stdout -> from_child write end */
  close(to_child[0]); close(to_child[1]);
  close(from_child[0]); close(from_child[1]);
  execvp(argv[0], argv);
  _exit(127);
 }
 close(to_child[0]);              /* parent writes to to_child[1]   */
 close(from_child[1]);            /* parent reads from from_child[0] */
 if (pid < 0)
 {  close(to_child[1]); close(from_child[0]); return -1;  }
 *in_fd = to_child[1];
 *out_fd = from_child[0];
 return pid;
}

/* ---- the start: the handshake, one answer at a time ------------------- *
 *
 * Each step sends one request and waits for its answer; the answer sends the
 * next request.  The same steps run in both shapes the engine offers: driven
 * here with poll() until done (e_dap_open*, e_dap_run), or fed by the host's
 * event loop one readable descriptor at a time (e_dap_start*), so an editor
 * never waits on an adapter that does not answer.
 */

static void dap_send_configuration(e_dap_session *s);

/**
 * dap_send_initialize - Send the initialize request, the adapter's first.
 * @s: the session, connected.
 */
static void dap_send_initialize(e_dap_session *s)
{
 struct json_object *args = json_object_new_object();

 json_object_object_add(args, "clientID", json_object_new_string("xwpe"));
 json_object_object_add(args, "adapterID", json_object_new_string(s->adapter_id));
 json_object_object_add(args, "linesStartAt1", json_object_new_boolean(1));
 json_object_object_add(args, "columnsStartAt1", json_object_new_boolean(1));
 json_object_object_add(args, "pathFormat", json_object_new_string("path"));
 s->await_seq = dap_send(s, "initialize", args);
 s->step = DAP_STEP_INITIALIZE;
}

/**
 * dap_send_configuration_done - Tell the adapter the breakpoints are set.
 * @s: the session.
 *
 * gdb and lldb-dap start the program here.
 */
static void dap_send_configuration_done(e_dap_session *s)
{
 s->await_seq = dap_send(s, "configurationDone", NULL);
 s->step = DAP_STEP_CONFIGURED;
}

/**
 * dap_send_entry_breakpoint - Set the stop at the entry function (Go's
 * main.main, like gdb's "break main"), or go straight to configurationDone.
 * @s: the session.
 */
static void dap_send_entry_breakpoint(e_dap_session *s)
{
 struct json_object *args, *bps, *b;

 if (!s->entry_func)
 {
  dap_send_configuration_done(s);
  return;
 }
 args = json_object_new_object();
 bps = json_object_new_array();
 b = json_object_new_object();
 json_object_object_add(b, "name", json_object_new_string(s->entry_func));
 json_object_array_add(bps, b);
 json_object_object_add(args, "breakpoints", bps);
 s->await_seq = dap_send(s, "setFunctionBreakpoints", args);
 s->step = DAP_STEP_ENTRY;
}

/**
 * dap_send_source_breakpoints - Send the user's breakpoints.
 * @s: the session.
 */
static void dap_send_source_breakpoints(e_dap_session *s)
{
 struct json_object *args, *bps, *src;

 args = json_object_new_object();
 src = json_object_new_object();
 json_object_object_add(src, "path", json_object_new_string(s->bp_file));
 json_object_object_add(args, "source", src);
 bps = json_object_new_array();
 for (int i = 0; i < s->bp_n; i++)
 {
  struct json_object *b = json_object_new_object();
  json_object_object_add(b, "line", json_object_new_int(s->bp_lines[i]));
  json_object_array_add(bps, b);
 }
 json_object_object_add(args, "breakpoints", bps);
 s->await_seq = dap_send(s, "setBreakpoints", args);
 s->step = DAP_STEP_BREAKPOINTS;
}

/**
 * dap_send_configuration - Configure the launched program: its breakpoints,
 * then the entry stop, then configurationDone.
 * @s: the session.
 */
static void dap_send_configuration(e_dap_session *s)
{
 if (s->bp_n > 0)
 {
  dap_send_source_breakpoints(s);
  return;
 }
 dap_send_entry_breakpoint(s);
}

/**
 * dap_send_launch - Ask the adapter to launch the program.
 * @s: the session, its initialize answered.
 *
 * Run, not stop on entry: the stop comes from the breakpoints.  dlv adds
 * "mode":"debug" so it builds the package, and answers once it has built: the
 * breakpoints wait for that answer so they bind to the fresh binary.  gdb and
 * lldb-dap follow the specification and answer the launch only after
 * configurationDone, so the configuration goes out at once and their late
 * answer is dropped.  The Scala adapter (through Bloop) refuses breakpoints
 * until it has sent the `initialized` event, which is awaited first; its
 * launch names no program, the build server already chose the main class.
 */
static void dap_send_launch(e_dap_session *s)
{
 struct json_object *args = json_object_new_object();
 int seq;

 json_object_object_add(args, "request", json_object_new_string("launch"));
 if (!s->minimal_launch)
 {
  if (s->launch_mode_debug)
   json_object_object_add(args, "mode", json_object_new_string("debug"));
  json_object_object_add(args, "program", json_object_new_string(s->program));
  json_object_object_add(args, "stopOnEntry", json_object_new_boolean(0));
 }
 seq = dap_send(s, "launch", args);
 if (s->launch_mode_debug)
 {
  s->await_seq = seq;
  s->step = DAP_STEP_LAUNCH;
  return;
 }
 if (s->wait_init)
 {
  s->await_seq = DAP_NO_SEQ;
  s->step = DAP_STEP_INITIALIZED;
  return;
 }
 dap_send_configuration(s);
}

/**
 * dap_start_advance - The answer the current step waited for has come: send
 * the next request, or stop where the start's goal is.
 * @s: the session.
 */
static void dap_start_advance(e_dap_session *s)
{
 switch (s->step)
 {
  case DAP_STEP_INITIALIZE:
   s->step = DAP_STEP_OPENED;
   if (s->goal > DAP_STEP_OPENED)
    dap_send_launch(s);
   break;
  case DAP_STEP_LAUNCH:
  case DAP_STEP_INITIALIZED:
   dap_send_configuration(s);
   break;
  case DAP_STEP_BREAKPOINTS:
   dap_send_entry_breakpoint(s);
   break;
  case DAP_STEP_ENTRY:
   dap_send_configuration_done(s);
   break;
  case DAP_STEP_CONFIGURED:
   s->step = DAP_STEP_READY;
   dap_run_after_start(s);
   break;
  default:
   break;
 }
}

/**
 * dap_start_is_awaited - Is @m the answer the current step waits for?
 * @s: the session; @m: a message from the adapter.
 *
 * An event that ends the program while the Scala adapter's `initialized` is
 * awaited also lets the start go on, as the configuration is then moot.
 */
static int dap_start_is_awaited(e_dap_session *s, struct json_object *m)
{
 const char *type = obj_str(m, "type");
 const char *ev;

 if (!type)
  return 0;
 if (!strcmp(type, "response"))
  return s->await_seq != DAP_NO_SEQ &&
         obj_int(m, "request_seq", DAP_NO_SEQ) == s->await_seq;
 if (strcmp(type, "event") || s->step != DAP_STEP_INITIALIZED)
  return 0;
 ev = obj_str(m, "event");
 return ev && !strcmp(ev, "initialized");
}

/**
 * dap_start_refused - The adapter refused a step of the start: end it.
 * @s: the session; @m: the refusing response.
 *
 * Keeps the adapter's own words (dlv's build error, in body.error.format,
 * or the short "message") for the user, who sees why the program did not
 * start instead of a session that waits for a stop that never comes.
 */
static void dap_start_refused(e_dap_session *s, struct json_object *m)
{
 free(s->start_error);
 s->start_error = strdup(dap_refusal_text(m));
 s->start_failed = 1;
}

/**
 * dap_start_note_breakpoints - The launch's setBreakpoints was answered:
 * pass on which breakpoints the adapter took.
 * @s: the session; @m: the answer.
 *
 * Keeps the adapter's ids too, so a breakpoint gdb reports pending at the
 * launch is known when gdb later says it is set.
 */
static void dap_start_note_breakpoints(e_dap_session *s, struct json_object *m)
{
 if (s->step != DAP_STEP_BREAKPOINTS)
  return;
 dap_breakpoints_answered(s, s->bp_file, s->bp_lines, s->bp_n, m);
}

/**
 * dap_start_feed - Take one message during the start.
 * @s: the session; @m: the message (not freed here).
 *
 * The awaited answer moves the start on, unless it is a refusal, which ends
 * the start; any other message goes to the dispatcher as at any other time
 * (gdb's late launch answer has no owner and is dropped there).
 */
static void dap_start_feed(e_dap_session *s, struct json_object *m)
{
 int ended_before = s->ended;

 if (dap_start_is_awaited(s, m) && dap_response_refused(m))
 {
  dap_start_refused(s, m);
  return;
 }
 if (dap_start_is_awaited(s, m))
 {
  dap_start_note_breakpoints(s, m);
  dap_start_advance(s);
  return;
 }
 dap_dispatch(s, m);
 if (!ended_before && s->ended && s->step == DAP_STEP_INITIALIZED)
  dap_start_advance(s);
}

/**
 * dap_start_status - Where the start stands.
 * @s: the session.
 * Return: E_DAP_START_READY at its goal, E_DAP_START_FAILED when the adapter
 * hung up or never dialled in, E_DAP_START_WAITING otherwise.
 */
static int dap_start_status(const e_dap_session *s)
{
 if (s->start_failed)
  return E_DAP_START_FAILED;
 if (s->step >= s->goal)
  return E_DAP_START_READY;
 return E_DAP_START_WAITING;
}

/**
 * dap_start_consume - Take the messages already read, up to the start's goal.
 * @s: the session.
 *
 * Messages past the goal stay queued: the first stop, for one, is read by
 * e_dap_run_to_first_stop.
 * Return: dap_start_status.
 */
static int dap_start_consume(e_dap_session *s)
{
 while (dap_start_status(s) == E_DAP_START_WAITING &&
        s->step != DAP_STEP_ACCEPT)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);

  if (!m)
   break;
  dap_start_feed(s, m);
  json_object_put(m);
 }
 if (s->step == DAP_STEP_READY)
  dap_drain_queue(s);
 return dap_start_status(s);
}

/**
 * dap_start_fail - The adapter hung up, or never dialled in.
 * @s: the session.
 * Return: E_DAP_START_FAILED.
 */
static int dap_start_fail(e_dap_session *s)
{
 s->start_failed = 1;
 s->ended = 1;
 return E_DAP_START_FAILED;
}

/**
 * dap_start_hung_up - The adapter closed the transport during the start.
 * @s: the session.
 *
 * The transport is not read again, so a host that keeps watching it is
 * never handed the same end twice.
 * Return: E_DAP_START_FAILED.
 */
static int dap_start_hung_up(e_dap_session *s)
{
 s->eof = 1;
 return dap_start_fail(s);
}

/**
 * dap_start_accept - The reverse-TCP adapter dialled in: take the connection
 * and send it the initialize request.
 * @s: the session, waiting on its listening socket.
 * Return: dap_start_status.
 */
static int dap_start_accept(e_dap_session *s)
{
 s->sock = accept(s->listen_fd, NULL, NULL);
 close(s->listen_fd);
 s->listen_fd = DAP_NO_FD;
 if (s->sock < 0)
  return dap_start_fail(s);
 dap_send_initialize(s);
 return dap_start_status(s);
}

/**
 * dap_start_wait - Wait, with poll(), until the start's descriptor is
 * readable, passing on the program's output meanwhile.
 * @s: the session.
 *
 * Used where the caller has nothing else to do (a test, the synchronous
 * API).  The adapter gets DAP_ACCEPT_MS to dial in, as before.
 * Return: 0 when readable, -1 when the adapter did not dial in, poll failed
 * or the editor's end was asked for.
 */
static int dap_start_wait(e_dap_session *s)
{
 long deadline = DAP_NO_DEADLINE;

 if (s->step == DAP_STEP_ACCEPT)
  deadline = dap_now_ms() + DAP_ACCEPT_MS;
 if (dap_wait_readable(s, e_dap_start_fd(s), deadline) != DAP_WAIT_READY)
  return -1;
 return 0;
}

/**
 * dap_start_drive - Run the start to its goal, waiting with poll().
 * @s: the session.
 * Return: E_DAP_START_READY or E_DAP_START_FAILED.
 */
static int dap_start_drive(e_dap_session *s)
{
 int st = dap_start_consume(s);

 while (st == E_DAP_START_WAITING)
 {
  if (dap_start_wait(s) < 0)
   return dap_start_fail(s);
  st = e_dap_start_pump(s);
 }
 return st;
}

/**
 * e_dap_start_pump - Go on with the start: the start's descriptor is readable.
 * @s: the session.
 *
 * Reads once, so it never blocks when called on readiness; the host calls it
 * from its event loop each time e_dap_start_fd(s) wakes it.
 * Return: E_DAP_START_WAITING, E_DAP_START_READY or E_DAP_START_FAILED.
 */
int e_dap_start_pump(e_dap_session *s)
{
 char buf[DAP_READ_CHUNK];
 ssize_t n;

 if (!s)
  return E_DAP_START_FAILED;
 if (dap_start_status(s) != E_DAP_START_WAITING)
  return dap_start_status(s);
 if (s->step == DAP_STEP_ACCEPT)
  return dap_start_accept(s);
 n = read(dap_rfd(s), buf, sizeof(buf));
 if (n < 0 && (errno == EINTR || errno == EAGAIN))
  return dap_start_consume(s);
 if (n <= 0)
  return dap_start_hung_up(s);
 e_dap_reader_push(&s->rd, buf, (size_t)n);
 return dap_start_consume(s);
}

/**
 * e_dap_start_fd - The descriptor the start waits on.
 * @s: the session.
 *
 * The listening socket until a reverse-TCP adapter has dialled in, then the
 * DAP transport; the host watches it and calls e_dap_start_pump.
 */
int e_dap_start_fd(const e_dap_session *s)
{
 if (!s)
  return DAP_NO_FD;
 if (s->step == DAP_STEP_ACCEPT)
  return s->listen_fd;
 return s->sock >= 0 ? s->sock : s->out_fd;
}

/**
 * e_dap_output_fd - The pty the debuggee writes to (reverse TCP), or -1.
 * @s: the session.
 */
int e_dap_output_fd(const e_dap_session *s)
{
 return s ? s->pty_master : DAP_NO_FD;
}

/**
 * e_dap_start_answered - Has the adapter answered initialize?
 * @s: the session.
 *
 * An adapter that has answered is alive: what it does next (dlv building
 * the package) may take as long as it needs, so a host stops counting a
 * deadline from here and leaves the end to the user.
 */
int e_dap_start_answered(const e_dap_session *s)
{
 return s && s->step >= DAP_STEP_OPENED;
}

/**
 * e_dap_start_waits_for - What the start waits for, in a word for the user.
 * @s: the session.
 *
 * "starting" until the adapter has answered, then "building" while dlv
 * builds the package, "launching" while the program is launched, and
 * "configuring" while the breakpoints are set.
 */
const char *e_dap_start_waits_for(const e_dap_session *s)
{
 if (!e_dap_start_answered(s))
  return "starting";
 if (s->step == DAP_STEP_LAUNCH && s->launch_mode_debug)
  return "building";
 if (s->step <= DAP_STEP_INITIALIZED)
  return "launching";
 return "configuring";
}

/**
 * e_dap_start_error - Why the adapter refused the start, in its own words,
 * or NULL when it did not refuse.
 * @s: the session.
 */
const char *e_dap_start_error(const e_dap_session *s)
{
 return s ? s->start_error : NULL;
}

/**
 * e_dap_adapter_pid - The adapter's process, when it is this process's own
 * child; 0 for an adapter hosted by a build server.
 * @s: the session.
 */
pid_t e_dap_adapter_pid(const e_dap_session *s)
{
 if (!s || !s->spawned)
  return 0;
 return s->pid;
}

/**
 * e_dap_signal - Send @sig to the adapter, by its exact PID.
 * @s: the session; @sig: SIGTERM, then SIGKILL.
 *
 * Nothing is sent once the adapter has been reaped, so a PID the system gave
 * to another process is never signalled.
 */
void e_dap_signal(e_dap_session *s, int sig)
{
 if (e_dap_adapter_pid(s) > 0)
  kill(s->pid, sig);
}

/**
 * e_dap_adapter_gone - Has the adapter exited?
 * @s: the session.
 *
 * Reaps it without waiting; a host whose SIGCHLD handler reaped it first
 * sees ECHILD, which also means gone.  Its PID is then forgotten.  An adapter
 * that is not this process's child is never "gone" here.
 * Return: 1 once the adapter is gone.
 */
int e_dap_adapter_gone(e_dap_session *s)
{
 int st, r;

 if (!s || !s->spawned)
  return 0;
 if (s->pid <= 0)
  return 1;
 r = waitpid(s->pid, &st, WNOHANG);
 if (r == s->pid || (r < 0 && errno == ECHILD))
 {
  s->pid = 0;
  return 1;
 }
 return 0;
}

/* Allocate a session and set the transport-neutral defaults. */
static e_dap_session *dap_session_new(const char *program, const char *entry_func,
                                      const e_dap_host *host)
{
 e_dap_session *s = calloc(1, sizeof(*s));
 if (!s)
  return NULL;
 e_dap_reader_init(&s->rd);
 if (host)
  s->host = *host;
 s->program = program ? strdup(program) : NULL;
 s->entry_func = entry_func ? strdup(entry_func) : NULL;
 s->thread_id = 1;
 s->sock = -1;
 s->in_fd = -1;
 s->out_fd = -1;
 s->pty_master = -1;
 s->listen_fd = DAP_NO_FD;
 s->step = DAP_STEP_ACCEPT;
 s->goal = DAP_STEP_READY;
 s->await_seq = DAP_NO_SEQ;
 s->run = DAP_RUN_STARTING;
 return s;
}

/**
 * dap_open_finish - Drive a session the synchronous API opened to the
 * initialize answer.
 * @s: the session, its start begun (or NULL).
 * Return: the session, or NULL (and closed) when the adapter did not answer.
 */
static e_dap_session *dap_open_finish(e_dap_session *s)
{
 if (!s)
  return NULL;
 s->goal = DAP_STEP_OPENED;
 if (dap_start_drive(s) != E_DAP_START_READY)
 {
  e_dap_close(s);
  return NULL;
 }
 return s;
}

/* ---- public API -------------------------------------------------------- */

/**
 * e_dap_start - Spawn a reverse-TCP adapter (dlv) and begin its start,
 * without waiting for it.
 *
 * The host adds its breakpoints, watches e_dap_start_fd and calls
 * e_dap_start_pump until it answers E_DAP_START_READY, then
 * e_dap_run_to_first_stop.  Returns the session, or NULL when the adapter
 * could not be spawned.
 */
e_dap_session *e_dap_start(char *const argv[], const char *program,
                           const char *entry_func, const char *cwd,
                           const e_dap_host *host)
{
 e_dap_session *s;
 int port;

 s = dap_session_new(program, entry_func, host);
 if (!s)
  return NULL;
 s->launch_mode_debug = 1;        /* dlv launch arg "mode":"debug" (builds pkg) */
 s->adapter_id = "dap";

 s->listen_fd = dap_listen_ephemeral(&port);
 if (s->listen_fd < 0)
 {
  e_dap_close(s);
  return NULL;
 }
 /* Create the adapter's controlling pty here in the parent: the child takes the
    slave as its stdio/ctty, and we keep the master to read the debuggee's
    output from (forwarded to the host's Messages window). */
 {
  int slave = -1;
  if (openpty(&s->pty_master, &slave, NULL, NULL, NULL) != 0)
   s->pty_master = slave = -1;
  else
  {
   /* Raw mode: no OPOST/ONLCR, so the debuggee's "\n" is not cooked into
      "\r\n" -- otherwise every program line would show a trailing ^M in the
      Messages window. */
   struct termios tio;
   if (tcgetattr(s->pty_master, &tio) == 0)
   {
    cfmakeraw(&tio);
    tcsetattr(s->pty_master, TCSANOW, &tio);
   }
  }
  s->pid = dap_spawn(argv, port, cwd, s->listen_fd, slave);
  if (slave >= 0)
   close(slave);                /* the child holds its own copy */
 }
 if (s->pid <= 0)
 {
  e_dap_close(s);
  return NULL;
 }
 s->spawned = 1;
 return s;
}

e_dap_session *e_dap_open(char *const argv[], const char *program,
                          const char *entry_func, const char *cwd,
                          const e_dap_host *host)
{
 return dap_open_finish(e_dap_start(argv, program, entry_func, cwd, host));
}

/**
 * e_dap_start_stdio - Spawn a stdio adapter (gdb or lldb-dap speaking DAP)
 * and send it the initialize request, without waiting for the answer.
 *
 * `program` is a PREBUILT binary (the adapter launches it; it does not
 * build), `entry_func` may be NULL.  The host goes on as for e_dap_start.
 */
e_dap_session *e_dap_start_stdio(char *const argv[], const char *program,
                                 const char *entry_func, const char *cwd,
                                 const e_dap_host *host)
{
 e_dap_session *s = dap_session_new(program, entry_func, host);
 if (!s)
  return NULL;
 s->launch_mode_debug = 0;        /* program is a prebuilt binary, not a package */
 s->adapter_id = "gdb";
 s->pid = dap_spawn_stdio(argv, cwd, &s->in_fd, &s->out_fd);
 if (s->pid <= 0)
 {
  e_dap_close(s);
  return NULL;
 }
 s->spawned = 1;
 dap_send_initialize(s);
 return s;
}

/* Stdio transport: spawn `argv` (e.g. {"gdb","--interpreter=dap"}), talk DAP
   over its pipes, run the initialize handshake. */
e_dap_session *e_dap_open_stdio(char *const argv[], const char *program,
                                const char *entry_func, const char *cwd,
                                const e_dap_host *host)
{
 return dap_open_finish(e_dap_start_stdio(argv, program, entry_func, cwd, host));
}

/**
 * e_dap_start_tcp - Connect to a running DAP server (the Scala adapter that
 * Bloop hosts) and send it the initialize request, without waiting for the
 * answer.
 *
 * The connection is to a local endpoint the build server has just opened,
 * so connect() returns at once.  No spawn and no pty: the build server owns
 * the debuggee, and its output arrives as DAP "output" events.  The host
 * goes on as for e_dap_start.
 */
e_dap_session *e_dap_start_tcp(const char *host, int port, const char *program,
                               const char *entry_func, const e_dap_host *host_cb)
{
 e_dap_session *s = dap_session_new(program, entry_func, host_cb);
 struct sockaddr_in a;
 int fd;

 if (!s)
  return NULL;
 s->launch_mode_debug = 0;        /* build server already bound the main class */
 s->minimal_launch = 1;           /* launch carries no program/mode            */
 s->wait_init = 1;                /* strict order: wait `initialized` event     */
 s->adapter_id = "scala";

 fd = socket(AF_INET, SOCK_STREAM, 0);
 if (fd < 0)
 {  e_dap_close(s);  return NULL;  }
 memset(&a, 0, sizeof(a));
 a.sin_family = AF_INET;
 a.sin_port = htons((unsigned short)port);
 if (inet_pton(AF_INET, host, &a.sin_addr) != 1)
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
 {  close(fd);  e_dap_close(s);  return NULL;  }
 s->sock = fd;
 dap_send_initialize(s);
 return s;
}

/* Forward TCP client to an already-running DAP server (Bloop's scala-debug-
   adapter, address from BSP debugSession/start), initialize answered. */
e_dap_session *e_dap_open_tcp(const char *host, int port, const char *program,
                              const char *entry_func, const e_dap_host *host_cb)
{
 return dap_open_finish(e_dap_start_tcp(host, port, program, entry_func, host_cb));
}

int e_dap_add_breakpoint(e_dap_session *s, const char *file, int line)
{
 char abspath[PATH_MAX];
 if (!s || s->bp_n >= DAP_MAX_BP)
  return -1;
 if (!s->bp_file[0])
 {
  /* Canonicalize the source path: on macOS the build server compiles from the
     realpath()'d directory (e.g. /private/tmp/...), so a setBreakpoints with
     the original /tmp/... path is silently dropped as "unknown source". */
  if (!realpath(file, abspath))
   snprintf(abspath, sizeof(abspath), "%s", file);
  snprintf(s->bp_file, sizeof(s->bp_file), "%s", abspath);
 }
 s->bp_lines[s->bp_n++] = line;
 return 0;
}

/**
 * dap_breakpoints_args - The arguments of a setBreakpoints request.
 * @path: the file's absolute path; @lines, @n: every breakpoint line of it.
 */
static struct json_object *dap_breakpoints_args(const char *path,
                                                const int *lines, int n)
{
 struct json_object *args = json_object_new_object();
 struct json_object *src = json_object_new_object();
 struct json_object *bps = json_object_new_array();
 int i;

 json_object_object_add(src, "path", json_object_new_string(path));
 json_object_object_add(args, "source", src);
 for (i = 0; i < n; i++)
 {
  struct json_object *b = json_object_new_object();
  json_object_object_add(b, "line", json_object_new_int(lines[i]));
  json_object_array_add(bps, b);
 }
 json_object_object_add(args, "breakpoints", bps);
 return args;
}

/**
 * dap_absolute_path - The path the adapter knows @file by.
 * @file: as the host names it; @out: receives it (PATH_MAX bytes).
 *
 * On macOS the build server compiles from the realpath()'d directory, so a
 * path through a symlink would be dropped as an unknown source.
 */
static void dap_absolute_path(const char *file, char *out)
{
 if (!realpath(file, out))
  snprintf(out, PATH_MAX, "%s", file);
}

/**
 * dap_pending_keep_lines - Keep the file and lines a setBreakpoints asked
 * for, to name them when its answer comes.
 * @p: the request's slot; @path, @lines, @n: what was asked for.
 *
 * Without memory for them the answer is still taken, but not told.
 */
static void dap_pending_keep_lines(dap_pending *p, const char *path,
                                   const int *lines, int n)
{
 p->file = strdup(path);
 p->lines = malloc(sizeof(int) * (size_t)(n + 1));
 if (!p->file || !p->lines)
 {
  free(p->file);
  free(p->lines);
  p->file = NULL;
  p->lines = NULL;
  return;
 }
 if (n > 0)
  memcpy(p->lines, lines, sizeof(int) * (size_t)n);
 p->n = n;
}

/**
 * e_dap_set_file_breakpoints - Replace one file's breakpoints while the
 * program is stopped.
 * @s: the session; @file: the source file; @lines: its breakpoint lines;
 * @n: how many.
 *
 * DAP has no run-to-cursor request, so Go to cursor adds the cursor's line
 * to the file's breakpoints, continues, and sends the list again without it
 * at the next stop.  setBreakpoints replaces every breakpoint of the file,
 * which is why the whole list is sent each time.  The answer is also told
 * to on_breakpoints.
 * Return: 0 when the adapter answered, -1 otherwise.
 */
int e_dap_set_file_breakpoints(e_dap_session *s, const char *file,
                               const int *lines, int n)
{
 struct json_object *resp;
 char abspath[PATH_MAX];
 int seq;

 if (!s || s->ended || n < 0 || n > DAP_MAX_BP)
  return -1;
 dap_absolute_path(file, abspath);
 seq = dap_send_owned(s, "setBreakpoints",
                      dap_breakpoints_args(abspath, lines, n), DAP_REQ_WAITED);
 resp = dap_wait_answer(s, seq);
 if (!resp)
  return -1;
 dap_breakpoints_answered(s, abspath, lines, n, resp);
 json_object_put(resp);
 return 0;
}

/**
 * e_dap_set_file_breakpoints_async - Replace one file's breakpoints without
 * waiting for the adapter.
 * @s: the session; @file: the source file; @lines, @n: all its breakpoints.
 *
 * A breakpoint the user toggles while the program runs reaches the adapter
 * at once (gdb-dap, lldb-dap and dlv all take it while running), and the
 * answer, verified or pending, comes later through on_breakpoints.
 * Return: 0 when sent, -1 otherwise.
 */
int e_dap_set_file_breakpoints_async(e_dap_session *s, const char *file,
                                     const int *lines, int n)
{
 char abspath[PATH_MAX];
 int seq;

 if (!s || s->ended || s->eof || s->step < DAP_STEP_READY)
  return -1;
 if (s->run == DAP_RUN_DISCONNECTING || n < 0 || n > DAP_MAX_BP)
  return -1;
 dap_absolute_path(file, abspath);
 seq = dap_send_owned(s, "setBreakpoints",
                      dap_breakpoints_args(abspath, lines, n),
                      DAP_REQ_BREAKPOINTS);
 if (seq == DAP_NO_SEQ)
  return -1;
 dap_pending_keep_lines(dap_pending_find(s, seq), abspath, lines, n);
 return 0;
}

int e_dap_run(e_dap_session *s)
{
 if (!s)
  return -1;
 s->goal = DAP_STEP_READY;
 if (s->step == DAP_STEP_OPENED)
  dap_send_launch(s);
 if (dap_start_drive(s) != E_DAP_START_READY)
 {
  s->forward_output = 1;
  return s->ended ? 0 : -1;
 }
 return e_dap_run_to_first_stop(s);
}

/**
 * e_dap_run_to_first_stop - The handshake is done: wait for the program's
 * first stop and report it.
 * @s: the session, its start at E_DAP_START_READY.
 *
 * Fires host->on_stopped with the source line, or on_terminated when the
 * program ended first.
 * Return: 0, or -1 when the adapter hung up without either.
 */
int e_dap_run_to_first_stop(e_dap_session *s)
{
 int st;

 if (!s)
  return -1;
 if (s->ended)
 {
  s->forward_output = 1;
  return 0;
 }
 st = dap_run_wait(s, DAP_NO_DEADLINE);
 /* Program output is forwarded from here on even without a stop (the
    program ended first); dap_tell_stopped did it for a stop. */
 s->forward_output = 1;
 if (st == E_DAP_RUN_STOPPED)
  return 0;
 return s->ended ? 0 : -1;
}

/**
 * e_dap_resume - Let the stopped program go on: continue or one step.
 * @s: the session; @verb: "continue", "next", "stepIn" or "stepOut".
 *
 * Returns at once; the stop or the end comes through e_dap_run_pump, so
 * the editor takes keys while the program runs.
 * Return: 0 when sent, -1 when the program is not stopped or has ended.
 */
int e_dap_resume(e_dap_session *s, const char *verb)
{
 struct json_object *args;

 if (!s || s->ended || s->eof || s->run != DAP_RUN_STOPPED)
  return -1;
 args = json_object_new_object();
 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 if (dap_send_owned(s, verb, args, DAP_REQ_RESUME) == DAP_NO_SEQ)
  return -1;
 s->run = DAP_RUN_GOING;
 s->stop_reported = 0;
 return 0;
}

int e_dap_step(e_dap_session *s, const char *verb)
{
 if (e_dap_resume(s, verb) != 0)
  return -1;
 dap_run_wait(s, DAP_NO_DEADLINE);
 return 0;       /* ended is normal for `continue` past the last breakpoint */
}

/**
 * dap_run_pump_start - The pump was called before the start was done: go
 * on with the start, and with the run once the start is ready.
 * @s: the session.
 */
static int dap_run_pump_start(e_dap_session *s)
{
 int st;

 /* The start pump reads (or accepts) blindly, on readiness; the run pump
    may be called at any time, so it looks first. */
 if (!dap_fd_readable_now(e_dap_start_fd(s)))
  return E_DAP_RUN_GOING;
 st = e_dap_start_pump(s);

 if (st == E_DAP_START_FAILED)
  return E_DAP_RUN_ENDED;
 if (st == E_DAP_START_READY)
  return dap_run_consume(s);
 return E_DAP_RUN_GOING;
}

/**
 * e_dap_run_pump - Follow the running program: read what the adapter sent.
 * @s: the session; called when its descriptor (e_dap_fd) is readable, and
 * once right after the start is ready.
 *
 * Reads once, and only when there is something to read, so it never
 * blocks; it takes every message read, so nothing waits for the adapter to
 * write again: output
 * goes to Messages, breakpoint answers to on_breakpoints, a stop is located
 * and told to on_stopped.  At the end of the transport (the adapter died,
 * even in the middle of a message) the run ends; the transport is not read
 * again.  Called before the start is done, it goes on with the start.
 * Return: E_DAP_RUN_GOING, E_DAP_RUN_STOPPED or E_DAP_RUN_ENDED.
 */
int e_dap_run_pump(e_dap_session *s)
{
 if (!s)
  return E_DAP_RUN_ENDED;
 if (dap_start_status(s) == E_DAP_START_WAITING)
  return dap_run_pump_start(s);
 if (dap_start_status(s) == E_DAP_START_FAILED)
  return E_DAP_RUN_ENDED;
 if (dap_transport_readable_now(s))
  dap_read_once(s);
 return dap_run_consume(s);
}

/**
 * e_dap_fd - The descriptor the run is read from.
 * @s: the session.
 *
 * The DAP transport once connected; -1 before a reverse-TCP adapter has
 * dialled in and after the transport ended.
 */
int e_dap_fd(const e_dap_session *s)
{
 if (!s || s->eof || s->step == DAP_STEP_ACCEPT)
  return DAP_NO_FD;
 if (s->sock >= 0)
  return s->sock;
 return s->out_fd;
}

/**
 * e_dap_running - Is the program running (resumed, no stop told yet)?
 * @s: the session.
 *
 * The editor refuses what needs a stopped program (a step, the stack)
 * while this is true.
 */
int e_dap_running(const e_dap_session *s)
{
 if (!s || s->ended)
  return 0;
 return s->run == DAP_RUN_GOING || s->run == DAP_RUN_LOCATING;
}

/**
 * e_dap_pause - Ask the adapter to stop the running program.
 * @s: the session.
 *
 * The user pressed Pause: the stop that follows is told as "pause" whatever
 * reason the adapter gives, so the editor never takes it for a breakpoint
 * or an error.  A refused pause is forgotten.
 * Return: 0 when sent, -1 when the program is not running.
 */
int e_dap_pause(e_dap_session *s)
{
 struct json_object *args;

 if (!s || s->ended || s->eof || s->run != DAP_RUN_GOING)
  return -1;
 args = json_object_new_object();
 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 if (dap_send_owned(s, "pause", args, DAP_REQ_PAUSE) == DAP_NO_SEQ)
  return -1;
 s->pause_pending = 1;
 return 0;
}

char *e_dap_evaluate(e_dap_session *s, const char *expr)
{
 struct json_object *args, *resp, *body;
 char *out = NULL;
 int seq;

 if (!s || s->ended)
  return NULL;
 args = json_object_new_object();
 json_object_object_add(args, "expression", json_object_new_string(expr));
 json_object_object_add(args, "frameId", json_object_new_int(s->frame_id));
 json_object_object_add(args, "context", json_object_new_string("watch"));
 seq = dap_send_owned(s, "evaluate", args, DAP_REQ_WAITED);
 resp = dap_wait_answer(s, seq);
 body = obj_obj(resp, "body");
 if (body)
 {
  const char *r = obj_str(body, "result");
  if (r)
   out = strdup(r);
 }
 if (resp)
  json_object_put(resp);
 return out;
}

int e_dap_ended(const e_dap_session *s)
{
 return s ? s->ended : 1;
}

/**
 * dap_disconnect_args - The arguments of Quit's `disconnect`: end the
 * program too, so none is left behind.
 */
static struct json_object *dap_disconnect_args(void)
{
 struct json_object *args = json_object_new_object();

 json_object_object_add(args, "terminateDebuggee",
                        json_object_new_boolean(1));
 return args;
}

/**
 * e_dap_disconnect - Quit: ask the adapter to end the program and exit.
 * @s: the session.
 *
 * Returns at once.  The pump answers E_DAP_RUN_ENDED once the adapter has
 * answered (or its transport ended); the host counts its deadline and shows
 * its status line meanwhile, and e_dap_close then waits, bounded, for the
 * adapter's exit.  An adapter that never answered initialize is not
 * spoken to: it would never read the request, and is ended by signal.
 * Return: 0 when sent (or already sent), -1 when there is no adapter to
 * speak to.
 */
int e_dap_disconnect(e_dap_session *s)
{
 if (!s)
  return -1;
 if (s->run == DAP_RUN_DISCONNECTING)
  return 0;
 if (s->eof || s->step < DAP_STEP_OPENED || dap_wfd(s) < 0)
  return -1;
 s->run = DAP_RUN_DISCONNECTING;
 s->pause_pending = 0;
 if (dap_send_owned(s, "disconnect", dap_disconnect_args(),
                    DAP_REQ_DISCONNECT) != DAP_NO_SEQ)
  return 0;
 /* No slot for its answer: sent all the same, and the adapter's exit (the
    end of its output) ends the wait instead. */
 dap_send(s, "disconnect", dap_disconnect_args());
 dap_close_write_side(s);
 return 0;
}

/**
 * dap_quit_and_wait - Quit the adapter and wait, briefly, for it to go.
 * @s: the session.
 *
 * Used when the host closes a session it did not disconnect itself: the
 * editor's quit while stopped, a test, the editor's own end.  The wait
 * ends at the adapter's answer or the end of its transport, or at
 * DAP_DISCONNECT_MS.  The editor's end request does not cut it short:
 * lldb-dap ends a running program only on `disconnect`, and giving up here
 * sends it SIGTERM before it has acted on it, so the program stays behind
 * (measured on a running lldb-dap program, 5 of 5).  The adapter's exit is
 * waited for by dap_reap_adapter.
 */
static void dap_quit_and_wait(e_dap_session *s)
{
 if (s->run == DAP_RUN_DISCONNECTING)
  return;                        /* the host disconnected and waited */
 if (e_dap_disconnect(s) != 0)
  return;
 s->quitting = 1;
 dap_run_wait(s, dap_now_ms() + DAP_DISCONNECT_MS);
}

/**
 * dap_close_fds - Close every descriptor the session holds.
 * @s: the session.
 */
static void dap_close_fds(e_dap_session *s)
{
 if (s->listen_fd >= 0)
  close(s->listen_fd);
 if (s->sock >= 0)
  close(s->sock);
 if (s->in_fd >= 0)
  close(s->in_fd);
 if (s->out_fd >= 0)
  close(s->out_fd);
 if (s->pty_master >= 0)
  close(s->pty_master);
 s->listen_fd = s->sock = s->in_fd = s->out_fd = s->pty_master = DAP_NO_FD;
}

#if defined(DAP_NO_PIDFD)
/* The test build of a system with neither pidfd nor kqueue. */
#elif defined(__linux__) && defined(SYS_pidfd_open)
# define DAP_EXIT_BY_PIDFD 1
#elif defined(HAVE_KQUEUE) && defined(HAVE_SYS_EVENT_H)
# define DAP_EXIT_BY_KQUEUE 1
#endif

#if defined(DAP_EXIT_BY_KQUEUE)
/**
 * dap_kqueue_exit_open - The BSD and macOS form of a pidfd: a kqueue that
 * becomes readable when @pid exits (EVFILT_PROC, NOTE_EXIT).
 * @pid: the adapter, the editor's own child.
 * Return: the descriptor, or -1 (the adapter already exited, or no room).
 */
static int dap_kqueue_exit_open(pid_t pid)
{
 struct kevent ev;
 int kq = kqueue();

 if (kq < 0)
  return DAP_NO_FD;
 EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, NULL);
 if (kevent(kq, &ev, 1, NULL, 0, NULL) < 0)
 {
  close(kq);
  return DAP_NO_FD;
 }
 return kq;
}
#endif

/**
 * dap_exit_fd_open - A descriptor that becomes readable when @pid exits:
 * a pidfd on Linux, a kqueue on the BSDs and macOS, or -1 where the system
 * has neither (then the end of the adapter's output is waited for).
 * @pid: the adapter, the editor's own child.
 *
 * With it, Quit reaps an adapter the moment it exits, never later.
 */
static int dap_exit_fd_open(pid_t pid)
{
#if defined(DAP_EXIT_BY_PIDFD)
 return (int)syscall(SYS_pidfd_open, pid, 0);
#elif defined(DAP_EXIT_BY_KQUEUE)
 return dap_kqueue_exit_open(pid);
#else
 (void)pid;
 return DAP_NO_FD;
#endif
}

/**
 * dap_exit_grace_wait - With no exit descriptor, the adapter has closed
 * every descriptor it held: it may be exiting, or still cleaning up after
 * SIGTERM.  Give it what is left of @deadline, then look once.
 * @s: the session; @deadline: a dap_now_ms() instant.
 *
 * The engine has no other wake for a child's exit here: the editor's
 * SIGCHLD wake (WpeChildWatch) delivers through the editor's loop, which
 * is not running while e_dap_close waits.  So the rest of the bound is
 * waited out on the end request alone (the user's work is saved if it
 * comes), and a single waitpid decides.  Only systems with neither pidfd
 * nor kqueue reach this, and only after the adapter closed all its
 * descriptors: there a Quit can take the whole bound.
 * Return: 1 once the adapter is gone (and reaped), else 0.
 */
static int dap_exit_grace_wait(e_dap_session *s, long deadline)
{
 for (;;)
 {
  struct pollfd p[1];
  int np = 0, end_i, r;

  if (e_dap_adapter_gone(s))
   return 1;
  end_i = dap_quit_end_watch_add(p, &np);
  r = poll(p, np, dap_ms_left(deadline));
  if (r < 0 && errno == EINTR)
   continue;
  dap_quit_end_check(dap_poll_fired(p, end_i, r));
  if (r <= 0)
   return e_dap_adapter_gone(s);
 }
}

/**
 * dap_exit_watch - Wait until the adapter has exited, or @deadline.
 * @s: the session; @exit_fd: from dap_exit_fd_open, or -1; @deadline: a
 * dap_now_ms() instant.
 *
 * What the adapter still writes meanwhile is read and dropped, so it never
 * blocks on a full pipe while it exits; an adapter that writes without end
 * still cannot hold the wait past @deadline.  Without an exit descriptor,
 * the end of every descriptor the adapter holds stands for "may have
 * exited", and the rest of the bound is given by dap_exit_grace_wait.  The
 * end request does not cut this wait short (it is bounded, and it is what
 * keeps a program from being left behind when the editor ends), but when
 * it comes the user's work is saved at once.
 * Return: 1 once the adapter is gone (and reaped), 0 at the deadline.
 */
static int dap_exit_watch(e_dap_session *s, int exit_fd, long deadline)
{
 for (;;)
 {
  struct pollfd p[DAP_EXIT_FDS];
  int np = 0, pty_i = -1, rd_i = -1, end_i, r;

  if (e_dap_adapter_gone(s))
   return 1;
  if (dap_ms_left(deadline) == 0)
   return 0;
  if (exit_fd >= 0)
  {
   p[np].fd = exit_fd; p[np].events = POLLIN; p[np].revents = 0; np++;
  }
  if (s->pty_master >= 0)
  {
   pty_i = np;
   p[np].fd = s->pty_master; p[np].events = POLLIN; p[np].revents = 0;
   np++;
  }
  if (!s->eof && dap_rfd(s) >= 0)
  {
   rd_i = np;
   p[np].fd = dap_rfd(s); p[np].events = POLLIN; p[np].revents = 0; np++;
  }
  if (np == 0)
   return dap_exit_grace_wait(s, deadline);
  end_i = dap_quit_end_watch_add(p, &np);
  r = poll(p, np, dap_ms_left(deadline));
  if (r < 0 && errno == EINTR)
   continue;
  dap_quit_end_check(dap_poll_fired(p, end_i, r));
  if (r <= 0)
   return e_dap_adapter_gone(s);
  if (pty_i >= 0 && p[pty_i].revents)
   e_dap_drain_output(s);
  if (rd_i >= 0 && p[rd_i].revents)
   dap_discard_once(s);
 }
}

/**
 * dap_wait_adapter_exit - Give the adapter up to @ms to exit by itself.
 * @s: the session; @ms: the bound.
 * Return: 1 once it is gone and reaped, else 0.
 */
static int dap_wait_adapter_exit(e_dap_session *s, int ms)
{
 int exit_fd = dap_exit_fd_open(s->pid);
 int gone = dap_exit_watch(s, exit_fd, dap_now_ms() + ms);

 if (exit_fd >= 0)
  close(exit_fd);
 return gone;
}

/**
 * dap_reap_adapter - Make sure the adapter is gone, and reap it.
 * @s: the session.
 *
 * An adapter that answered Quit is given DAP_EXIT_MS to exit by itself
 * and is then only reaped: dlv closes its socket before it is done, so the
 * end of the transport is not taken for its exit.  One still there gets
 * SIGTERM and DAP_TERM_MS, so it can end the program it runs (a SIGKILLed
 * lldb-dap or dlv leaves its program behind), and SIGKILL after that, so
 * the user is never left with an editor that does not quit.  Every wait is
 * a poll on the adapter's exit, bounded, never a sleep.
 */
static void dap_reap_adapter(e_dap_session *s)
{
 int st;

 if (!s->spawned || s->pid <= 0)
  return;
 dap_close_write_side(s);
 if (s->disconnect_done && dap_wait_adapter_exit(s, DAP_EXIT_MS))
  return;
 if (e_dap_adapter_gone(s))
  return;
 kill(s->pid, SIGTERM);
 if (dap_wait_adapter_exit(s, DAP_TERM_MS))
  return;
 kill(s->pid, SIGKILL);
 while (waitpid(s->pid, &st, 0) < 0 && errno == EINTR)
  ;
 s->pid = 0;
}

void e_dap_close(e_dap_session *s)
{
 if (!s)
  return;
 dap_quit_and_wait(s);
 dap_reap_adapter(s);
 dap_close_fds(s);
 dap_pending_free_all(s);
 dap_bp_ids_free(s);
 e_dap_reader_free(&s->rd);
 free(s->start_error);
 free(s->program);
 free(s->entry_func);
 free(s);
}
