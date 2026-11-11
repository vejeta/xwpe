/* we_dap.c -- DAP debug session engine.  See we_dap.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "we_child.h"
#include "we_dap.h"
#include "we_dap_proto.h"
#include "we_fdloop.h"

#define DAP_MAX_BP   64
#define DAP_READ_CHUNK 4096
#define DAP_NO_FD    (-1)
#define DAP_NO_SEQ   (-1)
#define DAP_REASON_MAX   32
#define DAP_PENDING_MAX  64     /* requests whose answer has an owner      */
#define DAP_BP_IDS_MAX   256    /* adapter breakpoint ids remembered       */
#define DAP_NO_ID        (-1)
/* The reason a stop is reported with when the adapter refused to resume:
   the program is where it was, and the host marks it there again. */
#define DAP_REASON_REFUSED "refused"
#define DAP_REASON_PAUSE   "pause"

/* Who owns the answer to a request: the dispatcher hands each response to
   the owner its seq was registered with.  A response whose seq has no
   owner (gdb's late launch answer, a resume's plain success, a request
   given up on) is dropped. */
#define DAP_REQ_FREE        0   /* table slot unused                       */
#define DAP_REQ_BREAKPOINTS 2   /* setBreakpoints: told to on_breakpoints  */
#define DAP_REQ_LOCATE      3   /* stackTrace after a stop: on_stopped     */
#define DAP_REQ_RESUME      4   /* continue/next/...: a refusal is a stop  */
#define DAP_REQ_PAUSE       5   /* pause: a refusal forgets the pause      */
#define DAP_REQ_DISCONNECT  6   /* disconnect: the adapter is going        */
#define DAP_REQ_CALL        7   /* e_dap_request: its callback is told     */

/* A DAP command name, as a request's owner keeps it for the waiting line. */
#define DAP_CMD_MAX      48

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
 char *file;                /* DAP_REQ_BREAKPOINTS: the file and the lines */
 int  *lines;                 /*   asked for, to name them in the answer     */
 int   n;
 e_d_answered_fn fn;          /* DAP_REQ_CALL: told how the request ended    */
 void *ctx;
 char  cmd[DAP_CMD_MAX];      /*   its command, for the waiting line         */
 long  sent_ms;               /*   when it went out                          */
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
 WpeChild      adapter;       /* its process, the owner's (we_child.c) to end    */
 int           adapter_end;   /* WPE_CHILD_RUNS until its end is reported        */
 int           adapter_status;/* its wait status, once reported ENDED            */
 e_dap_adapter_end_fn end_fn; /* the host's: told when the adapter ended         */
 void         *end_data;
 int           seq;           /* request sequence counter           */
 e_dap_reader  rd;            /* incoming frame reassembly          */
 e_dap_host    host;
 char         *program;
 char         *entry_func;    /* e.g. "main.main", or NULL          */
 int           thread_id;     /* current stopped thread             */
 int           frame_id;      /* current top frame (for evaluate)   */
 int           ended;
 int           program_ended; /* `terminated` or `exited` said so    */
 int           exit_known;    /* the `exited` event named a code      */
 int           exit_code;     /* that code                           */
 int           program_pid;   /* the `process` event's systemProcessId:
                                 the program, on this machine; 0 when the
                                 adapter named none (Bloop's does not)  */
 char          bp_file[512];
 int           bp_lines[DAP_MAX_BP];
 int           bp_n;
 int           eof;           /* the transport reached its end              */
 int           input_lost;    /* a request could not be handed to the adapter:
                                 it stopped reading, or closed its input   */
 int           run;           /* DAP_RUN_*                                  */
 int           stop_pending;  /* a stop came while the start was not done   */
 int           stop_reported; /* on_stopped fired; the pump says STOPPED     */
 int           pause_pending; /* pause sent: the next stop is the pause      */
 int           disconnect_done;/* disconnect answered, nothing left to wait */
 char          stop_reason[DAP_REASON_MAX];
 dap_pending   pending[DAP_PENDING_MAX];
 dap_bp_id     bp_ids[DAP_BP_IDS_MAX];
 int           bp_ids_n;
 /* Requests whose answer goes to a callback (e_dap_request): the host's
    clock and timers, and which of them the waiting line and the deadline
    are counted for. */
 const E_D_REQ_IO *rio;       /* NULL: no waiting line and no deadline     */
 int           deadline_s;
 int           head_seq;      /* the oldest call out, or DAP_NO_SEQ        */
 int           waiting_shown; /* the waiting line names it               */
 /* The arguments a chain's step built for the request it is about to make
    (e_dap_stage_*), taken by e_dap_req_request. */
 char          staged_cmd[DAP_CMD_MAX];
 struct json_object *staged_args;
 char         *staged_file;
 int          *staged_lines;
 int           staged_n;
};

/* The fd to write requests to / read messages from, abstracting the transport. */
static int dap_wfd(e_dap_session *s) { return s->sock >= 0 ? s->sock : s->in_fd; }
static int dap_rfd(e_dap_session *s) { return s->sock >= 0 ? s->sock : s->out_fd; }

/* ---- low-level I/O ----------------------------------------------------- */

/* What the line that says the adapter stopped reading its input adds. */
#define DAP_INPUT_ENDS "Debug > Quit ends it"

/**
 * dap_input_name - The adapter as the status row names it when it stops
 * reading: its own name ("lldb-dap") once the host gave it, so the line
 * fits the row.
 */
static const char *dap_input_name(const e_dap_session *s)
{
 if (s->rio && s->rio->name && s->rio->name[0])
  return s->rio->name;
 return E_DAP_ADAPTER_WHAT;
}

/**
 * dap_socket_nonblocking - Make the adapter's socket return at once on a
 * read or a write: a request it does not take now is queued on the loop,
 * and the editor never waits inside a write to a wedged adapter.
 * @fd: the connected socket.
 */
