/* test_lsp_requests.c -- the language server's requests, answered on the loop.
 *
 * No real server needed: this program stands in for one.  Run with FAKE_FLAG
 * it answers `initialize` and then, as its mode says:
 *
 *   reorder  holds the first two requests, sends a diagnostics batch and a
 *            workspace/applyEdit request of its own (and notes the editor's
 *            reply), then answers the second request before the first;
 *   late     answers each request only once the next one has come, so an
 *            answer arrives after the editor gave its request up;
 *   silent   never answers anything after `initialize`;
 *   die      exits at the first request after `initialize`;
 *   together holds the first two requests and answers the second, then the
 *            first, in one write;
 *   holder   as silent, and leaves a process of its own session holding
 *            its output open after it ends;
 *   command  answers a code action request with a command-based action,
 *            and runs that command as a server does: it sends its edit
 *            back with workspace/applyEdit, answers the command once the
 *            editor replied, and then sends one more applyEdit.
 *
 * It notes what it reads in a log file next to the test, so every check is
 * on what the server saw.  What is checked, as the user would miss it: each
 * answer reaches the request that asked, whatever order they come in; the
 * server's own request is answered while requests are out; Esc tells each
 * owner once and drops the late answer; the deadline gives the request up
 * with a line that names it, and the start has none; a server that ends
 * tells every request it is gone; two answers read together reach their
 * own requests; a newer overlay fetch replaces the older one, whose answer
 * is never handed over, and Esc leaves the overlay fetches alone; a
 * command's edit is applied to the text it was run on while the command is
 * out, and declined after; the output of a closed session is let go even
 * when something it left behind holds it open; and the close asks on the
 * pipe and returns at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "../we_lsp.h"
#include "../we_child.h"
#include "../we_dap_proto.h"
#include "../we_fdloop.h"

/* A literal ASCII column of the fixtures: units are bytes in every encoding. */
#define COL(n)           e_lsp_units_make(n)

#define FAKE_FLAG        "--fake-lsp"
#define FAKE_REORDER     "reorder"
#define FAKE_LATE        "late"
#define FAKE_SILENT      "silent"
#define FAKE_DIE         "die"
#define FAKE_TOGETHER    "together"
#define FAKE_HOLDER      "holder"
#define FAKE_COMMAND     "command"
#define FAKE_LATE_APPLY_ID 902       /* the applyEdit after the command ended */
#define FAKE_HINT        "HINT"      /* the label of the canned inlay hint     */
#define FAKE_CMD_EDIT    "X"         /* what the command inserts at 0:0        */
#define FAKE_HOLDER_LIFETIME_S 15
/* The owner's whole ladder after the close, and slack: by then the closed
   session's output must no longer be read. */
#define DRAIN_ENDS_MS    (WPE_CHILD_END_GRACE_MS + WPE_CHILD_END_KILL_MS + 2500)
#define FAKE_LIFETIME_S  30          /* a stand-in left over ends by itself */
#define FAKE_READ_CHUNK  4096
#define FAKE_FRAME_MAX   2048
#define FAKE_APPLY_ID    900         /* the id of the server's own request  */
#define FAKE_LOG         "fake.log"
#define FAKE_HOVER_TEXT  "HOVER-ANSWER"
#define FAKE_DEF_PATH    "/answer/of/the/definition.c"
#define FAKE_DEF_LINE    41
#define LOOP_SLICE_MS    20
#define ANSWER_WAIT_MS   5000
#define CLOSE_RETURNS_MS 100         /* e_lsp_close must not wait           */
#define FAKE_DEADLINE_S  "5"         /* the deadline the timer test sets    */
#define DEADLINE_MS      5000
#define PATH_TEXT_MAX    (PATH_MAX + 64)
#define MS_PER_SEC       1000L
#define NS_PER_MS        1000000L
#define TOLD_MAX         8

static char self_path[PATH_MAX];
static char work_dir[PATH_MAX];
static int failures = 0;

/* ---- the fake server ---------------------------------------------------- */

static char fake_log_path[PATH_TEXT_MAX];

/** fake_note - Append one line to the fake's log. */
static void fake_note(const char *what, const char *detail)
{
 FILE *f = fopen(fake_log_path, "a");

 if (!f)
  return;
 fprintf(f, "%s %s\n", what, detail ? detail : "-");
 fclose(f);
}

/** fake_send - Write one framed message @body. */
static void fake_send(const char *body)
{
 char frame[FAKE_FRAME_MAX];
 int n = snprintf(frame, sizeof frame, "Content-Length: %zu\r\n\r\n%s",
                  strlen(body), body);

 if (write(STDOUT_FILENO, frame, (size_t)n) != n)
  _exit(2);
}

