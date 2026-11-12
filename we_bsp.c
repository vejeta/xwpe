/* we_bsp.c -- Build Server Protocol bootstrap for JVM/Scala debugging.
 * See we_bsp.h for the why and the handshake overview. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "we_bsp.h"
#include "we_child.h"
#include "we_dap_proto.h"        /* e_dap_reader: shared Content-Length framing */
#include "we_fdloop.h"           /* wpe_fd_write: the server's input, queued */

/* Each request's deadline.  The first compile may pull the Scala toolchain
   via coursier, so it is allowed to be slow; the user keeps the editor
   meanwhile and Esc gives the start up. */
#define BSP_TMO_BUILD  240000
#define BSP_LAUNCHER_WHAT "the build server"   /* in the owner's lines */
#define BSP_NO_ID      (-1)
#define BSP_METHOD_MAX  64       /* "buildTarget/scalaMainClasses" fits */
#define BSP_ERR_MAX    256
#define BSP_HOST_MAX    64
#define BSP_CLASS_MAX  256
#define BSP_READ_CHUNK 4096
#define BSP_READS_MAX    64      /* reads per call, so a flood cannot hold the loop */
#define BSP_TIMEOUT_FMT "%s did not answer `%s` within %ld s"

struct e_bsp_session {
 WpeChild     launcher;       /* the BSP server's launcher (scala-cli bsp)  */
 int          in_fd;          /* write requests to the server's stdin      */
 int          out_fd;         /* read responses from the server's stdout   */
 e_dap_reader rd;             /* incoming frame reassembly (reused from DAP)*/
 int          id;             /* JSON-RPC id counter                       */
 int          input_lost;     /* a message could not be handed to it       */
 int          eof;            /* its output has ended                      */
 /* The one request out, for the start's chain. */
 const E_D_REQ_IO *rio;       /* the host's clock, timers and lines, or NULL */
 int          out_id;         /* its id, or BSP_NO_ID                      */
 char         out_method[BSP_METHOD_MAX];
 long         sent_ms;
 int          waiting_shown;
 e_d_answered_fn out_fn;
 void        *out_ctx;
 /* What the next step sends, kept here so a long argument goes out whole. */
 char         staged_cmd[BSP_METHOD_MAX];
 struct json_object *staged_args;
 /* What the start learns. */
 char         rootdir[PATH_MAX];
 struct json_object *target;  /* the main build target's id               */
 char         mainclass[BSP_CLASS_MAX];
 char         host[BSP_HOST_MAX];
 int          port;
 char         err[BSP_ERR_MAX];
 int          failed;         /* a step found what it needed missing       */
 int          starting;       /* the chain runs                            */
 int          closing;        /* e_bsp_close gives the start up            */
 e_bsp_started_fn done;
 void        *done_ctx;
};

/* ---- transport --------------------------------------------------------- */

/**
 * bsp_write_all - Hand one whole message to the build server.
 * @b: the session; @buf, @n: the framed message.
 *
 * Written now as far as the server reads; the rest waits on the loop's queue,
 * so the editor never sits inside a write to a server that stopped reading.
 * A message it can no longer be given ends the start with "BSP send failed"
 * instead of an answer that cannot come.
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

static void bsp_notify(e_bsp_session *b, const char *method,
                       struct json_object *params)
{
 bsp_send(b, -1, method, params);
}

/* ---- the one request out ------------------------------------------------ *
 *
 * The start asks one thing at a time (each answer decides the next
 * request), so one slot holds the request out.  Its answer is read on the
 * loop; the host's timers show the waiting line and give it up at its
 * deadline; an answer to a request given up on is dropped by its id.
 */

/** bsp_now - The host's clock, or 0 when the host gave none. */
static long bsp_now(const e_bsp_session *b)
{
 if (!b->rio || !b->rio->now_ms)
  return 0;
 return b->rio->now_ms(b->rio->io);
}

