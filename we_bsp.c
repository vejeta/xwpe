/* we_bsp.c -- Build Server Protocol bootstrap for JVM/Scala debugging.
 * See we_bsp.h for the why and the handshake overview. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "we_bsp.h"
#include "we_child.h"
#include "we_dap_proto.h"        /* e_dap_reader: shared Content-Length framing */
#include "we_fdloop.h"           /* wpe_fd_write: the server's input, queued */

/* Per-request ceilings.  The first compile may pull the Scala toolchain via
   coursier, so allow it to be slow; the wait is on poll() (event-driven), not a
   spin, and mirrors the synchronous start of the gdb/dlv backends. */
#define BSP_TMO_QUICK   30000
#define BSP_TMO_BUILD  240000
#define BSP_LAUNCHER_WHAT "the build server"   /* in the owner's lines */

struct e_bsp_session {
 WpeChild     launcher;       /* the BSP server's launcher (scala-cli bsp)  */
 int          in_fd;          /* write requests to the server's stdin      */
 int          out_fd;         /* read responses from the server's stdout   */
 e_dap_reader rd;             /* incoming frame reassembly (reused from DAP)*/
 int          id;             /* JSON-RPC id counter                       */
 int          input_lost;     /* a message could not be handed to it       */
};

/* ---- transport --------------------------------------------------------- */

/**
 * bsp_write_all - Hand one whole message to the build server.
 * @b: the session; @buf, @n: the framed message.
 *
 * Written now as far as the server reads; the rest waits on the loop's queue
 * and the message wait writes it on, so the editor never sits inside a write
 * to a server that stopped reading.  A message it can no longer be given
 * ends the handshake with "BSP send failed" instead of an answer that cannot
 * come.
 * Return: 0 when the message is on its way, else -1.
 */
static int bsp_write_all(e_bsp_session *b, const char *buf, size_t n)
{
 int r;

 if (b->input_lost)
  return -1;
 r = wpe_fd_write(b->in_fd, buf, n, BSP_LAUNCHER_WHAT, NULL);
 if (r == WPE_FD_WRITE_SENT || r == WPE_FD_WRITE_QUEUED)
  return 0;
 b->input_lost = 1;
 return -1;
}

/* Send a JSON-RPC message: id < 0 means a notification (no id field).  Takes
   ownership of `params` (may be NULL). */
static int bsp_send(e_bsp_session *b, int id, const char *method,
                    struct json_object *params)
{
 struct json_object *msg = json_object_new_object();
 const char *body;
 char *frame;
 size_t blen, flen;
 int rc;

 json_object_object_add(msg, "jsonrpc", json_object_new_string("2.0"));
 if (id >= 0)
  json_object_object_add(msg, "id", json_object_new_int(id));
 json_object_object_add(msg, "method", json_object_new_string(method));
 if (params)
  json_object_object_add(msg, "params", params);

 body = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
 blen = strlen(body);
 flen = blen + 40;
 frame = malloc(flen);
 if (!frame)
 {  json_object_put(msg);  return -1;  }
 rc = snprintf(frame, flen, "Content-Length: %zu\r\n\r\n%s", blen, body);
 rc = bsp_write_all(b, frame, (size_t)rc);
 free(frame);
 json_object_put(msg);
 return rc;
}