/* What the fake holds back. */
typedef struct {
 int id;
 char method[64];
} fake_req;

/** fake_answer_body - The response to request @id of @method, in @body. */
static void fake_answer_body(int id, const char *method, char *body, size_t n)
{
 const char *result = "null";
 char loc[FAKE_FRAME_MAX / 4];

 if (!strcmp(method, "initialize"))
  result = "{\"capabilities\":{}}";
 else if (!strcmp(method, "textDocument/hover"))
  result = "{\"contents\":{\"kind\":\"plaintext\",\"value\":\"" FAKE_HOVER_TEXT "\"}}";
 else if (!strcmp(method, "textDocument/definition"))
 {
  snprintf(loc, sizeof loc, "[{\"uri\":\"file://%s\",\"range\":{\"start\":"
           "{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,"
           "\"character\":1}}}]", FAKE_DEF_PATH, FAKE_DEF_LINE, FAKE_DEF_LINE);
  result = loc;
 }
 else if (!strcmp(method, "textDocument/inlayHint"))
  result = "[{\"position\":{\"line\":0,\"character\":1},\"label\":\""
           FAKE_HINT "\"}]";
 else if (!strcmp(method, "textDocument/codeAction"))
  result = "[{\"title\":\"Fix\",\"command\":{\"title\":\"Fix\","
           "\"command\":\"stand-in.fix\",\"arguments\":[]}}]";
 snprintf(body, n, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":%s}", id, result);
}

/** fake_answer - Answer request @id of @method with its canned result. */
static void fake_answer(int id, const char *method)
{
 char body[FAKE_FRAME_MAX / 2];

 fake_answer_body(id, method, body, sizeof body);
 fake_send(body);
}

/**
 * fake_answer_two - Answer @second, then @first, in one write, so the
 * editor reads both at once.
 */
static void fake_answer_two(const fake_req *second, const fake_req *first)
{
 char a[FAKE_FRAME_MAX / 2], b[FAKE_FRAME_MAX / 2];
 char frames[FAKE_FRAME_MAX * 2];
 int n;

 fake_answer_body(second->id, second->method, a, sizeof a);
 fake_answer_body(first->id, first->method, b, sizeof b);
 n = snprintf(frames, sizeof frames,
              "Content-Length: %zu\r\n\r\n%sContent-Length: %zu\r\n\r\n%s",
              strlen(a), a, strlen(b), b);
 if (write(STDOUT_FILENO, frames, (size_t)n) != n)
  _exit(2);
}

/** fake_side_traffic - What a busy server sends while requests are out. */
static void fake_side_traffic(void)
{
 char body[FAKE_FRAME_MAX / 2];

 fake_send("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\","
           "\"params\":{\"uri\":\"file:///side/traffic.c\",\"diagnostics\":"
           "[{\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":"
           "{\"line\":1,\"character\":2}},\"severity\":1,\"message\":\"x\"}]}}");
 snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":"
          "\"workspace/applyEdit\",\"params\":{\"edit\":{\"changes\":{}}}}",
          FAKE_APPLY_ID);
 fake_send(body);
}

/* The command mode's state: the file the code action was asked for, and
   the command waiting for the editor's reply to its edit. */
static char fake_cmd_uri[FAKE_FRAME_MAX / 4];
static int fake_cmd_id = -1;

/**
 * fake_send_apply_edit - Ask the editor to insert FAKE_CMD_EDIT at the
 * start of the code action's file, as request @id.
 */
static void fake_send_apply_edit(int id)
{
 char body[FAKE_FRAME_MAX / 2];

 snprintf(body, sizeof body, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":"
          "\"workspace/applyEdit\",\"params\":{\"edit\":{\"changes\":{\"%s\":"
          "[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":"
          "{\"line\":0,\"character\":0}},\"newText\":\"" FAKE_CMD_EDIT "\"}]}}}}",
          id, fake_cmd_uri);
 fake_send(body);
}

/**
 * fake_command_replied - The editor replied to the command's edit: the
 * command is answered, and one more edit is sent after it, which the
 * editor must decline.
 */
static void fake_command_replied(void)
{
 if (fake_cmd_id < 0)
  return;
 fake_answer(fake_cmd_id, "workspace/executeCommand");
 fake_cmd_id = -1;
 fake_send_apply_edit(FAKE_LATE_APPLY_ID);
}

/**
 * fake_command_take - The command mode's requests: the code action is
 * answered and its file noted; the command sends its edit back first.
 */