/** bsp_arm - Ask the host for timer @which in @ms, or for none. */
static void bsp_arm(const e_bsp_session *b, int which, long ms)
{
 if (!b->rio || !b->rio->arm_timer)
  return;
 b->rio->arm_timer(which, ms, b->rio->io);
}

/** bsp_disarm_all - Take back every timer the request asked for. */
static void bsp_disarm_all(e_bsp_session *b)
{
 int which;

 for (which = 0; which < E_D_TIMERS; which++)
  bsp_arm(b, which, E_D_TIMER_OFF);
}

/** bsp_stage_clear - Forget what a step built and did not send. */
static void bsp_stage_clear(e_bsp_session *b)
{
 if (b->staged_args)
  json_object_put(b->staged_args);
 b->staged_args = NULL;
 b->staged_cmd[0] = '\0';
}

/**
 * bsp_stage - Keep @args for the request @method a step is about to make,
 * and write its name as the step's command.
 * @b: the session; @method: the BSP method; @args: its arguments (taken
 * over, may be NULL); @cmd, @n: the step's command buffer.
 * Return: 1 (the step sends).
 */
static int bsp_stage(e_bsp_session *b, const char *method,
                     struct json_object *args, char *cmd, size_t n)
{
 bsp_stage_clear(b);
 snprintf(b->staged_cmd, sizeof b->staged_cmd, "%s", method);
 b->staged_args = args;
 snprintf(cmd, n, "%s", method);
 return 1;
}

/**
 * bsp_out_take - Free the slot of the request out and hand back who is
 * told; an answer that still comes for it is dropped.
 */
static void bsp_out_take(e_bsp_session *b, e_d_answered_fn *fn, void **ctx)
{
 *fn = b->out_fn;
 *ctx = b->out_ctx;
 b->out_id = BSP_NO_ID;
 b->out_fn = NULL;
 b->out_ctx = NULL;
 b->waiting_shown = 0;
 bsp_disarm_all(b);
}

/**
 * bsp_out_end - End the request out without an answer: @why tells how, and
 * the reason the start says is kept.
 */
static void bsp_out_end(e_bsp_session *b, E_D_REQ_END why)
{
 e_d_answered_fn fn;
 void *ctx;
 E_D_ANSWER empty;

 if (b->out_id == BSP_NO_ID)
  return;
 if (!b->err[0])
  snprintf(b->err, sizeof b->err, "BSP timeout/EOF on %s", b->out_method);
 bsp_out_take(b, &fn, &ctx);
 memset(&empty, 0, sizeof empty);
 if (fn)
  fn(why, &empty, ctx);
}

/**
 * bsp_req_request - The chain's requester: send the staged request @cmd.
 * @engine: the session; @cmd: the method; @answered, @ctx: who is told.
 * Return: E_D_SEND_OK, or why it was not sent (then nobody is told).
 */
static int bsp_req_request(void *engine, const char *cmd,
                           e_d_answered_fn answered, void *ctx)
{
 e_bsp_session *b = engine;
 struct json_object *args = NULL;

 if (b->staged_cmd[0] && !strcmp(b->staged_cmd, cmd))
 {
  args = b->staged_args;
  b->staged_args = NULL;
 }
 bsp_stage_clear(b);
 if (b->eof || b->input_lost || b->out_id != BSP_NO_ID)
 {
  if (args)
   json_object_put(args);
  return E_D_SEND_GONE;
 }
 b->out_id = ++b->id;
 if (bsp_send(b, b->out_id, cmd, args) != 0)
 {
  b->out_id = BSP_NO_ID;
  snprintf(b->err, sizeof b->err, "BSP send failed: %s", cmd);
  return E_D_SEND_GONE;
 }
 snprintf(b->out_method, sizeof b->out_method, "%s", cmd);
 b->out_fn = answered;
 b->out_ctx = ctx;
 b->sent_ms = bsp_now(b);
 b->waiting_shown = 0;
 bsp_arm(b, E_D_TIMER_QUIET, E_D_REQ_QUIET_MS);
 bsp_arm(b, E_D_TIMER_DEADLINE, BSP_TMO_BUILD);
 return E_D_SEND_OK;
}