static void dap_socket_nonblocking(int fd)
{
 int fl = fcntl(fd, F_GETFL, 0);

 if (fl >= 0)
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int dap_start_fail(e_dap_session *s);
static int dap_start_status(const e_dap_session *s);

/**
 * dap_write_all - Hand one whole request (header and body) to the adapter.
 * @s: the session; @buf, @n: the framed request.
 *
 * Written now as far as the adapter reads, the rest by the loop behind any
 * request still waiting, so the editor keeps its keys while an adapter is
 * wedged, and the status row names it once it has read nothing for a
 * while.  A request it can no longer be given (its queue is full, or it
 * closed its input) loses the adapter's input for good: a start in progress
 * fails, and every later request ends at once as gone, never waiting for
 * an answer that cannot come.
 * Return: 0 when the request is on its way, -1 when the input is lost.
 */
static int dap_write_all(e_dap_session *s, const char *buf, size_t n)
{
 int r;

 if (s->input_lost)
  return -1;
 r = wpe_fd_write(dap_wfd(s), buf, n, dap_input_name(s), DAP_INPUT_ENDS);
 if (r == WPE_FD_WRITE_SENT || r == WPE_FD_WRITE_QUEUED)
  return 0;
 s->input_lost = 1;
 if (dap_start_status(s) == E_DAP_START_WAITING)
  dap_start_fail(s);
 return -1;
}

/**
 * dap_send - Send a request; takes ownership of @args (may be NULL).
 * Return: its seq, or DAP_NO_SEQ when it could not be handed to the
 * adapter (dap_write_all).
 */
static int dap_send(e_dap_session *s, const char *command, struct json_object *args)
{
 int seq = ++s->seq;
 size_t len = 0;
 char *frame = e_dap_build_request(seq, command, args, &len);
 int r = -1;

 if (args)
  json_object_put(args);
 if (frame)
 {
  r = dap_write_all(s, frame, len);
  free(frame);
 }
 if (r != 0)
  return DAP_NO_SEQ;
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
 if (n < 0 && (errno == EINTR || errno == EAGAIN))
  return;
 close(s->pty_master);                /* slave gone (EIO/EOF) */
 s->pty_master = -1;
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
static void dap_transport_ended(e_dap_session *s);

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
 dap_transport_ended(s);
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
 if (fd < 0)
  return 0;
 return wpe_fd_readable_now(fd) != WPE_FD_NOT_READABLE;
}

/**
 * e_dap_discard_input - Read what the adapter sent, once, and drop it.
 * @s: the session, its answers no longer wanted (Quit waits for the
 * adapter's exit).
 *
 * Keeps an adapter that writes while it exits from blocking on a full
 * pipe, without keeping what it wrote: one that writes without end cannot
 * grow the editor's memory while Quit waits.  Reads only what is there.
 */
void e_dap_discard_input(e_dap_session *s)
{
 char buf[DAP_READ_CHUNK];
 ssize_t n;

 if (!s || !dap_transport_readable_now(s))
  return;
 n = read(dap_rfd(s), buf, sizeof(buf));
 if (n > 0 || (n < 0 && (errno == EINTR || errno == EAGAIN)))
  return;
 dap_transport_ended(s);
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

/* The `process` event's startMethod for a program the adapter started
   itself; `attach` and `attachForSuspendedLaunch` name one it joined. */
#define DAP_START_METHOD_LAUNCH "launch"

/**
 * dap_process_was_launched - Did the adapter start the program a `process`
 * event names, rather than attach to one already running?
 * @body: the event's body.
 *
 * Quit ends only a program the session started: a process the user had
 * running and attached the debugger to goes on after the session.  An
 * event without startMethod is taken as a launch, which is all the editor
 * asks adapters for.
 * Return: 1 for a launch, 0 otherwise.
 */
static int dap_process_was_launched(struct json_object *body)
{
 struct json_object *v = NULL;
 const char *how;

 if (!json_object_object_get_ex(body, "startMethod", &v))
  return 1;
 how = json_object_get_string(v);
 return how != NULL && strcmp(how, DAP_START_METHOD_LAUNCH) == 0;
}

/**
 * dap_note_program_pid - Keep the process number a `process` event names.
 * @s: the session; @body: the event's body.
 *
 * Quit asks the adapter to end the program; one that answers and leaves it
 * running would leave the user's program behind the session, so the editor
 * keeps the number to end it itself.  A remote program (isLocalProcess
 * false), a process the adapter attached to rather than started (the
 * user's own, which must outlive the session) and a number that is not a
 * process (lldb-dap may say 0) are not kept.
 */
static void dap_note_program_pid(e_dap_session *s, struct json_object *body)
{
 struct json_object *v = NULL;
 int pid;

 if (!body || !json_object_object_get_ex(body, "systemProcessId", &v))
  return;
 pid = json_object_get_int(v);
 if (json_object_object_get_ex(body, "isLocalProcess", &v) &&
     !json_object_get_boolean(v))
  return;
 if (!dap_process_was_launched(body))
  return;
 if (pid > 0)
  s->program_pid = pid;
}

/**
 * e_dap_program_pid - The program the adapter said it runs, while it runs.
 * @s: the session.
 *
 * Return: its process number, from the adapter's `process` event, or 0
 * when the adapter named none or said the program ended (the number may
 * be another process's by then).
 */
int e_dap_program_pid(const e_dap_session *s)
{
 if (!s || s->program_ended)
  return 0;
 return s->program_pid;
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
 * the answer is for.  Every read hands each message to dap_dispatch: a
 * response goes to the owner of its seq, an event to dap_handle_event.  So
 * a setBreakpoints answer that comes while a stackTrace or an evaluate at
 * a stop is out reaches on_breakpoints, an event that comes then is never
 * taken for the answer, and a stop that comes before the answer to the
 * continue that caused it (dlv's pause) is the stop.
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
/**
 * dap_pending_free_slot - A table slot no request owns, or NULL when every
 * one waits for an answer.
 * @s: the session.
 */
static dap_pending *dap_pending_free_slot(e_dap_session *s)
{
 int i;

 for (i = 0; i < DAP_PENDING_MAX; i++)
  if (s->pending[i].kind == DAP_REQ_FREE)
   return &s->pending[i];
 return NULL;
}

static int dap_send_owned(e_dap_session *s, const char *command,
                          struct json_object *args, int kind)
{
 dap_pending *p = dap_pending_free_slot(s);

 if (!p || s->eof)
 {
  if (args)
   json_object_put(args);
  return DAP_NO_SEQ;
 }
 p->kind = kind;
 p->seq = dap_send(s, command, args);
 if (p->seq != DAP_NO_SEQ)
  return p->seq;
 dap_pending_release(p);           /* clears the slot, its seq with it */
 return DAP_NO_SEQ;
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

static void dap_breakpoints_answered(e_dap_session *s, const char *file,
                                     const int *lines, int n,
                                     struct json_object *m);

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
 * its output.  While requests still wait for the adapter to read them (the
 * `disconnect` among them) the side stays open too, so they are not cut
 * off; the session's close takes it then.
 */
static void dap_close_write_side(e_dap_session *s)
{
 if (wpe_fd_write_pending(dap_wfd(s)) > 0)
  return;
 if (s->sock >= 0)
 {
  shutdown(s->sock, SHUT_WR);
  return;
 }
 if (s->in_fd >= 0)
  wpe_fd_close(s->in_fd);
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
 * Its owner is called now and its slot freed.  A response nobody owns (a
 * request given up on) is dropped.
 */
static void dap_call_answered(e_dap_session *s, dap_pending *p,
                              struct json_object *m);

static void dap_dispatch_response(e_dap_session *s, struct json_object *m)
{
 dap_pending *p = dap_pending_find(s, obj_int(m, "request_seq", DAP_NO_SEQ));
 int kind;

 if (!p)
  return;
 kind = p->kind;
 if (kind == DAP_REQ_CALL)
 {
  dap_call_answered(s, p, m);
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
 else if (!strcmp(ev, "process"))
  dap_note_program_pid(s, body);
 else if (!strcmp(ev, "terminated") || !strcmp(ev, "exited"))
 {
  if (!strcmp(ev, "exited"))
   dap_note_exit_code(s, body);
  s->ended = 1;
  s->program_ended = 1;
  if (s->host.on_terminated)
   s->host.on_terminated(s->host.ud);
 }
}

/* ---- reading: one shared path for the pump and the requests ------------ */

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
 * e_dap_read_now - Read what the adapter has sent, if anything, and hand
 * every message to its owner, without taking the run's answer.
 * @s: the session.
 *
 * What the host calls when the transport is readable while the program is
 * stopped: the answers of requests sent at the stop (a watch's value, the
 * stack, a breakpoint's verdict) reach their callbacks, and a stop read
 * here is still the run pump's to answer.  It never blocks.
 * Return: 1 while the transport is open, 0 once it reached its end.
 */
int e_dap_read_now(e_dap_session *s)
{
 if (!s)
  return 0;
 if (dap_transport_readable_now(s))
  dap_read_once(s);
 dap_drain_queue(s);
 return !s->eof;
}

/* ---- requests answered by callback: e_dap_request -----------------------
 *
 * The editor asks the adapter at a stop (a watch, the stack) without
 * waiting: the request is registered with its seq and a callback, its
 * answer is handed to the callback by the dispatcher whenever it is read,
 * and events read meanwhile go where they always go.  The host's clock and
 * one-shot timers (an E_D_REQ_IO, the same world the line debuggers'
 * engine is given) count the waiting line and the deadline of the oldest
 * request out; a request given up on frees its slot, so an answer that
 * comes after it is dropped.
 */

/**
 * dap_now - The host's clock, or 0 when the host gave none.
 * @s: the session.
 */
static long dap_now(const e_dap_session *s)
{
 if (!s->rio || !s->rio->now_ms)
  return 0;
 return s->rio->now_ms(s->rio->io);
}

/**
 * dap_arm - Ask the host for timer @which in @ms, or for none.
 * @s: the session; @which: E_D_TIMER_*; @ms: the delay, or E_D_TIMER_OFF.
 */
static void dap_arm(const e_dap_session *s, int which, long ms)
{
 if (!s->rio || !s->rio->arm_timer)
  return;
 s->rio->arm_timer(which, ms, s->rio->io);
}

/**
 * dap_disarm_all - Take back every timer the requests asked for.
 * @s: the session.
 */
static void dap_disarm_all(e_dap_session *s)
{
 int which;

 for (which = 0; which < E_D_TIMERS; which++)
  dap_arm(s, which, E_D_TIMER_OFF);
}

/**
 * dap_oldest_call - The request answered by callback that went out first,
 * or NULL when none is out.
 * @s: the session.
 */
static dap_pending *dap_oldest_call(e_dap_session *s)
{
 dap_pending *oldest = NULL;
 int i;

 for (i = 0; i < DAP_PENDING_MAX; i++)
 {
  dap_pending *p = &s->pending[i];

  if (p->kind != DAP_REQ_CALL)
   continue;
  if (!oldest || p->seq < oldest->seq)
   oldest = p;
 }
 return oldest;
}

/**
 * dap_ms_after - What is left of @ms counted from @since, never below 0.
 * @s: the session; @since: a dap_now() instant; @ms: the span.
 */
static long dap_ms_after(const e_dap_session *s, long since, long ms)
{
 long left = since + ms - dap_now(s);

 if (left < 0)
  return 0;
 return left;
}

/**
 * dap_calls_time - Count the waiting line and the deadline for the oldest
 * request out, once it changed.
 * @s: the session.
 *
 * The user is told what the editor waits for after a quiet second, and the
 * request is given up at the deadline, counted from when it went out, as
 * for the line debuggers.  With no request out every timer is taken back.
 */
static void dap_calls_time(e_dap_session *s)
{
 dap_pending *p = dap_oldest_call(s);

 if (!p)
 {
  s->head_seq = DAP_NO_SEQ;
  s->waiting_shown = 0;
  dap_disarm_all(s);
  return;
 }
 if (p->seq == s->head_seq)
  return;
 s->head_seq = p->seq;
 s->waiting_shown = 0;
 dap_arm(s, E_D_TIMER_SECOND, E_D_TIMER_OFF);
 dap_arm(s, E_D_TIMER_QUIET, dap_ms_after(s, p->sent_ms, E_D_REQ_QUIET_MS));
 dap_arm(s, E_D_TIMER_DEADLINE,
         dap_ms_after(s, p->sent_ms,
                      (long)s->deadline_s * E_D_REQ_SECOND_MS));
}

/**
 * dap_answer_of - The answer an owner is handed for response @m.
 * @ans: receives it; its text is the response as the adapter sent it (the
 * caller frees it), and its error flag is set for a refusal.
 */
static void dap_answer_of(E_D_ANSWER *ans, struct json_object *m)
{
 const char *text = json_object_to_json_string_ext(m, JSON_C_TO_STRING_PLAIN);

 memset(ans, 0, sizeof(*ans));
 ans->text = text ? strdup(text) : NULL;
 ans->len = ans->text ? strlen(ans->text) : 0;
 ans->error = dap_response_refused(m);
}

/**
 * dap_call_answered - A request answered by callback has its answer.
 * @s: the session; @p: its slot; @m: the response.
 *
 * A setBreakpoints made this way is told to on_breakpoints first, as every
 * breakpoint answer is.  The slot is free before the owner is called, so
 * the owner may send its next request at once.
 */
static void dap_call_answered(e_dap_session *s, dap_pending *p,
                              struct json_object *m)
{
 e_d_answered_fn fn = p->fn;
 void *ctx = p->ctx;
 E_D_ANSWER ans;

 if (p->file)
  dap_breakpoints_answered(s, p->file, p->lines, p->n, m);
 dap_answer_of(&ans, m);
 dap_pending_release(p);
 dap_calls_time(s);
 if (fn)
  fn(E_D_REQ_DONE, &ans, ctx);
 free(ans.text);
}

/**
 * dap_call_given_up - Forget a request answered by callback that will
 * never be answered to its owner.
 * @p: its slot.
 *
 * A setBreakpoints keeps its slot as an ordinary breakpoint request, so its
 * late answer still turns the user's marks solid or hollow; any other
 * answer that comes later is dropped.
 */
static void dap_call_given_up(dap_pending *p)
{
 if (p->file)
 {
  p->kind = DAP_REQ_BREAKPOINTS;
  p->fn = NULL;
  p->ctx = NULL;
  return;
 }
 dap_pending_release(p);
}

/* The owners a cancel tells, oldest first. */
typedef struct {
 int seq;
 e_d_answered_fn fn;
 void *ctx;
} dap_told;

/**
 * dap_told_sort - Order the owners a cancel tells by their requests' seq.
 * @t, @n: the owners.
 */
static void dap_told_sort(dap_told *t, int n)
{
 int i, j;

 for (i = 1; i < n; i++)
  for (j = i; j > 0 && t[j - 1].seq > t[j].seq; j--)
  {
   dap_told x = t[j];

   t[j] = t[j - 1];
   t[j - 1] = x;
  }
}

void e_dap_requests_cancel(e_dap_session *s, E_D_REQ_END why)
{
 dap_told told[DAP_PENDING_MAX];
 E_D_ANSWER empty;
 int i, n = 0;

 if (!s)
  return;
 for (i = 0; i < DAP_PENDING_MAX; i++)
 {
  dap_pending *p = &s->pending[i];

  if (p->kind != DAP_REQ_CALL)
   continue;
  told[n].seq = p->seq;
  told[n].fn = p->fn;
  told[n].ctx = p->ctx;
  n++;
  dap_call_given_up(p);
 }
 dap_calls_time(s);
 dap_told_sort(told, n);
 memset(&empty, 0, sizeof(empty));
 for (i = 0; i < n; i++)
  if (told[i].fn)
   told[i].fn(why, &empty, told[i].ctx);
}

/**
 * dap_transport_ended - The adapter's transport reached its end.
 * @s: the session.
 *
 * Nothing more will come: every request out is told the adapter is gone,
 * so no owner waits for an answer, and the session counts as ended.
 */
static void dap_transport_ended(e_dap_session *s)
{
 s->eof = 1;
 s->ended = 1;
 e_dap_requests_cancel(s, E_D_REQ_GONE);
}

void e_dap_requests_init(e_dap_session *s, const E_D_REQ_IO *io,
                         const char *timeout_text)
{
 if (!s)
  return;
 s->rio = io;
 s->deadline_s = e_d_req_timeout_from_text(timeout_text);
}

/**
 * dap_call_say_given_up - The sentence the deadline leaves on the status
 * row: which request was not answered, within how long, and how to leave.
 * @s: the session; @p: the request.
 */
static void dap_call_say_given_up(const e_dap_session *s, const dap_pending *p)
{
 char msg[E_D_REQ_MSG_MAX];
 const char *name = s->rio->name ? s->rio->name : "the debug adapter";

 if (!s->rio->notice)
  return;
 snprintf(msg, sizeof msg, E_D_REQ_TIMEOUT_FMT, name, p->cmd, s->deadline_s);
 s->rio->notice(msg, s->rio->io);
}

/**
 * dap_call_say_waiting - Show the waiting line for the oldest request out.
 * @s: the session; @p: the request.
 */
static void dap_call_say_waiting(e_dap_session *s, const dap_pending *p)
{
 long secs = (dap_now(s) - p->sent_ms) / E_D_REQ_SECOND_MS;

 s->waiting_shown = 1;
 if (s->rio->waiting)
  s->rio->waiting(p->cmd, secs, s->rio->io);
 dap_arm(s, E_D_TIMER_SECOND, E_D_REQ_SECOND_MS);
}

void e_dap_request_timer(e_dap_session *s, int which)
{
 dap_pending *p;

 if (!s || !s->rio)
  return;
 p = dap_pending_find(s, s->head_seq);
 if (!p || p->kind != DAP_REQ_CALL)
  return;                       /* a timer of a request that has ended */
 if (which == E_D_TIMER_QUIET)
  dap_call_say_waiting(s, p);
 else if (which == E_D_TIMER_SECOND && s->waiting_shown)
  dap_call_say_waiting(s, p);
 else if (which == E_D_TIMER_DEADLINE)
 {
  dap_call_say_given_up(s, p);
  e_dap_requests_cancel(s, E_D_REQ_TIMED_OUT);
 }
}

int e_dap_requests_pending(const e_dap_session *s)
{
 int i;

 if (!s)
  return 0;
 for (i = 0; i < DAP_PENDING_MAX; i++)
  if (s->pending[i].kind == DAP_REQ_CALL)
   return 1;
 return 0;
}

int e_dap_answers_owed(const e_dap_session *s)
{
 int i;

 if (!s)
  return 0;
 for (i = 0; i < DAP_PENDING_MAX; i++)
  if (s->pending[i].kind == DAP_REQ_CALL ||
      s->pending[i].kind == DAP_REQ_BREAKPOINTS)
   return 1;
 return 0;
}

const char *e_dap_request_waited(const e_dap_session *s, long *secs)
{
 dap_pending *p;

 if (!s)
  return NULL;
 p = dap_oldest_call((e_dap_session *)s);
 if (!p)
  return NULL;
 if (secs)
  *secs = (dap_now(s) - p->sent_ms) / E_D_REQ_SECOND_MS;
 return p->cmd;
}

/**
 * dap_call_send - Send a request whose answer goes to @fn, in slot @p.
 * @s: the session; @p: a free slot; @command, @args: the request (@args is
 * taken over); @file, @lines, @n: for a setBreakpoints, what it asks for
 * (taken over), else NULL, NULL, 0.
 * Return: E_D_SEND_OK, or E_D_SEND_GONE when the adapter could not be
 * given the request: the slot is freed and @fn is never called, so the
 * caller ends the request at once instead of waiting for its deadline.
 */
static int dap_call_send(e_dap_session *s, dap_pending *p, const char *command,
                         struct json_object *args, char *file, int *lines,
                         int n, e_d_answered_fn fn, void *ctx)
{
 p->kind = DAP_REQ_CALL;
 p->fn = fn;
 p->ctx = ctx;
 p->file = file;
 p->lines = lines;
 p->n = n;
 snprintf(p->cmd, sizeof p->cmd, "%s", command);
 p->sent_ms = dap_now(s);
 p->seq = dap_send(s, command, args);
 if (p->seq == DAP_NO_SEQ)
 {
  dap_pending_release(p);
  return E_D_SEND_GONE;
 }
 dap_calls_time(s);
 return E_D_SEND_OK;
}

/**
 * dap_call_refused - Why a request cannot go out now, or E_D_SEND_OK.
 * @s: the session; @command: the request.
 */
static int dap_call_refused(const e_dap_session *s, const char *command)
{
 if (!s || s->eof || s->ended || dap_wfd((e_dap_session *)s) < 0)
  return E_D_SEND_GONE;
 if (s->input_lost)
  return E_D_SEND_GONE;
 if (s->run == DAP_RUN_DISCONNECTING)
  return E_D_SEND_GONE;
 if (!command || strlen(command) >= DAP_CMD_MAX)
  return E_D_SEND_TOO_LONG;
 return E_D_SEND_OK;
}

int e_dap_request(e_dap_session *s, const char *command,
                  struct json_object *args, e_d_answered_fn fn, void *ctx)
{
 int why = dap_call_refused(s, command);
 dap_pending *p;

 if (why == E_D_SEND_OK)
 {
  p = dap_pending_free_slot(s);
  if (p)
   return dap_call_send(s, p, command, args, NULL, NULL, 0, fn, ctx);
  why = E_D_SEND_FULL;
 }
 if (args)
  json_object_put(args);
 return why;
}

/* ---- the requester: chains of steps over the seq table ----------------- */

/**
 * dap_stage_clear - Forget what a step built and did not send.
 * @s: the session.
 */
static void dap_stage_clear(e_dap_session *s)
{
 if (s->staged_args)
  json_object_put(s->staged_args);
 free(s->staged_file);
 free(s->staged_lines);
 s->staged_args = NULL;
 s->staged_file = NULL;
 s->staged_lines = NULL;
 s->staged_n = 0;
 s->staged_cmd[0] = '\0';
}

/**
 * dap_stage - Keep @args for the request named @command a step is about
 * to make, and write its name as the step's command.
 * @s: the session; @command: the DAP command; @args: its arguments (taken
 * over); @cmd, @n: the step's command buffer.
 *
 * The arguments live in the session, not in the step's buffer, so a long
 * watch expression or a file with many breakpoints goes out whole.
 * Return: 1 (the step sends), or 0 when nothing can be sent.
 */
static int dap_stage(e_dap_session *s, const char *command,
                     struct json_object *args, char *cmd, size_t n)
{
 dap_stage_clear(s);
 if (!args)
  return 0;
 snprintf(s->staged_cmd, sizeof s->staged_cmd, "%s", command);
 s->staged_args = args;
 snprintf(cmd, n, "%s", command);
 return 1;
}

int e_dap_stage_evaluate(e_dap_session *s, const char *expr, char *cmd,
                         size_t n)
{
 struct json_object *args;

 if (!s || !expr)
  return 0;
 args = json_object_new_object();
 json_object_object_add(args, "expression", json_object_new_string(expr));
 json_object_object_add(args, "frameId", json_object_new_int(s->frame_id));
 json_object_object_add(args, "context", json_object_new_string("watch"));
 return dap_stage(s, "evaluate", args, cmd, n);
}

int e_dap_stage_stack_trace(e_dap_session *s, int levels, char *cmd, size_t n)
{
 struct json_object *args;

 if (!s || levels <= 0)
  return 0;
 args = json_object_new_object();
 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 json_object_object_add(args, "startFrame", json_object_new_int(0));
 json_object_object_add(args, "levels", json_object_new_int(levels));
 return dap_stage(s, "stackTrace", args, cmd, n);
}

static struct json_object *dap_breakpoints_args(const char *path,
                                                const int *lines, int n);
static void dap_absolute_path(const char *file, char *out);

/**
 * dap_stage_keep_lines - Keep the file and lines of a staged setBreakpoints,
 * to name them when its answer comes.
 * @s: the session; @path, @lines, @n: what is asked for.
 * Return: 0, or -1 without memory for them.
 */
static int dap_stage_keep_lines(e_dap_session *s, const char *path,
                                const int *lines, int n)
{
 s->staged_file = strdup(path);
 s->staged_lines = malloc(sizeof(int) * (size_t)(n + 1));
 if (!s->staged_file || !s->staged_lines)
  return -1;
 if (n > 0)
  memcpy(s->staged_lines, lines, sizeof(int) * (size_t)n);
 s->staged_n = n;
 return 0;
}

int e_dap_stage_breakpoints(e_dap_session *s, const char *file,
                            const int *lines, int count, char *cmd, size_t n)
{
 char abspath[PATH_MAX];

 if (!s || !file || count < 0 || count > DAP_MAX_BP)
  return 0;
 dap_absolute_path(file, abspath);
 if (!dap_stage(s, "setBreakpoints", dap_breakpoints_args(abspath, lines, count),
                cmd, n))
  return 0;
 if (dap_stage_keep_lines(s, abspath, lines, count) == 0)
  return 1;
 dap_stage_clear(s);
 return 0;
}

int e_dap_req_request(void *engine, const char *cmd, e_d_answered_fn answered,
                      void *ctx)
{
 e_dap_session *s = engine;
 struct json_object *args = NULL;
 char *file = NULL;
 int *lines = NULL, n = 0, why;
 dap_pending *p;

 if (!s)
  return E_D_SEND_GONE;
 if (s->staged_cmd[0] && cmd && !strcmp(s->staged_cmd, cmd))
 {
  args = s->staged_args;
  file = s->staged_file;
  lines = s->staged_lines;
  n = s->staged_n;
  s->staged_args = NULL;
  s->staged_file = NULL;
  s->staged_lines = NULL;
 }
 dap_stage_clear(s);
 why = dap_call_refused(s, cmd);
 p = why == E_D_SEND_OK ? dap_pending_free_slot(s) : NULL;
 if (p)
  return dap_call_send(s, p, cmd, args, file, lines, n, answered, ctx);
 if (why == E_D_SEND_OK)
  why = E_D_SEND_FULL;
 if (args)
  json_object_put(args);
 free(file);
 free(lines);
 return why;
}

/* ---- reading the answers ------------------------------------------------ */

/**
 * dap_answer_json - The response an answer holds, parsed, or NULL.
 * @ans: an answer handed to a callback (its text is the response).
 */
static struct json_object *dap_answer_json(const E_D_ANSWER *ans)
{
 if (!ans || !ans->text)
  return NULL;
 return json_tokener_parse(ans->text);
}

char *e_dap_answer_result(const E_D_ANSWER *ans)
{
 struct json_object *m = dap_answer_json(ans);
 const char *r = obj_str(obj_obj(m, "body"), "result");
 char *out = NULL;

 if (r)
  out = strdup(r);
 if (m)
  json_object_put(m);
 return out;
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

int e_dap_answer_frames(const E_D_ANSWER *ans, e_dap_frame *frames, int max)
{
 struct json_object *m = dap_answer_json(ans);
 struct json_object *list = obj_obj(obj_obj(m, "body"), "stackFrames");
 int i, n = 0;

 if (!m)
  return -1;
 if (list && json_object_is_type(list, json_type_array))
 {
  n = (int)json_object_array_length(list);
  if (n > max)
   n = max;
  for (i = 0; i < n; i++)
   dap_frame_from_json(json_object_array_get_idx(list, i), &frames[i]);
 }
 json_object_put(m);
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
 fcntl(fd, F_SETFD, FD_CLOEXEC);  /* the adapter dials in; it never listens */
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

/**
 * dap_adapter_ended - The owner reported the adapter's end, from the loop.
 * @c: the adapter; @status: its wait status; @how: WPE_CHILD_ENDED, or
 * WPE_CHILD_ENDED_UNKNOWN when it did not end even after SIGKILL;
 * @data: the session.
 *
 * Kept for the line that tells the user how it ended, then told to the
 * host, which may free the session from there.
 */
static void dap_adapter_ended(WpeChild *c, int status, int how, void *data)
{
 e_dap_session *s = data;

 (void)c;
 s->adapter_end = how;
 s->adapter_status = status;
 if (s->end_fn)
  s->end_fn(s, s->end_data);
}

/**
 * dap_adapter_spec - What every adapter the editor starts has in common.
 * @spec: filled in; @argv, @cwd: the command and its directory.
 *
 * A session of its own, so a Ctrl-C typed in the editor never ends an
 * adapter that does not catch it, and the system ends it if the editor is
 * killed outright.
 */
static void dap_adapter_spec(WpeChildSpec *spec, char *const argv[],
                             const char *cwd)
{
 memset(spec, 0, sizeof *spec);
 spec->argv = argv;
 spec->cwd = cwd;
 spec->group = WPE_CHILD_OWN_SESSION;
 spec->die_with_editor = 1;
 spec->what = E_DAP_ADAPTER_WHAT;
}

/**
 * dap_spawn - Start a reverse-TCP adapter (dlv) on a pty of its own, with
 * --client-addr=127.0.0.1:@port appended.
 * @s: the session; @argv, @cwd: the command and its directory; @port: the
 * port the editor listens on.
 *
 * DAP traffic flows over the socket the adapter dials, so its stdio is free
 * to be a pty, and it must be: dlv runs its program in the foreground and
 * calls tcsetpgrp() on its own stdin, which fails on a pipe.  The program
 * inherits the pty, so what it prints reaches the master the editor reads
 * (the Messages window), raw, with no carriage return added to each line.
 * Return: 0, or -1 when it could not be started.
 */
static int dap_spawn(e_dap_session *s, char *const argv[], int port,
                     const char *cwd)
{
 WpeChildSpec spec;
 int argc = 0, i, rc;
 char addr[64];
 char **av;

 while (argv[argc])
  argc++;
 av = calloc(argc + 2, sizeof(char *));
 if (!av)
  return -1;
 for (i = 0; i < argc; i++)
  av[i] = argv[i];
 snprintf(addr, sizeof(addr), "--client-addr=127.0.0.1:%d", port);
 av[argc] = addr;
 av[argc + 1] = NULL;
 dap_adapter_spec(&spec, av, cwd);
 spec.io = WPE_CHILD_IO_PTY;
 spec.pty_raw = 1;
 rc = wpe_child_start(&spec, &s->adapter, dap_adapter_ended, s);
 free(av);
 if (rc != 0)
  return -1;
 s->pty_master = s->adapter.pty_fd;
 return 0;
}

/**
 * dap_spawn_stdio - Start a stdio adapter (gdb or lldb-dap speaking DAP):
 * DAP flows over its own stdin and stdout.
 * @s: the session; @argv, @cwd: the command and its directory.
 *
 * Its stderr is the editor's own (adapter diagnostics, rarely used).
 * Return: 0, or -1 when it could not be started.
 */
static int dap_spawn_stdio(e_dap_session *s, char *const argv[],
                           const char *cwd)
{
 WpeChildSpec spec;

 dap_adapter_spec(&spec, argv, cwd);
 spec.io = WPE_CHILD_IO_PIPES;
 spec.stderr_inherit = 1;
 if (wpe_child_start(&spec, &s->adapter, dap_adapter_ended, s) != 0)
  return -1;
 s->in_fd = s->adapter.in_fd;
 s->out_fd = s->adapter.out_fd;
 return 0;
}

/* ---- the start: the handshake, one answer at a time ------------------- *
 *
 * Each step sends one request and waits for its answer; the answer sends the
 * next request.  The host's event loop feeds them one readable descriptor
 * at a time (e_dap_start*), so an editor never waits on an adapter that
 * does not answer; we_dap_sync.c drives the same steps with poll() for a
 * caller that has no loop (the C tests).
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
 dap_socket_nonblocking(s->sock);
 dap_send_initialize(s);
 return dap_start_status(s);
}

int e_dap_start_state(e_dap_session *s)
{
 if (!s)
  return E_DAP_START_FAILED;
 return dap_start_consume(s);
}

int e_dap_start_give_up(e_dap_session *s)
{
 if (!s)
  return E_DAP_START_FAILED;
 return dap_start_fail(s);
}

int e_dap_start_dialling(const e_dap_session *s)
{
 return s && s->step == DAP_STEP_ACCEPT;
}

void e_dap_start_hold_after_initialize(e_dap_session *s)
{
 if (s)
  s->goal = DAP_STEP_OPENED;
}

int e_dap_start_go_on(e_dap_session *s)
{
 if (!s)
  return E_DAP_START_FAILED;
 s->goal = DAP_STEP_READY;
 if (s->step == DAP_STEP_OPENED)
  dap_send_launch(s);
 return dap_start_consume(s);
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
 return (pid_t)s->adapter.pid;
}

/**
 * e_dap_signal - Send @sig to the adapter and the group it leads.
 * @s: the session; @sig: the signal.
 *
 * Nothing is sent once the adapter has been reaped, so a PID the system gave
 * to another process is never signalled.
 */
void e_dap_signal(e_dap_session *s, int sig)
{
 if (s && s->spawned)
  wpe_child_signal(&s->adapter, sig);
}

/**
 * e_dap_end_adapter - End the adapter without waiting for it.
 * @s: the session; @polite_sig: SIGTERM, or WPE_CHILD_NO_SIGNAL when it was
 * already asked to go (`disconnect` went out).
 *
 * The owner of the editor's children gives it WPE_CHILD_END_GRACE_MS, then
 * SIGKILL, on the loop; the host learns of the end from the function given
 * to e_dap_on_adapter_end.
 */
void e_dap_end_adapter(e_dap_session *s, int polite_sig)
{
 if (s && s->spawned)
  wpe_child_end(&s->adapter, polite_sig);
}

/**
 * e_dap_on_adapter_end - Who is told when the adapter has ended.
 * @s: the session; @fn: called once, from the loop, or NULL; @data: for it.
 *
 * Unlike the host's callbacks, it is not cleared by e_dap_host_forget: Quit
 * learns of the end of an adapter it is ending this way.
 */
void e_dap_on_adapter_end(e_dap_session *s, e_dap_adapter_end_fn fn, void *data)
{
 if (!s)
  return;
 s->end_fn = fn;
 s->end_data = data;
}

/**
 * e_dap_adapter_gone - Has the adapter exited?  Never waits.
 * @s: the session.
 *
 * Read from what the editor's SIGCHLD handler kept, so it is right the
 * moment the adapter is reaped, before its end is reported.  An adapter that
 * is not this process's child is never "gone" here.
 * Return: 1 once the adapter is gone.
 */
int e_dap_adapter_gone(e_dap_session *s)
{
 int st = 0;

 if (!s || !s->spawned)
  return 0;
 return(e_dap_adapter_end_status(s, &st) || s->adapter.pid <= 0 ||
        wpe_child_ended(&s->adapter, &st) != WPE_CHILD_RUNS);
}

/**
 * e_dap_adapter_end_status - How the adapter ended, when that is known.
 * @s: the session; @status: receives its wait status.
 *
 * Known once its end was reported, and already when the SIGCHLD handler has
 * reaped it but the report is still to come: a Quit that comes in between
 * still tells the user that the adapter died, and how.
 * Return: 1 with *status set, else 0.
 */
int e_dap_adapter_end_status(const e_dap_session *s, int *status)
{
 if (!s || !s->spawned)
  return 0;
 if (s->adapter_end == WPE_CHILD_ENDED)
 {
  *status = s->adapter_status;
  return 1;
 }
 if (s->adapter_end != WPE_CHILD_RUNS || s->adapter.pid <= 0)
  return 0;
 return wpe_child_ended(&s->adapter, status) == WPE_CHILD_ENDED;
}

/**
 * e_dap_adapter_reaped - A caller with no SIGCHLD handler of its own (the
 * protocol tests' waits, we_dap_sync.c) reaped the adapter itself: the owner
 * stops following it.
 * @s: the session.
 */
void e_dap_adapter_reaped(e_dap_session *s)
{
 if (s && s->spawned)
  wpe_child_forget(&s->adapter);
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
  e_dap_free(s);
  return NULL;
 }
 /* The adapter's controlling pty is the owner's to make: we keep its master
    to read the debuggee's output from (forwarded to the host's Messages
    window). */
 if (dap_spawn(s, argv, port, cwd) != 0)
 {
  e_dap_free(s);
  return NULL;
 }
 s->spawned = 1;
 return s;
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
 if (dap_spawn_stdio(s, argv, cwd) != 0)
 {
  e_dap_free(s);
  return NULL;
 }
 s->spawned = 1;
 dap_send_initialize(s);
 return s;
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
 {  e_dap_free(s);  return NULL;  }
 memset(&a, 0, sizeof(a));
 a.sin_family = AF_INET;
 a.sin_port = htons((unsigned short)port);
 if (inet_pton(AF_INET, host, &a.sin_addr) != 1)
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
 {  close(fd);  e_dap_free(s);  return NULL;  }
 s->sock = fd;
 dap_socket_nonblocking(fd);
 dap_send_initialize(s);
 return s;
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
 * e_dap_set_file_breakpoints_async - Replace one file's breakpoints without
 * waiting for the adapter.
 * @s: the session; @file: the source file; @lines, @n: all its breakpoints.
 *
 * A breakpoint the user toggles reaches the adapter at once, running or
 * stopped (gdb-dap, lldb-dap and dlv all take it while running), and the
 * answer, verified or pending, comes later through on_breakpoints.  DAP has
 * no run-to-cursor request, so Go to cursor adds the cursor's line to the
 * file's breakpoints this way, continues, and sends the list again without
 * it at the next stop; setBreakpoints replaces every breakpoint of the
 * file, which is why the whole list is sent each time.  The adapter takes
 * requests in order, so a resume sent after it runs with the new list.
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

void e_dap_forward_output(e_dap_session *s)
{
 if (s)
  s->forward_output = 1;
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
/**
 * dap_start_goes_on - Is the start still to be taken on by the pump?
 * @s: the session.
 *
 * Not once Quit has sent `disconnect`: an adapter that is going is asked
 * nothing more, and the answers the start waited for are only read, so the
 * answer to `disconnect` ends Quit's wait even before the start was done.
 */
static int dap_start_goes_on(const e_dap_session *s)
{
 if (s->run == DAP_RUN_DISCONNECTING)
  return 0;
 return dap_start_status(s) == E_DAP_START_WAITING;
}

int e_dap_run_pump(e_dap_session *s)
{
 if (!s)
  return E_DAP_RUN_ENDED;
 if (dap_start_goes_on(s))
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

int e_dap_ended(const e_dap_session *s)
{
 return s ? s->ended : 1;
}

/**
 * e_dap_program_said_ended - Did the adapter say the program ended?
 * @s: the session.
 *
 * A `terminated` or `exited` event, as opposed to a transport that just
 * ended: the editor tells the user that an adapter that went away on its
 * own ended unexpectedly, and how.
 * Return: 1 or 0.
 */
int e_dap_program_said_ended(const e_dap_session *s)
{
 return s ? s->program_ended : 0;
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
 if (s->eof || s->step < DAP_STEP_OPENED || dap_wfd(s) < 0 || s->input_lost)
  return -1;
 s->run = DAP_RUN_DISCONNECTING;
 s->pause_pending = 0;
 if (dap_send_owned(s, "disconnect", dap_disconnect_args(),
                    DAP_REQ_DISCONNECT) != DAP_NO_SEQ)
  return 0;
 if (s->input_lost)
  return -1;
 /* No slot for its answer: sent all the same, and the adapter's exit (the
    end of its output) ends the wait instead. */
 if (dap_send(s, "disconnect", dap_disconnect_args()) == DAP_NO_SEQ)
  return -1;
 dap_close_write_side(s);
 return 0;
}

void e_dap_host_forget(e_dap_session *s)
{
 if (!s)
  return;
 memset(&s->host, 0, sizeof(s->host));
 s->rio = NULL;
}

int e_dap_disconnect_sent(const e_dap_session *s)
{
 return s && s->run == DAP_RUN_DISCONNECTING;
}

int e_dap_disconnect_answered(const e_dap_session *s)
{
 return s && s->disconnect_done;
}

void e_dap_hang_up(e_dap_session *s)
{
 if (s)
  dap_close_write_side(s);
}

/**
 * dap_close_fds - Close every descriptor the session holds; requests still
 * queued for the adapter go with its socket or its input.
 * @s: the session.
 */
static void dap_close_fds(e_dap_session *s)
{
 if (s->listen_fd >= 0)
  close(s->listen_fd);
 if (s->sock >= 0)
  wpe_fd_close(s->sock);
 if (s->in_fd >= 0)
  wpe_fd_close(s->in_fd);
 if (s->out_fd >= 0)
  close(s->out_fd);
 if (s->pty_master >= 0)
  close(s->pty_master);
 s->listen_fd = s->sock = s->in_fd = s->out_fd = s->pty_master = DAP_NO_FD;
}

void e_dap_free(e_dap_session *s)
{
 if (!s)
  return;
 e_dap_requests_cancel(s, E_D_REQ_GONE);
 dap_close_fds(s);
 /* The owner keeps a pointer to the adapter's record, which goes with the
    session: no end is reported to it after this.  The host ends the
    adapter first (Quit's ladder); the SIGCHLD handler reaps it anyway. */
 if (s->spawned)
  wpe_child_forget(&s->adapter);
 dap_pending_free_all(s);
 dap_bp_ids_free(s);
 dap_stage_clear(s);
 e_dap_reader_free(&s->rd);
 free(s->start_error);
 free(s->program);
 free(s->entry_func);
 free(s);
}