static void fake_command_take(int id, const char *method, struct json_object *m)
{
 struct json_object *params = NULL, *doc = NULL, *uri = NULL;

 if (!strcmp(method, "workspace/executeCommand"))
 {
  fake_cmd_id = id;
  fake_send_apply_edit(FAKE_APPLY_ID);
  return;
 }
 if (!strcmp(method, "textDocument/codeAction") &&
     json_object_object_get_ex(m, "params", &params) &&
     json_object_object_get_ex(params, "textDocument", &doc) &&
     json_object_object_get_ex(doc, "uri", &uri))
  snprintf(fake_cmd_uri, sizeof fake_cmd_uri, "%s", json_object_get_string(uri));
 fake_answer(id, method);
}

/**
 * fake_take - One message read: note it, and answer, hold or drop it as
 * the mode says.
 * @mode: the fake's mode; @m: the message; @held, @nheld: what is held.
 * Return: 1 to go on, 0 to end the fake now.
 */
static int fake_take(const char *mode, struct json_object *m, fake_req *held,
                     int *nheld)
{
 struct json_object *jid = NULL, *jm = NULL, *jres = NULL;
 const char *method;
 int id;

 json_object_object_get_ex(m, "method", &jm);
 if (!json_object_object_get_ex(m, "id", &jid))
 {
  fake_note("NOTE", jm ? json_object_get_string(jm) : "-");
  return 1;
 }
 id = json_object_get_int(jid);
 if (!jm)
 {
  /* the editor's reply to the server's own request */
  json_object_object_get_ex(m, "result", &jres);
  fake_note("REPLY", jres ? json_object_to_json_string_ext(jres,
                                                  JSON_C_TO_STRING_PLAIN)
                          : "null");
  if (!strcmp(mode, FAKE_COMMAND))
   fake_command_replied();
  return 1;
 }
 method = json_object_get_string(jm);
 fake_note("REQ", method);
 if (!strcmp(method, "initialize"))
 {
  fake_answer(id, method);
  return 1;
 }
 if (!strcmp(mode, FAKE_DIE))
  return 0;
 if (!strcmp(mode, FAKE_COMMAND))
 {
  fake_command_take(id, method, m);
  return 1;
 }
 if (!strcmp(mode, FAKE_SILENT) || !strcmp(mode, FAKE_HOLDER) ||
     !strcmp(method, "shutdown"))
  return 1;
 if (!strcmp(mode, FAKE_LATE))
 {
  if (*nheld)
   fake_answer(held[0].id, held[0].method);
  held[0].id = id;
  snprintf(held[0].method, sizeof held[0].method, "%s", method);
  *nheld = 1;
  return 1;
 }
 held[*nheld].id = id;
 snprintf(held[*nheld].method, sizeof held[*nheld].method, "%s", method);
 (*nheld)++;
 if (*nheld < 2)
  return 1;
 if (!strcmp(mode, FAKE_TOGETHER))
 {
  fake_answer_two(&held[1], &held[0]);
  *nheld = 0;
  return 1;
 }
 fake_side_traffic();
 fake_answer(held[1].id, held[1].method);
 fake_answer(held[0].id, held[0].method);
 *nheld = 0;
 return 1;
}

/**
 * fake_leave_a_holder - Start a process of a session of its own that keeps
 * the server's output open after the server has ended, out of the owner's
 * reach, as a badly behaved daemon would.  It notes its PID first and ends
 * by itself after FAKE_HOLDER_LIFETIME_S.
 */
static void fake_leave_a_holder(void)
{
 char pid[32];
 pid_t child = fork();

 if (child != 0)
  return;
 setsid();
 close(STDIN_FILENO);
 snprintf(pid, sizeof pid, "%ld", (long)getpid());
 fake_note("HOLDER", pid);
 alarm(FAKE_HOLDER_LIFETIME_S);
 for (;;)
  pause();
}

/** fake_server - The stand-in language server. */
static int fake_server(const char *mode, const char *dir)
{
 e_dap_reader rd;
 fake_req held[2];
 int nheld = 0;
 char buf[FAKE_READ_CHUNK];
 ssize_t n;

 alarm(FAKE_LIFETIME_S);
 snprintf(fake_log_path, sizeof fake_log_path, "%s/%s", dir, FAKE_LOG);
 if (!strcmp(mode, FAKE_HOLDER))
  fake_leave_a_holder();
 e_dap_reader_init(&rd);
 while ((n = read(STDIN_FILENO, buf, sizeof buf)) != 0)
 {
  struct json_object *m;
  int err = 0;

  if (n < 0 && errno == EINTR)
   continue;
  if (n < 0)
   break;
  e_dap_reader_push(&rd, buf, (size_t)n);
  while ((m = e_dap_reader_pop(&rd, &err)) != NULL)
  {
   int go_on = fake_take(mode, m, held, &nheld);

   json_object_put(m);
   if (!go_on)
    return 0;
  }
 }
 fake_note("EOF", NULL);
 return 0;
}

