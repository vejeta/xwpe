/* we_dap.c -- DAP debug session engine.  See we_dap.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "we_dap.h"
#include "we_dap_proto.h"

#define DAP_MAX_BP   64
#define DAP_ACCEPT_MS 10000

struct e_dap_session {
 int           sock;          /* connected adapter socket           */
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
  write_all(s->sock, frame, len);
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
  char buf[4096];
  ssize_t n;

  if (m)
   return m;
  n = read(s->sock, buf, sizeof(buf));
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
  const char *txt = obj_str(body, "output");
  if (txt && s->host.on_output)
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

/* Fork+exec the adapter with --client-addr=127.0.0.1:port appended. */
static pid_t dap_spawn(char *const argv[], int port, const char *cwd, int listen_fd)
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
  /* keep the adapter quiet on our terminal */
  {
   int devnull = open("/dev/null", O_RDWR);
   if (devnull >= 0)
   {
    dup2(devnull, 1);
    dup2(devnull, 2);
    if (devnull > 2)
     close(devnull);
   }
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

/* ---- public API -------------------------------------------------------- */

e_dap_session *e_dap_open(char *const argv[], const char *program,
                          const char *entry_func, const char *cwd,
                          const e_dap_host *host)
{
 e_dap_session *s;
 int listen_fd, port, iseq;
 struct json_object *args, *resp;

 s = calloc(1, sizeof(*s));
 if (!s)
  return NULL;
 e_dap_reader_init(&s->rd);
 if (host)
  s->host = *host;
 s->program = program ? strdup(program) : NULL;
 s->entry_func = entry_func ? strdup(entry_func) : NULL;
 s->thread_id = 1;

 listen_fd = dap_listen_ephemeral(&port);
 if (listen_fd < 0)
 {
  e_dap_close(s);
  return NULL;
 }
 s->pid = dap_spawn(argv, port, cwd, listen_fd);
 if (s->pid <= 0)
 {
  close(listen_fd);
  e_dap_close(s);
  return NULL;
 }
 s->sock = dap_accept(listen_fd);
 close(listen_fd);
 if (s->sock < 0)
 {
  e_dap_close(s);
  return NULL;
 }

 /* initialize handshake */
 args = json_object_new_object();
 json_object_object_add(args, "clientID", json_object_new_string("xwpe"));
 json_object_object_add(args, "adapterID", json_object_new_string("dap"));
 json_object_object_add(args, "linesStartAt1", json_object_new_boolean(1));
 json_object_object_add(args, "columnsStartAt1", json_object_new_boolean(1));
 json_object_object_add(args, "pathFormat", json_object_new_string("path"));
 iseq = dap_send(s, "initialize", args);
 resp = dap_wait_response(s, iseq);
 if (!resp)
 {
  e_dap_close(s);
  return NULL;
 }
 json_object_put(resp);
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

 /* launch (build + run; stop is driven by breakpoints, not stopOnEntry) */
 args = json_object_new_object();
 json_object_object_add(args, "request", json_object_new_string("launch"));
 json_object_object_add(args, "mode", json_object_new_string("debug"));
 json_object_object_add(args, "program", json_object_new_string(s->program));
 json_object_object_add(args, "stopOnEntry", json_object_new_boolean(0));
 seq = dap_send(s, "launch", args);
 resp = dap_wait_response(s, seq);          /* dlv replies before config */
 if (resp)
  json_object_put(resp);

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
  return 0;
 }
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
 if (s->pid > 0)
 {
  int st;
  kill(s->pid, SIGTERM);
  waitpid(s->pid, &st, 0);
 }
 e_dap_reader_free(&s->rd);
 free(s->program);
 free(s->entry_func);
 free(s);
}
