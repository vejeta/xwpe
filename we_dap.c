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
 char          bp_file[512];
 int           bp_lines[DAP_MAX_BP];
 int           bp_n;
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

/* Block until one complete message arrives; NULL on EOF/error. */
static struct json_object *dap_read_message(e_dap_session *s)
{
 for (;;)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);
  struct pollfd p[2];
  char buf[4096];
  ssize_t n;
  int np = 0, sock_i, pty_i = -1, pret;

  if (m)
   return m;
  /* Watch the DAP transport fd AND, on the TCP transport, the debuggee's pty:
     program output arrives on the pty while the program runs (between continue
     and the next stop), so we must drain it here or it stalls when the pty
     buffer fills.  In stdio mode there is no pty (output comes via events). */
  sock_i = np; p[np].fd = dap_rfd(s); p[np].events = POLLIN; np++;
  if (s->pty_master >= 0)
  {  pty_i = np; p[np].fd = s->pty_master; p[np].events = POLLIN; np++;  }
  pret = poll(p, np, -1);
  if (pret < 0)
  {
   if (errno == EINTR)
    continue;
   return NULL;
  }
  if (pty_i >= 0 && (p[pty_i].revents & (POLLIN | POLLHUP | POLLERR)))
  {
   e_dap_drain_output(s);
   continue;
  }
  if (!(p[sock_i].revents & (POLLIN | POLLHUP | POLLERR)))
   continue;
  n = read(dap_rfd(s), buf, sizeof(buf));
  if (n < 0)
  {
   if (errno == EINTR)
    continue;
   return NULL;
  }
  if (n == 0)
  {
   s->ended = 1;
   return NULL;
  }
  e_dap_reader_push(&s->rd, buf, (size_t)n);
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

/* Handle an unsolicited event (output / stopped / terminated / exited). */
static void dap_handle_event(e_dap_session *s, struct json_object *m)
{
 const char *ev = obj_str(m, "event");
 struct json_object *body = obj_obj(m, "body");

 if (!ev)
  return;
 if (!strcmp(ev, "output"))
 {
  /* TCP transport (dlv): the debuggee's output reaches us through the pty (see
     dap_read_message), so the only "output" events are the adapter talking about
     itself -- drop them.  Stdio transport (gdb/lldb -dap): there is no pty, so
     the program's stdout/stderr DO arrive here -- forward them, but only once the
     program is running (forward_output), or the adapter's startup banner (also
     category "stdout" for gdb) would be echoed into Messages. */
  const char *cat = obj_str(body, "category");
  const char *txt = obj_str(body, "output");
  int is_program = cat && (!strcmp(cat, "stdout") || !strcmp(cat, "stderr"));
  if (s->forward_output && is_program && txt && s->host.on_output)
   s->host.on_output(txt, s->host.ud);
 }
 else if (!strcmp(ev, "stopped"))
 {
  s->thread_id = obj_int(body, "threadId", s->thread_id);
 }
 else if (!strcmp(ev, "terminated") || !strcmp(ev, "exited"))
 {
  s->ended = 1;
  if (s->host.on_terminated)
   s->host.on_terminated(s->host.ud);
 }
}

/* Pump messages, dispatching events, until the response to `seq` arrives.
 * Returns that response (caller owns) or NULL on EOF. */
static struct json_object *dap_wait_response(e_dap_session *s, int seq)
{
 for (;;)
 {
  struct json_object *m = dap_read_message(s);
  const char *type;
  if (!m)
   return NULL;
  type = obj_str(m, "type");
  if (type && !strcmp(type, "response") && obj_int(m, "request_seq", -1) == seq)
   return m;
  if (type && !strcmp(type, "event"))
   dap_handle_event(s, m);
  json_object_put(m);
 }
}

/* Pump until a `stopped` event or the program ends.  Returns the stop reason
 * (static/caller-must-copy via on_stopped) in *reason; 1 = stopped, 0 = ended. */