/* ---- the test side ------------------------------------------------------ */

/** now_ms - A monotonic instant in milliseconds. */
static long now_ms(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * MS_PER_SEC + ts.tv_nsec / NS_PER_MS;
}

/** check - Print one result and count a failure. */
static void check(int ok, const char *what)
{
 if (ok)
 {
  printf("ok: %s\n", what);
  return;
 }
 printf("FAIL: %s\n", what);
 failures++;
}

/** on_child - Reap as the editor's SIGCHLD handler does. */
static void on_child(int sig)
{
 int status;
 pid_t pid;

 (void)sig;
 while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
  wpe_child_note_ended(pid, status);
}

/** install_reaper - Reap children as the editor does. */
static void install_reaper(void)
{
 struct sigaction act;

 memset(&act, 0, sizeof act);
 act.sa_handler = on_child;
 sigemptyset(&act.sa_mask);
 act.sa_flags = SA_RESTART;
 sigaction(SIGCHLD, &act, NULL);
}

/** fake_log - The fake's log so far ("" when none). */
static void fake_log(char *out, size_t n)
{
 char path[PATH_TEXT_MAX];
 FILE *f;
 size_t got;

 out[0] = '\0';
 snprintf(path, sizeof path, "%s/%s", work_dir, FAKE_LOG);
 f = fopen(path, "r");
 if (!f)
  return;
 got = fread(out, 1, n - 1, f);
 out[got] = '\0';
 fclose(f);
}

/**
 * fake_log_wait_for - Run the loop until the fake's log holds @what (the
 * fake writes it once it has read the editor's bytes), or a bound passes.
 */
static void fake_log_wait_for(const char *what, char *log, size_t n)
{
 long deadline = now_ms() + ANSWER_WAIT_MS;

 fake_log(log, n);
 while (!strstr(log, what) && now_ms() < deadline)
 {
  wpe_fd_poll(LOOP_SLICE_MS);
  fake_log(log, n);
 }
}

/** fake_log_clear - Start each case with an empty log. */
static void fake_log_clear(void)
{
 char path[PATH_TEXT_MAX];

 snprintf(path, sizeof path, "%s/%s", work_dir, FAKE_LOG);
 unlink(path);
}

/* What the continuations were told. */
typedef struct {
 int n;                       /* how many times */
 E_D_REQ_END how[TOLD_MAX];
 char text[256];              /* hover: the text; jump: the path */
 int line;
} told;

static int diagnostics_seen = 0;

/** on_diag - The host's diagnostic hook: count what arrives. */
static void on_diag(const char *path, int line, e_lsp_units ch, int el,
                    e_lsp_units ec, int sev, const char *msg, void *ud)
{
 (void)path; (void)line; (void)ch; (void)el; (void)ec; (void)sev;
 (void)msg; (void)ud;
 diagnostics_seen++;
}

/** told_text - A hover's continuation. */
static void told_text(E_D_REQ_END how, char *text, void *ctx)
{
 told *t = ctx;

 if (t->n < TOLD_MAX)
  t->how[t->n] = how;
 t->n++;
 snprintf(t->text, sizeof t->text, "%s", text ? text : "");
 free(text);
}

/** told_location - A definition's continuation. */
static void told_location(E_D_REQ_END how, int found, const char *path,
                          int line, e_lsp_units character, void *ctx)
{
 told *t = ctx;

 (void)character;
 if (t->n < TOLD_MAX)
  t->how[t->n] = how;
 t->n++;
 snprintf(t->text, sizeof t->text, "%s", found ? path : "");
 t->line = line;
}

/** on_readable - The loop's callback for the server's output. */
static void on_readable(int fd, void *data)
{
 (void)fd;
 e_lsp_poll(data);
}

/**
 * loop_until - Run the editor's loop until *@flag is at least @want, the
 * server's output ended, or @ms passed.
 */
static void loop_until(e_lsp_session *s, const int *flag, int want, long ms)
{
 long deadline = now_ms() + ms;
 int fd = e_lsp_fd(s);

 if (wpe_fd_add(fd, POLLIN, on_readable, s) != 0)
  return;
 while (*flag < want && !e_lsp_dead(s) && now_ms() < deadline)
  wpe_fd_poll(LOOP_SLICE_MS);
 wpe_fd_del(fd);
}

