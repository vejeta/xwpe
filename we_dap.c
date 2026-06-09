/* we_dap.c -- DAP debug session engine.  See we_dap.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pty.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "we_dap.h"
#include "we_dap_proto.h"

#define DAP_MAX_BP   64
#define DAP_ACCEPT_MS 10000

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
   n = read(s->pty_master, buf, sizeof(buf) - 1);
   if (n > 0)
   {
    buf[n] = '\0';
    if (s->host.on_output)
     s->host.on_output(buf, s->host.ud);
   }
   else
   {  close(s->pty_master);  s->pty_master = -1;  }  /* slave gone (EIO/EOF) */
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

/* Pump messages until the named event arrives, dispatching the others (and
 * dropping any responses, e.g. a launch reply the adapter defers).  Used to
 * wait for the spec's `initialized` event before sending breakpoints on the
 * BSP/Bloop transport.  Returns 1 on the event, 0 on EOF. */
static int dap_wait_event(e_dap_session *s, const char *name)
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
   if (ev && !strcmp(ev, name))
   {
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

/* Wait up to DAP_ACCEPT_MS for the adapter to dial in; return the conn fd. */
static int dap_accept(int listen_fd)
{
 struct pollfd p;
 p.fd = listen_fd;
 p.events = POLLIN;
 if (poll(&p, 1, DAP_ACCEPT_MS) <= 0)
  return -1;
 return accept(listen_fd, NULL, NULL);
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

/* The DAP `initialize` handshake (shared by both transports). */
static int dap_initialize(e_dap_session *s, const char *adapter_id)
{
 struct json_object *args, *resp;
 int iseq;

 args = json_object_new_object();
 json_object_object_add(args, "clientID", json_object_new_string("xwpe"));
 json_object_object_add(args, "adapterID", json_object_new_string(adapter_id));
 json_object_object_add(args, "linesStartAt1", json_object_new_boolean(1));
 json_object_object_add(args, "columnsStartAt1", json_object_new_boolean(1));
 json_object_object_add(args, "pathFormat", json_object_new_string("path"));
 iseq = dap_send(s, "initialize", args);
 resp = dap_wait_response(s, iseq);
 if (!resp)
  return -1;
 json_object_put(resp);
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
 return s;
}

/* ---- public API -------------------------------------------------------- */

e_dap_session *e_dap_open(char *const argv[], const char *program,
                          const char *entry_func, const char *cwd,
                          const e_dap_host *host)
{
 e_dap_session *s;
 int listen_fd, port;

 s = dap_session_new(program, entry_func, host);
 if (!s)
  return NULL;
 s->launch_mode_debug = 1;        /* dlv launch arg "mode":"debug" (builds pkg) */

 listen_fd = dap_listen_ephemeral(&port);
 if (listen_fd < 0)
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
  s->pid = dap_spawn(argv, port, cwd, listen_fd, slave);
  if (slave >= 0)
   close(slave);                /* the child holds its own copy */
 }
 if (s->pid <= 0)
 {
  close(listen_fd);
  e_dap_close(s);
  return NULL;
 }
 s->sock = dap_accept(listen_fd);
 close(listen_fd);
 if (s->sock < 0 || dap_initialize(s, "dap") != 0)
 {
  e_dap_close(s);
  return NULL;
 }
 return s;
}

/* Stdio transport: spawn `argv` (e.g. {"gdb","--interpreter=dap"}), talk DAP
   over its pipes, run the initialize handshake.  `program` is a PREBUILT binary
   (the adapter launches it; it does not build), `entry_func` may be NULL. */
e_dap_session *e_dap_open_stdio(char *const argv[], const char *program,
                                const char *entry_func, const char *cwd,
                                const e_dap_host *host)
{
 e_dap_session *s = dap_session_new(program, entry_func, host);
 if (!s)
  return NULL;
 s->launch_mode_debug = 0;        /* program is a prebuilt binary, not a package */
 s->pid = dap_spawn_stdio(argv, cwd, &s->in_fd, &s->out_fd);
 if (s->pid <= 0 || dap_initialize(s, "gdb") != 0)
 {
  e_dap_close(s);
  return NULL;
 }
 return s;
}

/* Forward TCP client to an already-running DAP server (Bloop's scala-debug-
   adapter, address from BSP debugSession/start).  No spawn, no pty: the build
   server owns the debuggee, and its output reaches us as DAP "output" events. */
e_dap_session *e_dap_open_tcp(const char *host, int port, const char *program,
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
 if (dap_initialize(s, "scala") != 0)
 {  e_dap_close(s);  return NULL;  }
 return s;
}

int e_dap_add_breakpoint(e_dap_session *s, const char *file, int line)
{
 if (!s || s->bp_n >= DAP_MAX_BP)
  return -1;
 if (!s->bp_file[0])
  snprintf(s->bp_file, sizeof(s->bp_file), "%s", file);
 s->bp_lines[s->bp_n++] = line;
 return 0;
}

int e_dap_run(e_dap_session *s)
{
 struct json_object *args, *bps, *src, *resp;
 char reason[32];
 int seq;

 if (!s)
  return -1;

 /* launch (run; stop is driven by breakpoints, not stopOnEntry).  TCP/dlv adds
    "mode":"debug" so the adapter builds the package; stdio/gdb launches the
    prebuilt binary directly.  For the TCP transport the debuggee stays on the
    adapter's controlling pty (read in dap_read_message); for stdio the program's
    output arrives as "output" events, enabled below. */
 args = json_object_new_object();
 json_object_object_add(args, "request", json_object_new_string("launch"));
 if (!s->minimal_launch)
 {
  /* dlv/gdb launch: name the program (and, for dlv, the build mode).  The
     BSP/Bloop path omits these -- the build server already bound the main
     class via debugSession/start, and naming a program confuses it. */
  if (s->launch_mode_debug)
   json_object_object_add(args, "mode", json_object_new_string("debug"));
  json_object_object_add(args, "program", json_object_new_string(s->program));
  json_object_object_add(args, "stopOnEntry", json_object_new_boolean(0));
 }
 seq = dap_send(s, "launch", args);
 if (s->launch_mode_debug)
 {
  /* dlv builds the package at launch and replies BEFORE configurationDone, so
     wait here -- breakpoints set afterwards bind to the freshly built binary. */
  resp = dap_wait_response(s, seq);
  if (resp)
   json_object_put(resp);
 }
 /* gdb/lldb -dap follow the DAP spec and defer the launch response until AFTER
    configurationDone, so we must NOT block on it here (that would deadlock); the
    binary's symbols are already loaded, so breakpoints set now still bind, and
    the late launch response is consumed by dap_wait_stop below. */
 if (s->wait_init)
  /* scala-debug-adapter (via Bloop) is strict: it rejects setBreakpoints until
     it has emitted the `initialized` event after launch ("Empty debug session"
     otherwise).  Wait for it before configuring breakpoints. */
  dap_wait_event(s, "initialized");

 /* source breakpoints */
 if (s->bp_n > 0)
 {
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
  seq = dap_send(s, "setBreakpoints", args);
  resp = dap_wait_response(s, seq);
  if (resp)
   json_object_put(resp);
 }

 /* a function breakpoint at the entry (like gdb "break main") */
 if (s->entry_func)
 {
  args = json_object_new_object();
  bps = json_object_new_array();
  {
   struct json_object *b = json_object_new_object();
   json_object_object_add(b, "name", json_object_new_string(s->entry_func));
   json_object_array_add(bps, b);
  }
  json_object_object_add(args, "breakpoints", bps);
  seq = dap_send(s, "setFunctionBreakpoints", args);
  resp = dap_wait_response(s, seq);
  if (resp)
   json_object_put(resp);
 }

 seq = dap_send(s, "configurationDone", NULL);
 resp = dap_wait_response(s, seq);
 if (resp)
  json_object_put(resp);

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

void e_dap_close(e_dap_session *s)
{
 if (!s)
  return;
 if (s->sock >= 0)
 {
  /* best-effort disconnect */
  struct json_object *args = json_object_new_object();
  json_object_object_add(args, "terminateDebuggee", json_object_new_boolean(1));
  dap_send(s, "disconnect", args);
  close(s->sock);
 }
 if (s->in_fd >= 0 || s->out_fd >= 0)
 {
  /* stdio transport: ask the adapter to quit, then close the pipes */
  struct json_object *args = json_object_new_object();
  json_object_object_add(args, "terminateDebuggee", json_object_new_boolean(1));
  dap_send(s, "disconnect", args);
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
 free(s->program);
 free(s->entry_func);
 free(s);
}