static int dap_wait_stop(e_dap_session *s, char *reason, size_t rsz)
{
 for (;;)
 {
  struct json_object *m = dap_read_message(s);
  const char *type;
  if (!m)
   return 0;
  type = obj_str(m, "type");
  if (type && !strcmp(type, "event"))
  {
   const char *ev = obj_str(m, "event");
   if (ev && !strcmp(ev, "stopped"))
   {
    const char *r = obj_str(obj_obj(m, "body"), "reason");
    s->thread_id = obj_int(obj_obj(m, "body"), "threadId", s->thread_id);
    if (reason && rsz)
     snprintf(reason, rsz, "%s", r ? r : "stopped");
    json_object_put(m);
    return 1;
   }
   dap_handle_event(s, m);
   if (s->ended)
   {
    json_object_put(m);
    return 0;
   }
  }
  json_object_put(m);
 }
}

/* After a stop: stackTrace the top frame and report the source line. */
static void dap_report_stop(e_dap_session *s, const char *reason)
{
 struct json_object *args = json_object_new_object();
 struct json_object *resp, *body, *frames, *fr0;
 int seq;

 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 json_object_object_add(args, "startFrame", json_object_new_int(0));
 json_object_object_add(args, "levels", json_object_new_int(1));
 seq = dap_send(s, "stackTrace", args);
 resp = dap_wait_response(s, seq);

 body = obj_obj(resp, "body");
 frames = body ? obj_obj(body, "stackFrames") : NULL;
 if (frames && json_object_is_type(frames, json_type_array) &&
     json_object_array_length(frames) > 0)
 {
  fr0 = json_object_array_get_idx(frames, 0);
  s->frame_id = obj_int(fr0, "id", 0);
  if (s->host.on_stopped)
   s->host.on_stopped(obj_str(obj_obj(fr0, "source"), "path"),
                      obj_int(fr0, "line", 0), reason, s->host.ud);
 }
 else if (s->host.on_stopped)
  /* e.g. Go's pre-main entry stop has no user frame. */
  s->host.on_stopped(NULL, 0, reason, s->host.ud);
 if (resp)
  json_object_put(resp);
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
 * dap_start_refused - The adapter refused a step of the start: end it.
 * @s: the session; @m: the refusing response.
 *
 * Keeps the adapter's own words (dlv's build error, in body.error.format,
 * or the short "message") for the user, who sees why the program did not
 * start instead of a session that waits for a stop that never comes.
 */
static void dap_start_refused(e_dap_session *s, struct json_object *m)
{
 const char *why = obj_str(obj_obj(obj_obj(m, "body"), "error"), "format");

 if (!why || !*why)
  why = obj_str(m, "message");
 if (!why || !*why)
  why = "the debug adapter refused the request";
 free(s->start_error);
 s->start_error = strdup(why);
 s->start_failed = 1;
}

/**
 * dap_start_feed - Take one message during the start.
 * @s: the session; @m: the message (not freed here).
 *
 * The awaited answer moves the start on, unless it is a refusal, which ends
 * the start; an event is handled as at any other time; any other response
 * (gdb's late launch answer) is dropped.
 */