/** open_fake - A started session on the fake in @mode, or NULL. */
static e_lsp_session *open_fake(const char *mode)
{
 char *argv[5];
 e_lsp_host host;

 memset(&host, 0, sizeof host);
 host.on_diagnostic = on_diag;
 argv[0] = self_path;
 argv[1] = FAKE_FLAG;
 argv[2] = (char *)mode;
 argv[3] = work_dir;
 argv[4] = NULL;
 fake_log_clear();
 diagnostics_seen = 0;
 return e_lsp_open(argv, work_dir, "c", &host);
}

/* A file the requests name (the fake never reads it). */
#define ASKED_FILE "asked.c"

static void test_answers_reach_their_own_requests_in_any_order(void)
{
 e_lsp_session *s = open_fake(FAKE_REORDER);
 told hover, def;
 char log[4096];

 memset(&hover, 0, sizeof hover);
 memset(&def, 0, sizeof def);
 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 check(e_lsp_hover_async(s, ASKED_FILE, 1, COL(1), told_text, &hover) == E_D_SEND_OK,
       "the hover goes out");
 check(e_lsp_locate_async(s, E_LSP_LOCATE_DEFINITION, ASKED_FILE, 1, COL(1),
                          told_location, &def) == E_D_SEND_OK,
       "the definition goes out while the hover is still out");
 check(e_lsp_requests_pending(s), "both are waited for");
 loop_until(s, &hover.n, 1, ANSWER_WAIT_MS);
 check(def.n == 1 && def.how[0] == E_D_REQ_DONE &&
       !strcmp(def.text, FAKE_DEF_PATH) && def.line == FAKE_DEF_LINE,
       "the definition, answered first, gets its own location");
 check(hover.n == 1 && hover.how[0] == E_D_REQ_DONE &&
       !strcmp(hover.text, FAKE_HOVER_TEXT),
       "the hover, answered second, gets its own text");
 check(!e_lsp_requests_pending(s), "nothing is waited for afterwards");
 check(diagnostics_seen > 0,
       "diagnostics sent while requests were out reach the host");
 fake_log_wait_for("REPLY ", log, sizeof log);
 check(strstr(log, "REPLY {\"applied\":false}") != NULL,
       "the server's workspace/applyEdit, asked while requests were out, is "
       "answered");
 e_lsp_close(s);
}

static void test_cancel_tells_each_owner_once_and_drops_the_late_answer(void)
{
 e_lsp_session *s = open_fake(FAKE_LATE);
 told hover, def, again;

 memset(&hover, 0, sizeof hover);
 memset(&def, 0, sizeof def);
 memset(&again, 0, sizeof again);
 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 e_lsp_hover_async(s, ASKED_FILE, 1, COL(1), told_text, &hover);
 e_lsp_requests_cancel(s, E_D_REQ_CANCELLED);
 check(hover.n == 1 && hover.how[0] == E_D_REQ_CANCELLED,
       "Esc tells the hover it was cancelled, once");
 check(!e_lsp_requests_pending(s), "nothing is waited for after Esc");
 /* The next request makes the stand-in answer the cancelled hover now. */
 e_lsp_locate_async(s, E_LSP_LOCATE_DEFINITION, ASKED_FILE, 1, COL(1),
                    told_location, &def);
 e_lsp_hover_async(s, ASKED_FILE, 2, COL(2), told_text, &again);
 loop_until(s, &def.n, 1, ANSWER_WAIT_MS);
 check(hover.n == 1, "the cancelled hover's late answer is dropped");
 check(def.n == 1 && def.how[0] == E_D_REQ_DONE &&
       !strcmp(def.text, FAKE_DEF_PATH),
       "the next request gets its own answer, not the late one");
 e_lsp_close(s);
 check(again.n == 1 && again.how[0] == E_D_REQ_GONE,
       "a request still out at the close is told the server is gone");
}

/* The timers and lines the deadline test hands the engine. */
static struct {
 long armed[E_D_TIMERS];
 int waiting;
 int notices;
 char notice[E_D_REQ_MSG_MAX];
 char waited_for[64];
} world;

static long world_now(void *io)                 { (void)io; return 0; }
static void world_arm(int which, long ms, void *io)
{
 (void)io;
 world.armed[which] = ms;
}
static void world_waiting(const char *cmd, long secs, void *io)
{
 (void)secs; (void)io;
 world.waiting++;
 snprintf(world.waited_for, sizeof world.waited_for, "%s", cmd);
}
static void world_notice(const char *msg, void *io)
{
 (void)io;
 world.notices++;
 snprintf(world.notice, sizeof world.notice, "%s", msg);
}