/**
 * bsp_error_of - Keep the reason a refused request gives.
 * @b: the session; @m: the response holding "error".
 */
static void bsp_error_of(e_bsp_session *b, struct json_object *m)
{
 struct json_object *jerr = NULL, *jmsg = NULL;

 if (!json_object_object_get_ex(m, "error", &jerr))
  return;
 json_object_object_get_ex(jerr, "message", &jmsg);
 snprintf(b->err, sizeof b->err, "BSP error on %s: %s", b->out_method,
          jmsg ? json_object_get_string(jmsg) : "?");
}

/**
 * bsp_answered - The response @m to the request out has come: hand its
 * text (the whole response) to who asked, with the error flag set for a
 * refusal.
 */
static void bsp_answered(e_bsp_session *b, struct json_object *m)
{
 const char *text = json_object_to_json_string_ext(m, JSON_C_TO_STRING_PLAIN);
 e_d_answered_fn fn;
 void *ctx;
 E_D_ANSWER ans;

 memset(&ans, 0, sizeof ans);
 ans.text = text ? strdup(text) : NULL;
 ans.len = ans.text ? strlen(ans.text) : 0;
 ans.error = json_object_object_get_ex(m, "error", NULL);
 if (ans.error)
  bsp_error_of(b, m);
 bsp_out_take(b, &fn, &ctx);
 if (fn)
  fn(E_D_REQ_DONE, &ans, ctx);
 free(ans.text);
}

/**
 * bsp_dispatch - One message from the build server: the answer to the
 * request out, or something nobody waits for (build/logMessage,
 * build/taskProgress, a late answer), dropped.
 */
static void bsp_dispatch(e_bsp_session *b, struct json_object *m)
{
 struct json_object *jid = NULL;

 if (json_object_object_get_ex(m, "method", NULL))
  return;
 if (!json_object_object_get_ex(m, "id", &jid))
  return;
 if (b->out_id != BSP_NO_ID && json_object_get_int(jid) == b->out_id)
  bsp_answered(b, m);
}

int e_bsp_read_now(e_bsp_session *b)
{
 int reads = 0;

 if (!b || b->out_fd < 0)
  return 0;
 while (!b->eof && reads++ < BSP_READS_MAX)
 {
  char buf[BSP_READ_CHUNK];
  ssize_t n;

  if (wpe_fd_readable_now(b->out_fd) != WPE_FD_READABLE)
   break;
  n = read(b->out_fd, buf, sizeof buf);
  if (n == 0)
   b->eof = 1;
  if (n <= 0)
   break;
  e_dap_reader_push(&b->rd, buf, (size_t)n);
 }
 for (;;)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&b->rd, &err);

  if (!m)
   break;
  bsp_dispatch(b, m);
  json_object_put(m);
 }
 if (b->eof)
  bsp_out_end(b, E_D_REQ_GONE);
 return !b->eof;
}

/**
 * bsp_say_waiting - The waiting line for the request out.
 */
static void bsp_say_waiting(e_bsp_session *b)
{
 long secs = (bsp_now(b) - b->sent_ms) / E_D_REQ_SECOND_MS;

 b->waiting_shown = 1;
 if (b->rio->waiting)
  b->rio->waiting(b->out_method, secs, b->rio->io);
 bsp_arm(b, E_D_TIMER_SECOND, E_D_REQ_SECOND_MS);
}

/**
 * bsp_say_given_up - The deadline's sentence on the status row.
 */