static void dap_start_feed(e_dap_session *s, struct json_object *m)
{
 const char *type = obj_str(m, "type");
 int ended_before = s->ended;

 if (dap_start_is_awaited(s, m) && dap_response_refused(m))
 {
  dap_start_refused(s, m);
  return;
 }
 if (dap_start_is_awaited(s, m))
 {
  dap_start_advance(s);
  return;
 }
 if (type && !strcmp(type, "event"))
  dap_handle_event(s, m);
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
 * Return: 0 when readable, -1 when the adapter did not dial in or poll failed.
 */
static int dap_start_wait(e_dap_session *s)
{
 for (;;)
 {
  struct pollfd p[2];
  int np = 0, pty_i = -1, pret;
  int timeout = DAP_POLL_FOREVER;

  if (s->step == DAP_STEP_ACCEPT)
   timeout = DAP_ACCEPT_MS;
  p[np].fd = e_dap_start_fd(s); p[np].events = POLLIN; np++;
  if (s->pty_master >= 0)
  {  pty_i = np; p[np].fd = s->pty_master; p[np].events = POLLIN; np++;  }
  pret = poll(p, np, timeout);
  if (pret < 0 && errno == EINTR)
   continue;
  if (pret <= 0)
   return -1;
  if (pty_i >= 0 && (p[pty_i].revents & (POLLIN | POLLHUP | POLLERR)))
   e_dap_drain_output(s);
  if (p[0].revents & (POLLIN | POLLHUP | POLLERR))
   return 0;
 }
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
  return dap_start_fail(s);
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
 char reason[32];

 if (!s)
  return -1;
 if (s->ended)
 {
  s->forward_output = 1;
  return 0;
 }
 if (dap_wait_stop(s, reason, sizeof(reason)))
 {
  dap_report_stop(s, reason);
  /* Start forwarding the program's stdout/stderr only now, after the first
     stop: gdb emits its banner and its pending-breakpoint chatter ("No source
     file ... pending") as category "stdout" during launch/configuration, which
     would otherwise clutter Messages.  Real program output begins once the user
     continues from here. */
  s->forward_output = 1;
  return 0;
 }
 s->forward_output = 1;
 return s->ended ? 0 : -1;
}

int e_dap_step(e_dap_session *s, const char *verb)
{
 struct json_object *args, *resp;
 char reason[32];
 int seq;

 if (!s || s->ended)
  return -1;
 args = json_object_new_object();
 json_object_object_add(args, "threadId", json_object_new_int(s->thread_id));
 seq = dap_send(s, verb, args);
 resp = dap_wait_response(s, seq);
 if (resp)
  json_object_put(resp);

 if (dap_wait_stop(s, reason, sizeof(reason)))
 {
  dap_report_stop(s, reason);
  return 0;
 }
 return 0;       /* ended is normal for `continue` past the last breakpoint */
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
 seq = dap_send(s, "evaluate", args);
 resp = dap_wait_response(s, seq);
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
 * dap_send_disconnect - Ask the adapter to stop the program and exit.
 * @s: the session.
 *
 * Best effort.  An adapter that never answered initialize is not spoken to:
 * it is ended by signal, and a request it would never read is not left in
 * its input.
 */
static void dap_send_disconnect(e_dap_session *s)
{
 struct json_object *args;

 if (s->step < DAP_STEP_OPENED)
  return;
 args = json_object_new_object();
 json_object_object_add(args, "terminateDebuggee", json_object_new_boolean(1));
 dap_send(s, "disconnect", args);
}

void e_dap_close(e_dap_session *s)
{
 if (!s)
  return;
 if (s->listen_fd >= 0)
  close(s->listen_fd);
 if (s->sock >= 0)
 {
  dap_send_disconnect(s);
  close(s->sock);
 }
 if (s->in_fd >= 0 || s->out_fd >= 0)
 {
  /* stdio transport: ask the adapter to quit, then close the pipes */
  dap_send_disconnect(s);
  if (s->in_fd >= 0)
   close(s->in_fd);
  if (s->out_fd >= 0)
   close(s->out_fd);
 }
 if (s->pty_master >= 0)
  close(s->pty_master);
 if (s->pid > 0)
 {
  int st;
  /* We already sent a DAP "disconnect" (terminateDebuggee) and closed the
     transport, asking the adapter to stop the program and exit.  SIGTERM first
     for a graceful exit, but reap deterministically: gdb, while ptracing a
     stopped inferior, can ignore SIGTERM and waitpid() would then block forever
     -- so if it has not exited promptly, SIGKILL guarantees the reap (Linux
     PTRACE_O_EXITKILL takes the inferior down with it). */
  kill(s->pid, SIGTERM);
  if (waitpid(s->pid, &st, WNOHANG) == 0)
  {
   struct timespec ts = { 0, 200 * 1000 * 1000 };  /* 200ms grace */
   nanosleep(&ts, NULL);
   if (waitpid(s->pid, &st, WNOHANG) == 0)
   {
    kill(s->pid, SIGKILL);
    waitpid(s->pid, &st, 0);
   }
  }
 }
 e_dap_reader_free(&s->rd);
 free(s->start_error);
 free(s->program);
 free(s->entry_func);
 free(s);
}