static void test_deadline_gives_the_request_up_and_the_start_has_none(void)
{
 char *argv[5];
 e_lsp_session *s;
 E_D_REQ_IO io;
 told hover;
 int i;

 memset(&world, 0, sizeof world);
 memset(&io, 0, sizeof io);
 memset(&hover, 0, sizeof hover);
 io.now_ms = world_now;
 io.arm_timer = world_arm;
 io.waiting = world_waiting;
 io.notice = world_notice;
 io.name = "stand-in";
 argv[0] = self_path;
 argv[1] = FAKE_FLAG;
 argv[2] = FAKE_SILENT;
 argv[3] = work_dir;
 argv[4] = NULL;
 fake_log_clear();
 s = e_lsp_open_async(argv, work_dir, "c", NULL);
 check(s != NULL, "the stand-in starts");
 if (!s)
  return;
 for (i = 0; i < E_D_TIMERS; i++)
  world.armed[i] = E_D_TIMER_OFF;
 e_lsp_requests_init(s, &io, FAKE_DEADLINE_S);
 check(world.armed[E_D_TIMER_QUIET] == E_D_REQ_QUIET_MS,
       "the start gets its waiting line after a quiet second");
 check(world.armed[E_D_TIMER_DEADLINE] == E_D_TIMER_OFF,
       "the start has no deadline (a server may import a build for minutes)");
 e_lsp_request_timer(s, E_D_TIMER_QUIET);
 check(world.waiting == 1 && !strcmp(world.waited_for, "initialize"),
       "the waiting line names the start");
 e_lsp_request_timer(s, E_D_TIMER_DEADLINE);
 check(e_lsp_requests_pending(s), "a deadline timer does not end the start");
 e_lsp_close(s);

 /* A started session: a request gets the deadline the user set. */
 s = open_fake(FAKE_SILENT);
 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 for (i = 0; i < E_D_TIMERS; i++)
  world.armed[i] = E_D_TIMER_OFF;
 e_lsp_requests_init(s, &io, FAKE_DEADLINE_S);
 e_lsp_hover_async(s, ASKED_FILE, 1, COL(1), told_text, &hover);
 check(world.armed[E_D_TIMER_DEADLINE] == DEADLINE_MS,
       "the request's deadline is the one the user set");
 e_lsp_request_timer(s, E_D_TIMER_DEADLINE);
 check(hover.n == 1 && hover.how[0] == E_D_REQ_TIMED_OUT,
       "at the deadline the request is given up");
 check(world.notices == 1 &&
       strstr(world.notice, "did not answer `textDocument/hover` within 5 s"),
       "the status row names the request and the deadline");
 check(world.armed[E_D_TIMER_DEADLINE] == E_D_TIMER_OFF,
       "with nothing out every timer is taken back");
 e_lsp_close(s);
}

static void test_a_server_that_ends_tells_every_request_it_is_gone(void)
{
 e_lsp_session *s = open_fake(FAKE_DIE);
 told hover, def;

 memset(&hover, 0, sizeof hover);
 memset(&def, 0, sizeof def);
 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 e_lsp_hover_async(s, ASKED_FILE, 1, COL(1), told_text, &hover);
 e_lsp_locate_async(s, E_LSP_LOCATE_DEFINITION, ASKED_FILE, 1, COL(1),
                    told_location, &def);
 loop_until(s, &def.n, 1, ANSWER_WAIT_MS);
 check(hover.n == 1 && hover.how[0] == E_D_REQ_GONE,
       "the hover is told the server is gone");
 check(def.n == 1 && def.how[0] == E_D_REQ_GONE,
       "so is the definition");
 e_lsp_close(s);
}

/* How long the loop runs after a waited request, with nothing more coming. */
#define QUIET_LOOP_MS 500

/*
 * Two answers in one read (signature help's and the hover's, written
 * together): each reaches its own continuation, and neither sits read and
 * unhandled until its deadline because the server has nothing more to
 * write.
 */