static void bsp_say_given_up(e_bsp_session *b)
{
 char msg[E_D_REQ_MSG_MAX];
 const char *name = b->rio->name ? b->rio->name : BSP_LAUNCHER_WHAT;

 snprintf(b->err, sizeof b->err, BSP_TIMEOUT_FMT, name, b->out_method,
          (long)(BSP_TMO_BUILD / E_D_REQ_SECOND_MS));
 if (!b->rio->notice)
  return;
 snprintf(msg, sizeof msg, "%s", b->err);
 b->rio->notice(msg, b->rio->io);
}

void e_bsp_request_timer(e_bsp_session *b, int which)
{
 if (!b || !b->rio || b->out_id == BSP_NO_ID)
  return;                       /* a timer of a request that has ended */
 if (which == E_D_TIMER_QUIET)
  bsp_say_waiting(b);
 else if (which == E_D_TIMER_SECOND && b->waiting_shown)
  bsp_say_waiting(b);
 else if (which == E_D_TIMER_DEADLINE)
 {
  bsp_say_given_up(b);
  bsp_out_end(b, E_D_REQ_TIMED_OUT);
 }
}

/* ---- BSP connection descriptor (.bsp/<name>.json) ---------------------- */

/* Asked before a connection file's command starts (e_bsp_set_approver). */
static e_bsp_approve_fn bsp_approver;

void e_bsp_set_approver(e_bsp_approve_fn fn)
{
 bsp_approver = fn;
}

/* Read the first ".json" file in workdir/.bsp and return its "argv" array as a
   NULL-terminated, malloc'd char* vector (each string malloc'd).  NULL if none.
   @file/@filesz receive the connection file's name. */
/* The connection file scala-cli writes, read by name before any other. */
#define BSP_SCALA_CLI_FILE "scala-cli.json"
#define BSP_JSON_SUFFIX    ".json"

/**
 * bsp_connection_file - Which connection file of a project's .bsp is read.
 * @dir: the project's .bsp folder; @file/@filesz: receive the file's name.
 *
 * scala-cli's own file by name when it is there; else the first ".json" by
 * name, so the choice never depends on the order the folder lists them in.
 * Return: 1 when one was found.
 */
static int bsp_connection_file(const char *dir, char *file, size_t filesz)
{
 char path[PATH_MAX];
 DIR *d;
 struct dirent *de;
 struct stat st;
 size_t l;

 file[0] = '\0';
 snprintf(path, sizeof(path), "%s/%s", dir, BSP_SCALA_CLI_FILE);
 if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
 {
  snprintf(file, filesz, "%s", BSP_SCALA_CLI_FILE);
  return 1;
 }
 d = opendir(dir);
 if (!d)
  return 0;
 while ((de = readdir(d)))
 {
  l = strlen(de->d_name);
  if (l <= strlen(BSP_JSON_SUFFIX) || strcmp(de->d_name + l - strlen(BSP_JSON_SUFFIX), BSP_JSON_SUFFIX))
   continue;
  if (!file[0] || strcmp(de->d_name, file) < 0)
   snprintf(file, filesz, "%s", de->d_name);
 }
 closedir(d);
 return file[0] != '\0';
}