static long bsp_now_ms(void)
{
 struct timespec ts;
 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The two descriptors a message wait watches: the server's output, and its
   input while messages for it are still queued. */
#define BSP_WAIT_OUT 0
#define BSP_WAIT_IN  1

/**
 * bsp_wait_set - Fill the poll() set of a message wait.
 * @b: the session; @p: room for two entries.
 * Return: the entries used.
 *
 * The request waited on may still be queued behind a server that read
 * slowly; the wait writes it on as the server takes it, so its answer can
 * come.
 */
static int bsp_wait_set(e_bsp_session *b, struct pollfd *p)
{
 p[BSP_WAIT_OUT].fd = b->out_fd;
 p[BSP_WAIT_OUT].events = POLLIN;
 p[BSP_WAIT_OUT].revents = 0;
 if (wpe_fd_write_pending(b->in_fd) <= 0)
  return 1;
 p[BSP_WAIT_IN].fd = b->in_fd;
 p[BSP_WAIT_IN].events = POLLOUT;
 p[BSP_WAIT_IN].revents = 0;
 return 2;
}

/**
 * bsp_wait_flush - Write on what the server took of its queue; a server
 * that closed its input ends the wait.
 */
static void bsp_wait_flush(e_bsp_session *b)
{
 int r = wpe_fd_write_flush(b->in_fd);

 if (r == WPE_FD_WRITE_CLOSED || r == WPE_FD_WRITE_REFUSED)
  b->input_lost = 1;
}

/* Block (on poll) until one complete message arrives or the timeout elapses. */
static struct json_object *bsp_read_message(e_bsp_session *b, int timeout_ms)
{
 long deadline = bsp_now_ms() + timeout_ms;
 for (;;)
 {
  struct pollfd p[2];
  char buf[4096];
  ssize_t n;
  int err = 0, left, np;
  struct json_object *m = e_dap_reader_pop(&b->rd, &err);

  if (m)
   return m;
  if (b->input_lost)
   return NULL;
  left = (int)(deadline - bsp_now_ms());
  if (left <= 0)
   return NULL;
  np = bsp_wait_set(b, p);
  if (poll(p, np, left) <= 0)
   return NULL;
  if (np > BSP_WAIT_IN && p[BSP_WAIT_IN].revents)
   bsp_wait_flush(b);
  if (!p[BSP_WAIT_OUT].revents)
   continue;
  n = read(b->out_fd, buf, sizeof(buf));
  if (n <= 0)
  {
   if (n < 0 && errno == EINTR)
    continue;
   return NULL;
  }
  e_dap_reader_push(&b->rd, buf, (size_t)n);
 }
}

/* Send a request and pump until its response, dispatching (ignoring) the
   server's notifications (build/logMessage, build/taskProgress, ...).  Returns
   the "result" object (a NEW reference the caller must json_object_put), or NULL
   on error/timeout -- copying any server error message into errbuf. */
static struct json_object *bsp_request(e_bsp_session *b, const char *method,
                                       struct json_object *params, int timeout_ms,
                                       char *errbuf, size_t errsz)
{
 int id = ++b->id;
 if (bsp_send(b, id, method, params) != 0)
 {
  if (errbuf) snprintf(errbuf, errsz, "BSP send failed: %s", method);
  return NULL;
 }
 for (;;)
 {
  struct json_object *m = bsp_read_message(b, timeout_ms);
  struct json_object *jid = NULL, *res = NULL, *jerr = NULL;
  if (!m)
  {
   if (errbuf) snprintf(errbuf, errsz, "BSP timeout/EOF on %s", method);
   return NULL;
  }
  if (json_object_object_get_ex(m, "id", &jid) &&
      json_object_get_int(jid) == id)
  {
   if (json_object_object_get_ex(m, "error", &jerr))
   {
    struct json_object *jmsg = NULL;
    json_object_object_get_ex(jerr, "message", &jmsg);
    if (errbuf)
     snprintf(errbuf, errsz, "BSP error on %s: %s", method,
              jmsg ? json_object_get_string(jmsg) : "?");
    json_object_put(m);
    return NULL;
   }
   if (json_object_object_get_ex(m, "result", &res))
    json_object_get(res);          /* keep across the put below */
   json_object_put(m);
   return res;                      /* may be NULL for an empty result */
  }
  json_object_put(m);               /* a notification or another id */
 }
}

static void bsp_notify(e_bsp_session *b, const char *method,
                       struct json_object *params)
{
 bsp_send(b, -1, method, params);
}

/* ---- BSP connection descriptor (.bsp/<name>.json) ---------------------- */

/* Read the first ".json" file in workdir/.bsp and return its "argv" array as a
   NULL-terminated, malloc'd char* vector (each string malloc'd).  NULL if none. */
static char **bsp_read_connection_argv(const char *workdir)
{
 char path[PATH_MAX];
 DIR *d;
 struct dirent *de;
 char jsonpath[PATH_MAX];
 struct json_object *root, *argv;
 char **av = NULL;
 int n, i;

 snprintf(path, sizeof(path), "%s/.bsp", workdir);
 d = opendir(path);
 if (!d)
  return NULL;
 jsonpath[0] = '\0';
 while ((de = readdir(d)))
 {
  size_t l = strlen(de->d_name);
  if (l > 5 && !strcmp(de->d_name + l - 5, ".json"))
  {
   snprintf(jsonpath, sizeof(jsonpath), "%s/%s", path, de->d_name);
   break;
  }
 }
 closedir(d);
 if (!jsonpath[0])
  return NULL;
 root = json_object_from_file(jsonpath);
 if (!root)
  return NULL;
 if (!json_object_object_get_ex(root, "argv", &argv) ||
     !json_object_is_type(argv, json_type_array))
 {  json_object_put(root);  return NULL;  }
 n = json_object_array_length(argv);
 av = calloc(n + 1, sizeof(char *));
 if (av)
  for (i = 0; i < n; i++)
   av[i] = strdup(json_object_get_string(json_object_array_get_idx(argv, i)));
 json_object_put(root);
 return av;
}

static void bsp_free_argv(char **av)
{
 int i;
 if (!av)
  return;
 for (i = 0; av[i]; i++)
  free(av[i]);
 free(av);
}

/**
 * bsp_spawn - Start the BSP server's launcher (argv from the connection
 * file) on two pipes, in the project's directory.
 * @b: the session; @argv: the command line; @workdir: the project.
 *
 * Its stderr goes nowhere: scala-cli and Bloop are chatty there ("Starting
 * compilation server", compile progress), which would bleed onto the
 * terminal screen.  Signals reach the launcher alone, never its group: the
 * shared build server (Bloop) may be its child, and other editors and the
 * language server go on using it.
 * Return: 0, or -1 when it could not be started.
 */
static int bsp_spawn(e_bsp_session *b, char **argv, const char *workdir)
{
 WpeChildSpec spec;

 memset(&spec, 0, sizeof spec);
 spec.argv = argv;
 spec.cwd = workdir;
 spec.io = WPE_CHILD_IO_PIPES;
 spec.group = WPE_CHILD_OWN_SESSION;
 spec.stderr_to_null = 1;
 spec.signal_leader_only = 1;
 spec.what = BSP_LAUNCHER_WHAT;
 if (wpe_child_start(&spec, &b->launcher, NULL, NULL) != 0)
  return -1;
 b->in_fd = b->launcher.in_fd;
 b->out_fd = b->launcher.out_fd;
 return 0;
}

/* ---- handshake helpers ------------------------------------------------- */

/* From a workspace/buildTargets result, copy the id of the first NON-test
   target (a fresh reference the caller owns).  Bloop exposes a "<name>" and a
   "<name>-test" target; debugSession/start stalls if given the test target. */
static struct json_object *bsp_pick_main_target(struct json_object *result)
{
 struct json_object *targets = NULL, *t, *id, *uri;
 int n, i;

 if (!result || !json_object_object_get_ex(result, "targets", &targets))
  return NULL;
 n = json_object_array_length(targets);
 for (i = 0; i < n; i++)
 {
  t = json_object_array_get_idx(targets, i);
  if (!json_object_object_get_ex(t, "id", &id))
   continue;
  if (json_object_object_get_ex(id, "uri", &uri))
  {
   const char *u = json_object_get_string(uri);
   size_t l = u ? strlen(u) : 0;
   if (l >= 5 && !strcmp(u + l - 5, "-test"))
    continue;
  }
  return json_object_get(id);        /* own a reference to the id object */
 }
 return NULL;
}

/* Wrap a target id in a fresh {"targets":[id]} arguments object. */
static struct json_object *bsp_targets_arg(struct json_object *id)
{
 struct json_object *args = json_object_new_object();
 struct json_object *arr = json_object_new_array();
 json_object_array_add(arr, json_object_get(id));
 json_object_object_add(args, "targets", arr);
 return args;
}

/* From a buildTarget/scalaMainClasses result, copy the first main class name. */
static int bsp_first_main_class(struct json_object *result, char *out, size_t osz)
{
 struct json_object *items = NULL, *it, *classes, *c, *name;

 if (!result || !json_object_object_get_ex(result, "items", &items))
  return -1;
 if (json_object_array_length(items) == 0)
  return -1;
 it = json_object_array_get_idx(items, 0);
 if (!json_object_object_get_ex(it, "classes", &classes) ||
     json_object_array_length(classes) == 0)
  return -1;
 c = json_object_array_get_idx(classes, 0);
 if (!json_object_object_get_ex(c, "class", &name))
  return -1;
 snprintf(out, osz, "%s", json_object_get_string(name));
 return 0;
}

/* Parse "tcp://HOST:PORT" into host/port; map the wildcard 0.0.0.0 to loopback. */
static int bsp_parse_uri(const char *uri, char *host, size_t hsz, int *port)
{
 const char *p, *colon;
 size_t hlen;

 if (!uri || !(p = strstr(uri, "://")))
  return -1;
 p += 3;
 colon = strrchr(p, ':');
 if (!colon)
  return -1;
 hlen = (size_t)(colon - p);
 if (hlen == 0 || hlen >= hsz)
  return -1;
 memcpy(host, p, hlen);
 host[hlen] = '\0';
 if (!strcmp(host, "0.0.0.0"))
  snprintf(host, hsz, "127.0.0.1");
 *port = atoi(colon + 1);
 return (*port > 0) ? 0 : -1;
}

/* ---- handshake stages (each owns one BSP exchange) --------------------- */

/* Resolve the BSP connection file's argv.  The caller has had `scala-cli
   setup-ide` write it (e_bsp_needs_setup) without waiting for it here.
   Returns a malloc'd argv or NULL. */
static char **bsp_connection_argv(const char *rootdir, char *errbuf, size_t errsz)
{
 char **argv = bsp_read_connection_argv(rootdir);

 if (!argv && errbuf)
  snprintf(errbuf, errsz, "no .bsp connection file in %s", rootdir);
 return argv;
}

/**
 * e_bsp_needs_setup - Does the project still lack its BSP connection file?
 * @workdir: the project's directory.
 *
 * The first Scala debug session in a project needs `scala-cli setup-ide` to
 * write it; the editor runs that command as a child of its own, keeping its
 * keys, and starts the build server once it has ended.
 * Return: 1 when the file is missing.
 */
int e_bsp_needs_setup(const char *workdir)
{
 char rootdir[PATH_MAX];
 char **argv;

 if (!realpath(workdir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", workdir);
 argv = bsp_read_connection_argv(rootdir);
 if (!argv)
  return 1;
 bsp_free_argv(argv);
 return 0;
}

/* build/initialize (advertising scala+java) followed by the build/initialized
   notification.  Returns 0 on success. */
static int bsp_handshake_init(e_bsp_session *b, const char *rootdir,
                              char *errbuf, size_t errsz)
{
 struct json_object *args, *caps, *langs, *res;
 char rooturi[PATH_MAX + 8];

 args = json_object_new_object();
 json_object_object_add(args, "displayName", json_object_new_string("xwpe"));
 json_object_object_add(args, "version", json_object_new_string("1.0"));
 json_object_object_add(args, "bspVersion", json_object_new_string("2.1.1"));
 snprintf(rooturi, sizeof(rooturi), "file://%s", rootdir);
 json_object_object_add(args, "rootUri", json_object_new_string(rooturi));
 caps = json_object_new_object();
 langs = json_object_new_array();
 json_object_array_add(langs, json_object_new_string("scala"));
 json_object_array_add(langs, json_object_new_string("java"));
 json_object_object_add(caps, "languageIds", langs);
 json_object_object_add(args, "capabilities", caps);
 res = bsp_request(b, "build/initialize", args, BSP_TMO_BUILD, errbuf, errsz);
 if (!res)
  return -1;
 json_object_put(res);
 bsp_notify(b, "build/initialized", NULL);
 return 0;
}

/* workspace/buildTargets -> buildTarget/compile -> buildTarget/scalaMainClasses.
   Returns the main target id (caller json_object_put's it) and fills `mainclass`
   with the class to launch, or NULL on failure. */
static struct json_object *bsp_resolve_main_target(e_bsp_session *b,
                                                   char *mainclass, size_t mcsz,
                                                   char *errbuf, size_t errsz)
{
 struct json_object *res, *id;

 res = bsp_request(b, "workspace/buildTargets", NULL, BSP_TMO_BUILD, errbuf, errsz);
 if (!res)
  return NULL;
 id = bsp_pick_main_target(res);
 json_object_put(res);
 if (!id)
 {
  if (errbuf) snprintf(errbuf, errsz, "no buildable target found");
  return NULL;
 }

 res = bsp_request(b, "buildTarget/compile", bsp_targets_arg(id),
                   BSP_TMO_BUILD, errbuf, errsz);
 if (res)
  json_object_put(res);

 res = bsp_request(b, "buildTarget/scalaMainClasses", bsp_targets_arg(id),
                   BSP_TMO_BUILD, errbuf, errsz);
 if (!res || bsp_first_main_class(res, mainclass, mcsz) != 0)
 {
  if (res) json_object_put(res);
  if (errbuf && !errbuf[0]) snprintf(errbuf, errsz, "no main class found");
  json_object_put(id);
  return NULL;
 }
 json_object_put(res);
 return id;
}

/* debugSession/start {dataKind:"scala-main-class"} -> parse the tcp:// endpoint
   into host/port.  Returns 0 on success.  Consumes (puts) `id`. */
static int bsp_start_debug_session(e_bsp_session *b, struct json_object *id,
                                   const char *mainclass, char *host,
                                   size_t hostsz, int *port,
                                   char *errbuf, size_t errsz)
{
 struct json_object *args, *data, *res, *uri;
 int rc = -1;

 args = bsp_targets_arg(id);
 json_object_put(id);
 json_object_object_add(args, "dataKind", json_object_new_string("scala-main-class"));
 data = json_object_new_object();
 json_object_object_add(data, "class", json_object_new_string(mainclass));
 json_object_object_add(data, "arguments", json_object_new_array());
 json_object_object_add(data, "jvmOptions", json_object_new_array());
 json_object_object_add(data, "environmentVariables", json_object_new_array());
 json_object_object_add(args, "data", data);

 res = bsp_request(b, "debugSession/start", args, BSP_TMO_BUILD, errbuf, errsz);
 if (res && json_object_object_get_ex(res, "uri", &uri) &&
     bsp_parse_uri(json_object_get_string(uri), host, hostsz, port) == 0)
  rc = 0;
 else if (errbuf && !errbuf[0])
  snprintf(errbuf, errsz, "no DAP endpoint from debugSession/start");
 if (res)
  json_object_put(res);
 return rc;
}

/* Spawn the BSP server from its connection-file argv, cwd = rootdir. */
static e_bsp_session *bsp_session_spawn(const char *rootdir, char *errbuf, size_t errsz)
{
 e_bsp_session *b;
 char **argv = bsp_connection_argv(rootdir, errbuf, errsz);

 if (!argv)
  return NULL;
 b = calloc(1, sizeof(*b));
 if (!b)
 {  bsp_free_argv(argv);  return NULL;  }
 e_dap_reader_init(&b->rd);
 b->in_fd = b->out_fd = -1;
 if (bsp_spawn(b, argv, rootdir) != 0)
 {
  bsp_free_argv(argv);
  if (errbuf) snprintf(errbuf, errsz, "could not start the BSP server");
  e_bsp_close(b);
  return NULL;
 }
 bsp_free_argv(argv);
 return b;
}

/* ---- public API -------------------------------------------------------- */

e_bsp_session *e_bsp_start(const char *workdir, const char *srcfile,
                           char *host, size_t hostsz, int *port,
                           char *mainclass, size_t mcsz,
                           char *errbuf, size_t errsz)
{
 e_bsp_session *b;
 struct json_object *id;
 char rootdir[PATH_MAX], dbgclass[256];

 (void)srcfile;
 if (errbuf && errsz)
  errbuf[0] = '\0';
 if (!realpath(workdir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", workdir);

 b = bsp_session_spawn(rootdir, errbuf, errsz);
 if (!b)
  return NULL;
 if (bsp_handshake_init(b, rootdir, errbuf, errsz) != 0)
 {  e_bsp_close(b);  return NULL;  }

 id = bsp_resolve_main_target(b, dbgclass, sizeof(dbgclass), errbuf, errsz);
 if (!id)
 {  e_bsp_close(b);  return NULL;  }
 if (mainclass && mcsz)
  snprintf(mainclass, mcsz, "%s", dbgclass);

 if (bsp_start_debug_session(b, id, dbgclass, host, hostsz, port,
                             errbuf, errsz) != 0)
 {  e_bsp_close(b);  return NULL;  }
 return b;
}

/* ---- the end of the build server --------------------------------------- */

/**
 * e_bsp_close - End the build server's launcher and free the session,
 * without waiting.
 * @b: the session.
 *
 * Quit returns at once: the launcher gets SIGTERM, so it can close its
 * connection to the build server cleanly, and the owner of the editor's
 * children gives it the grace and then SIGKILL, on the loop.  Only the
 * launcher is signalled, never its group (see bsp_spawn).  The program being
 * debugged is not the launcher's: the adapter ends it on Quit's
 * `disconnect`, before this.
 */
void e_bsp_close(e_bsp_session *b)
{
 if (!b)
  return;
 if (b->in_fd >= 0)
  wpe_fd_close(b->in_fd);          /* what still waits for it goes too */
 if (b->out_fd >= 0)
  close(b->out_fd);
 wpe_child_end_detached(&b->launcher, SIGTERM);
 e_dap_reader_free(&b->rd);
 free(b);
}