static void test_an_answer_read_by_a_waited_request_reaches_its_owner(void)
{
 e_lsp_session *s = open_fake(FAKE_TOGETHER);
 told hover;
 char *sig;
 char log[4096];

 memset(&hover, 0, sizeof hover);
 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 check(e_lsp_hover_async(s, ASKED_FILE, 1, COL(1), told_text, &hover) == E_D_SEND_OK,
       "the hover goes out");
 sig = e_lsp_signature_help(s, ASKED_FILE, 1, COL(1));
 free(sig);
 loop_until(s, &hover.n, 1, QUIET_LOOP_MS);
 check(hover.n == 1 && hover.how[0] == E_D_REQ_DONE &&
       !strcmp(hover.text, FAKE_HOVER_TEXT),
       "the hover's answer, read together with signature help's, reaches "
       "the hover");
 check(!e_lsp_requests_pending(s), "nothing is waited for afterwards");
 e_lsp_close(s);
 /* Its last line lands in the log before the next case clears it. */
 fake_log_wait_for("EOF", log, sizeof log);
}

/* What an overlay fetch's continuation was told. */
typedef struct {
 int n;                       /* how many times */
 E_D_REQ_END how[TOLD_MAX];
 int count;                   /* the hints handed over, -1 for none */
 char label[64];              /* the first hint's label */
} told_hints;

/** told_inlays - An inlay hint fetch's continuation. */
static void told_inlays(E_D_REQ_END how, const e_lsp_inlay_hint *hints, int n,
                        void *ctx)
{
 told_hints *t = ctx;

 if (t->n < TOLD_MAX)
  t->how[t->n] = how;
 t->n++;
 t->count = n;
 if (n > 0)
  snprintf(t->label, sizeof t->label, "%s", hints[0].label);
}

/* The lines an inlay hint fetch asks for. */
#define HINT_LINES 10
#define HINTS_MAX  8

/*
 * The overlay fetches the editor sends while the user types: a newer one
 * replaces the older, whose answer (for older text) is never handed over;
 * nobody waits for them, so Esc leaves them alone.
 */
static void test_a_newer_overlay_fetch_replaces_the_older(void)
{
 e_lsp_session *s = open_fake(FAKE_LATE);
 told_hints first, second;
 told hover;

 memset(&first, 0, sizeof first);
 memset(&second, 0, sizeof second);
 memset(&hover, 0, sizeof hover);
 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 check(e_lsp_inlay_hints_async(s, ASKED_FILE, 0, HINT_LINES, HINTS_MAX,
                               told_inlays, &first) == E_D_SEND_OK,
       "the first fetch goes out");
 check(e_lsp_requests_owed(s) && !e_lsp_requests_pending(s),
       "a fetch is owed its answer, but nobody waits for it");
 e_lsp_requests_give_up(s, E_D_REQ_CANCELLED);
 check(first.n == 0, "Esc leaves the fetch alone");
 check(e_lsp_inlay_hints_async(s, ASKED_FILE, 0, HINT_LINES, HINTS_MAX,
                               told_inlays, &second) == E_D_SEND_OK,
       "a newer fetch goes out");
 check(first.n == 1 && first.how[0] == E_D_REQ_CANCELLED && first.count == -1,
       "the older fetch is told at once that it was replaced");
 /* The stand-in answers the older fetch now, and the newer one when the
    next request comes. */
 e_lsp_hover_async(s, ASKED_FILE, 1, COL(1), told_text, &hover);
 loop_until(s, &second.n, 1, ANSWER_WAIT_MS);
 check(first.n == 1 && first.how[0] == E_D_REQ_CANCELLED && first.count == -1,
       "the older fetch's late answer is never handed over");
 check(second.n == 1 && second.how[0] == E_D_REQ_DONE && second.count == 1 &&
       !strcmp(second.label, FAKE_HINT),
       "the newer fetch gets its own answer");
 e_lsp_close(s);
}

/**
 * loop_until_logged - Run the loop until the fake's log holds @what, so
 * a message the server sends unasked is read and answered.
 */
static void loop_until_logged(e_lsp_session *s, const char *what, char *log,
                              size_t n)
{
 int fd = e_lsp_fd(s);

 if (wpe_fd_add(fd, POLLIN, on_readable, s) != 0)
  return;
 fake_log_wait_for(what, log, n);
 wpe_fd_del(fd);
}

/* The text a code action is run on, and what the stand-in's edit makes of
   it. */
#define ACTION_TEXT  "abc\n"
#define ACTION_AFTER FAKE_CMD_EDIT ACTION_TEXT
#define ACTIONS_MAX  4

/*
 * A command-based code action: the server runs the command and sends its
 * change back with workspace/applyEdit while the command is out; that
 * change is applied to the text the action was run on and handed back
 * when the command answers.  An applyEdit after the command ended is
 * declined: no request holds a copy of a file for it.
 */