static char **bsp_read_connection_argv(const char *workdir, char *file, size_t filesz)
{
 char path[PATH_MAX];
 char jsonpath[PATH_MAX];
 struct json_object *root, *argv;
 char **av = NULL;
 int n, i;

 snprintf(path, sizeof(path), "%s/.bsp", workdir);
 if (!bsp_connection_file(path, file, filesz))
  return NULL;
 snprintf(jsonpath, sizeof(jsonpath), "%s/%s", path, file);
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
  {
   const char *word = json_object_get_string(json_object_array_get_idx(argv, i));
   if (!word)
    word = "";                            /* a null entry is an empty word */
   av[i] = strdup(word);
  }
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
 * e_bsp_connection_argv - The command a project's .bsp connection file names.
 * @workdir: the project; @file/@filesz: receive the connection file's name.
 *
 * The same file and words the start would use, so the editor can note
 * exactly what its own `scala-cli setup-ide` just wrote.
 * Return: the malloc'd argv (free with e_bsp_free_argv), or NULL.
 */
char **e_bsp_connection_argv(const char *workdir, char *file, size_t filesz)
{
 char rootdir[PATH_MAX];

 if (!realpath(workdir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", workdir);
 return bsp_read_connection_argv(rootdir, file, filesz);
}

/** e_bsp_free_argv - Free what e_bsp_connection_argv gave. */
void e_bsp_free_argv(char **argv)
{
 bsp_free_argv(argv);
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
 char rootdir[PATH_MAX], file[PATH_MAX];
 char **argv;

 if (!realpath(workdir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", workdir);
 argv = bsp_read_connection_argv(rootdir, file, sizeof file);
 if (!argv)
  return 1;
 bsp_free_argv(argv);
 return 0;
}

/* ---- the start's chain: one step per request ---------------------------- */

/**
 * bsp_answer_result - The "result" of a step's answer, as a new reference
 * the caller puts, or NULL for none.
 * @ans: the answer (the whole response's text).
 */
static struct json_object *bsp_answer_result(const E_D_ANSWER *ans)
{
 struct json_object *m, *res = NULL;

 if (!ans || !ans->text)
  return NULL;
 m = json_tokener_parse(ans->text);
 if (!m)
  return NULL;
 if (json_object_object_get_ex(m, "result", &res))
  json_object_get(res);
 json_object_put(m);
 return res;
}

/**
 * bsp_step_fails - A step found what it needed missing: the start ends
 * with @reason (unless the server's own refusal already said why).
 * Return: E_D_STEP_STOP.
 */
static int bsp_step_fails(e_bsp_session *b, const char *reason)
{
 b->failed = 1;
 if (!b->err[0])
  snprintf(b->err, sizeof b->err, "%s", reason);
 return E_D_STEP_STOP;
}

/** bsp_init_cmd - build/initialize, advertising scala and java. */
static int bsp_init_cmd(char *cmd, size_t n, void *ctx)
{
 e_bsp_session *b = ctx;
 struct json_object *args, *caps, *langs;
 char rooturi[PATH_MAX + 8];

 args = json_object_new_object();
 json_object_object_add(args, "displayName", json_object_new_string("xwpe"));
 json_object_object_add(args, "version", json_object_new_string("1.0"));
 json_object_object_add(args, "bspVersion", json_object_new_string("2.1.1"));
 snprintf(rooturi, sizeof(rooturi), "file://%s", b->rootdir);
 json_object_object_add(args, "rootUri", json_object_new_string(rooturi));
 caps = json_object_new_object();
 langs = json_object_new_array();
 json_object_array_add(langs, json_object_new_string("scala"));
 json_object_array_add(langs, json_object_new_string("java"));
 json_object_object_add(caps, "languageIds", langs);
 json_object_object_add(args, "capabilities", caps);
 return bsp_stage(b, "build/initialize", args, cmd, n);
}

/** bsp_init_answered - Initialized: the build/initialized notification. */
static int bsp_init_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 e_bsp_session *b = ctx;

 (void)how;
 if (ans->error)
  return bsp_step_fails(b, "BSP error on build/initialize");
 bsp_notify(b, "build/initialized", NULL);
 return E_D_STEP_NEXT;
}

/** bsp_targets_cmd - workspace/buildTargets. */
static int bsp_targets_cmd(char *cmd, size_t n, void *ctx)
{
 return bsp_stage(ctx, "workspace/buildTargets", NULL, cmd, n);
}

/** bsp_targets_answered - Keep the first target that is not a test's. */
static int bsp_targets_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 e_bsp_session *b = ctx;
 struct json_object *res = bsp_answer_result(ans);

 (void)how;
 b->target = bsp_pick_main_target(res);
 if (res)
  json_object_put(res);
 if (!b->target)
  return bsp_step_fails(b, "no buildable target found");
 return E_D_STEP_NEXT;
}

/** bsp_compile_cmd - buildTarget/compile of the main target. */
static int bsp_compile_cmd(char *cmd, size_t n, void *ctx)
{
 e_bsp_session *b = ctx;

 return bsp_stage(b, "buildTarget/compile", bsp_targets_arg(b->target), cmd, n);
}

/**
 * bsp_compile_answered - The compile ended: the main classes are asked
 * whatever it said (a failed compile shows as no main class, with the
 * compiler's refusal as the reason).
 */
static int bsp_compile_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 (void)how;
 (void)ans;
 (void)ctx;
 return E_D_STEP_NEXT;
}

/** bsp_mains_cmd - buildTarget/scalaMainClasses of the main target. */
static int bsp_mains_cmd(char *cmd, size_t n, void *ctx)
{
 e_bsp_session *b = ctx;

 return bsp_stage(b, "buildTarget/scalaMainClasses", bsp_targets_arg(b->target),
                  cmd, n);
}

/** bsp_mains_answered - Keep the first main class. */
static int bsp_mains_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 e_bsp_session *b = ctx;
 struct json_object *res = bsp_answer_result(ans);
 int found;

 (void)how;
 found = (bsp_first_main_class(res, b->mainclass, sizeof b->mainclass) == 0);
 if (res)
  json_object_put(res);
 if (!found)
  return bsp_step_fails(b, "no main class found");
 return E_D_STEP_NEXT;
}

/** bsp_debug_cmd - debugSession/start for the main class. */
static int bsp_debug_cmd(char *cmd, size_t n, void *ctx)
{
 e_bsp_session *b = ctx;
 struct json_object *args = bsp_targets_arg(b->target), *data;

 json_object_object_add(args, "dataKind", json_object_new_string("scala-main-class"));
 data = json_object_new_object();
 json_object_object_add(data, "class", json_object_new_string(b->mainclass));
 json_object_object_add(data, "arguments", json_object_new_array());
 json_object_object_add(data, "jvmOptions", json_object_new_array());
 json_object_object_add(data, "environmentVariables", json_object_new_array());
 json_object_object_add(args, "data", data);
 return bsp_stage(b, "debugSession/start", args, cmd, n);
}

/** bsp_debug_answered - Keep the DAP endpoint the debug session listens on. */
static int bsp_debug_answered(E_D_REQ_END how, E_D_ANSWER *ans, void *ctx)
{
 e_bsp_session *b = ctx;
 struct json_object *res = bsp_answer_result(ans), *uri = NULL;
 int ok;

 (void)how;
 ok = res && json_object_object_get_ex(res, "uri", &uri) &&
      bsp_parse_uri(json_object_get_string(uri), b->host, sizeof b->host,
                    &b->port) == 0;
 if (res)
  json_object_put(res);
 if (!ok)
  return bsp_step_fails(b, "no DAP endpoint from debugSession/start");
 return E_D_STEP_STOP;
}

/* The start: each answer decides the next request. */
static const E_D_STEP bsp_start_steps[] = {
 { bsp_init_cmd,    bsp_init_answered },
 { bsp_targets_cmd, bsp_targets_answered },
 { bsp_compile_cmd, bsp_compile_answered },
 { bsp_mains_cmd,   bsp_mains_answered },
 { bsp_debug_cmd,   bsp_debug_answered },
 { NULL, NULL }
};

/**
 * bsp_start_finished - The chain ended: tell the owner, unless the start
 * was given up by e_bsp_close (then nobody waits for it any more).
 */
static void bsp_start_finished(int ok, void *ctx)
{
 e_bsp_session *b = ctx;
 int good = ok && !b->failed;

 b->starting = 0;
 if (!good && !b->err[0])
  snprintf(b->err, sizeof b->err, "the build server did not start");
 if (b->closing || !b->done)
  return;
 b->done(good, b->done_ctx);
}

/* Spawn the BSP server from its connection-file argv, cwd = rootdir. */
static e_bsp_session *bsp_session_spawn(const char *rootdir, char *errbuf, size_t errsz)
{
 e_bsp_session *b;
 char file[PATH_MAX];
 char **argv;

 file[0] = '\0';
 argv = bsp_read_connection_argv(rootdir, file, sizeof file);
 if (!argv)
 {
  if (errbuf)
   snprintf(errbuf, errsz, "no .bsp connection file in %s", rootdir);
  return NULL;
 }
 if (!argv[0] || (bsp_approver && bsp_approver(rootdir, file, argv) != 0))
 {
  bsp_free_argv(argv);
  if (errbuf)
   snprintf(errbuf, errsz, "%s", E_BSP_NOT_APPROVED);
  return NULL;
 }
 b = calloc(1, sizeof(*b));
 if (!b)
 {  bsp_free_argv(argv);  return NULL;  }
 e_dap_reader_init(&b->rd);
 b->in_fd = b->out_fd = -1;
 b->out_id = BSP_NO_ID;
 snprintf(b->rootdir, sizeof b->rootdir, "%s", rootdir);
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

e_bsp_session *e_bsp_start_async(const char *workdir, const E_D_REQ_IO *io,
                                 e_bsp_started_fn done, void *ctx,
                                 char *errbuf, size_t errsz)
{
 e_bsp_session *b;
 E_D_REQUESTER via;
 char rootdir[PATH_MAX];

 if (errbuf && errsz)
  errbuf[0] = '\0';
 if (!realpath(workdir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", workdir);
 b = bsp_session_spawn(rootdir, errbuf, errsz);
 if (!b)
  return NULL;
 b->rio = io;
 b->starting = 1;
 via.request = bsp_req_request;
 via.engine = b;
 e_d_chain_run(&via, bsp_start_steps, b, bsp_start_finished);
 if (!b->starting)
 {
  /* The first request could not even go out: the caller hears it here,
     not through a callback from inside this call. */
  if (errbuf)
   snprintf(errbuf, errsz, "%s", b->err);
  e_bsp_close(b);
  return NULL;
 }
 b->done = done;
 b->done_ctx = ctx;
 return b;
}

int e_bsp_fd(const e_bsp_session *b)
{
 return b ? b->out_fd : -1;
}

int e_bsp_starting(const e_bsp_session *b)
{
 return b && b->starting;
}

const char *e_bsp_request_waited(const e_bsp_session *b, long *secs)
{
 if (!b || b->out_id == BSP_NO_ID)
  return NULL;
 if (secs)
  *secs = (bsp_now(b) - b->sent_ms) / E_D_REQ_SECOND_MS;
 return b->out_method;
}

const char *e_bsp_endpoint(const e_bsp_session *b, int *port)
{
 if (!b || !b->host[0])
  return NULL;
 if (port)
  *port = b->port;
 return b->host;
}

const char *e_bsp_main_class(const e_bsp_session *b)
{
 return b ? b->mainclass : NULL;
}

const char *e_bsp_error(const e_bsp_session *b)
{
 return b ? b->err : NULL;
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
 * `disconnect`, before this.  A start still running is given up, and its
 * owner is not told.
 */
void e_bsp_close(e_bsp_session *b)
{
 if (!b)
  return;
 b->closing = 1;
 bsp_out_end(b, E_D_REQ_CANCELLED);
 bsp_stage_clear(b);
 bsp_disarm_all(b);
 if (b->target)
  json_object_put(b->target);
 if (b->in_fd >= 0)
  wpe_fd_close(b->in_fd);          /* what still waits for it goes too */
 if (b->out_fd >= 0)
  close(b->out_fd);
 wpe_child_end_detached(&b->launcher, SIGTERM);
 e_dap_reader_free(&b->rd);
 free(b);
}