static void test_a_command_edit_reaches_the_text_it_was_run_on(void)
{
 e_lsp_session *s = open_fake(FAKE_COMMAND);
 e_lsp_code_action acts[ACTIONS_MAX];
 char log[4096];
 char *text;
 int n, others = -1;

 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 n = e_lsp_code_actions(s, ASKED_FILE, 0, COL(0), acts, ACTIONS_MAX);
 check(n == 1 && !acts[0].has_edit, "a command-based action is offered");
 text = e_lsp_apply_code_action(s, 0, ASKED_FILE, ACTION_TEXT, &others);
 check(text && !strcmp(text, ACTION_AFTER) && others == 0,
       "the edit sent back while the command was out is applied to the text "
       "the action was run on");
 free(text);
 loop_until_logged(s, "REPLY {\"applied\":false}", log, sizeof log);
 check(strstr(log, "REPLY {\"applied\":true}") != NULL,
       "the edit is accepted while the command is out");
 check(strstr(log, "REPLY {\"applied\":false}") != NULL,
       "an edit sent after the command ended is declined");
 e_lsp_close(s);
 fake_log_wait_for("EOF", log, sizeof log);
}

/** holder_pid - The PID the holder noted in @log, or 0. */
static pid_t holder_pid(const char *log)
{
 const char *at = strstr(log, "HOLDER ");

 if (!at)
  return 0;
 return (pid_t)atol(at + strlen("HOLDER "));
}

/*
 * A server that leaves its output open behind it (a daemon of its own
 * session holds it): the closed session's output is still let go once the
 * owner's ladder has run, so the loop does not keep a descriptor for ever.
 */
static void test_a_closed_output_held_open_is_let_go(void)
{
 e_lsp_session *s = open_fake(FAKE_HOLDER);
 char log[4096];
 long deadline;
 pid_t holder;
 int fd;

 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 fake_log_wait_for("HOLDER ", log, sizeof log);
 holder = holder_pid(log);
 printf("  holder pid %ld\n", (long)holder);
 fd = e_lsp_fd(s);
 e_lsp_close(s);
 deadline = now_ms() + DRAIN_ENDS_MS;
 while (now_ms() < deadline && fcntl(fd, F_GETFD) != -1)
  wpe_fd_poll(LOOP_SLICE_MS);
 check(fcntl(fd, F_GETFD) == -1 && errno == EBADF,
       "the output a process left behind holds open is closed after the "
       "owner's ladder");
 if (holder > 0)
  kill(holder, SIGKILL);
}

static void test_close_asks_on_the_pipe_and_returns_at_once(void)
{
 e_lsp_session *s = open_fake(FAKE_SILENT);
 long t0, took;
 char log[4096];

 check(s != NULL, "the session starts on the stand-in");
 if (!s)
  return;
 t0 = now_ms();
 e_lsp_close(s);
 took = now_ms() - t0;
 printf("  close took %ld ms\n", took);
 check(took < CLOSE_RETURNS_MS,
       "the close does not wait for a server that never answers `shutdown`");
 fake_log_wait_for("EOF", log, sizeof log);
 check(strstr(log, "REQ shutdown") && strstr(log, "NOTE exit") &&
       strstr(log, "EOF"),
       "the server is asked to end on its pipe: shutdown, exit, then its "
       "input closes");
}

/* @scenario SC-debug-94 */
/* @scenario SC-debug-101 */
int main(int argc, char **argv)
{
 const char *tmp;

 if (argc >= 4 && !strcmp(argv[1], FAKE_FLAG))
  return fake_server(argv[2], argv[3]);
 if (!realpath(argv[0], self_path))
  snprintf(self_path, sizeof self_path, "%s", argv[0]);
 tmp = getenv("TMPDIR");
 snprintf(work_dir, sizeof work_dir, "%s/test_lsp_requests.XXXXXX",
          tmp && *tmp ? tmp : "/tmp");
 if (!mkdtemp(work_dir))
  return 1;
 signal(SIGPIPE, SIG_IGN);
 install_reaper();
 test_answers_reach_their_own_requests_in_any_order();
 test_cancel_tells_each_owner_once_and_drops_the_late_answer();
 test_deadline_gives_the_request_up_and_the_start_has_none();
 test_a_server_that_ends_tells_every_request_it_is_gone();
 test_an_answer_read_by_a_waited_request_reaches_its_owner();
 test_a_newer_overlay_fetch_replaces_the_older();
 test_a_command_edit_reaches_the_text_it_was_run_on();
 test_a_closed_output_held_open_is_let_go();
 test_close_asks_on_the_pipe_and_returns_at_once();
 fake_log_clear();
 rmdir(work_dir);
 if (failures == 0)
  return 0;
 printf("%d check(s) failed\n", failures);
 return 1;
}
