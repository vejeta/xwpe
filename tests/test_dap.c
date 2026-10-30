/* test_dap.c -- unit tests for the DAP transport core (we_dap_proto.c) and
 * the session start machine (we_dap.c).
 *
 * Offline: no adapter and no xwpe UI.  Exercises the frame builder and the
 * streaming reader (whole frame, split frame, back-to-back frames, and a
 * malformed frame that must resync), then plays an adapter on a loopback
 * socket to drive the start one read at a time. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../we_dap_proto.h"
#include "../we_dap.h"

static int failures = 0;

#define CHECK(cond, msg) do {                                           \
  if (!(cond)) { printf("FAIL: %s\n", msg); failures++; }               \
 } while (0)

/* Wrap a JSON body in a DAP frame into `dst` (must be big enough). */
static size_t frame(char *dst, const char *body)
{
 int h = sprintf(dst, "Content-Length: %zu\r\n\r\n", strlen(body));
 strcpy(dst + h, body);
 return (size_t)h + strlen(body);
}

static const char *str_field(struct json_object *o, const char *k)
{
 struct json_object *v = NULL;
 if (!json_object_object_get_ex(o, k, &v))
  return NULL;
 return json_object_get_string(v);
}

static void test_build(void)
{
 struct json_object *args = json_object_new_object();
 size_t len = 0;
 char *req;
 struct json_object *parsed, *seq, *a, *prog;
 struct json_tokener *tok;
 const char *body;

 json_object_object_add(args, "program", json_object_new_string("/tmp/x"));
 req = e_dap_build_request(7, "launch", args, &len);
 CHECK(req != NULL, "build_request returned NULL");
 if (!req) { json_object_put(args); return; }

 CHECK(strncmp(req, "Content-Length: ", 16) == 0, "request lacks Content-Length header");
 CHECK(strstr(req, "\r\n\r\n") != NULL, "request lacks header/body separator");
 CHECK(len == strlen(req), "out_len does not match string length");

 body = strstr(req, "\r\n\r\n") + 4;
 tok = json_tokener_new();
 parsed = json_tokener_parse_ex(tok, body, (int)strlen(body));
 json_tokener_free(tok);
 CHECK(parsed != NULL, "request body is not valid JSON");
 if (parsed)
 {
  CHECK(strcmp(str_field(parsed, "type"), "request") == 0, "type != request");
  CHECK(strcmp(str_field(parsed, "command"), "launch") == 0, "command != launch");
  CHECK(json_object_object_get_ex(parsed, "seq", &seq) &&
        json_object_get_int(seq) == 7, "seq != 7");
  CHECK(json_object_object_get_ex(parsed, "arguments", &a) &&
        (prog = NULL, json_object_object_get_ex(a, "program", &prog)) &&
        strcmp(json_object_get_string(prog), "/tmp/x") == 0,
        "arguments.program not preserved");
  json_object_put(parsed);
 }
 free(req);
 /* caller still owns args (build must not consume it) */
 CHECK(str_field(args, "program") != NULL, "build_request consumed caller's args");
 json_object_put(args);
}

static void test_whole_frame(void)
{
 e_dap_reader r;
 char buf[256];
 size_t n;
 int err = 99;
 struct json_object *m;

 e_dap_reader_init(&r);
 n = frame(buf, "{\"seq\":1,\"type\":\"event\",\"event\":\"stopped\"}");
 e_dap_reader_push(&r, buf, n);
 m = e_dap_reader_pop(&r, &err);
 CHECK(err == 0, "whole frame reported error");
 CHECK(m != NULL, "whole frame not popped");
 if (m)
 {
  CHECK(strcmp(str_field(m, "event"), "stopped") == 0, "event != stopped");
  json_object_put(m);
 }
 CHECK(e_dap_reader_pop(&r, &err) == NULL, "extra message after single frame");
 e_dap_reader_free(&r);
}

static void test_split_frame(void)
{
 e_dap_reader r;
 char buf[256];
 size_t n, half;
 int err = 0;
 struct json_object *m;

 e_dap_reader_init(&r);
 n = frame(buf, "{\"seq\":2,\"type\":\"response\",\"command\":\"next\"}");
 half = n / 2;
 e_dap_reader_push(&r, buf, half);
 CHECK(e_dap_reader_pop(&r, &err) == NULL, "popped an incomplete frame");
 e_dap_reader_push(&r, buf + half, n - half);
 m = e_dap_reader_pop(&r, &err);
 CHECK(m != NULL, "split frame not reassembled");
 if (m)
 {
  CHECK(strcmp(str_field(m, "command"), "next") == 0, "command != next");
  json_object_put(m);
 }
 e_dap_reader_free(&r);
}

static void test_two_frames(void)
{
 e_dap_reader r;
 char buf[512];
 size_t n;
 int err = 0;
 struct json_object *m1, *m2;

 e_dap_reader_init(&r);
 n = frame(buf, "{\"seq\":1,\"event\":\"output\"}");
 n += frame(buf + n, "{\"seq\":2,\"event\":\"terminated\"}");
 e_dap_reader_push(&r, buf, n);
 m1 = e_dap_reader_pop(&r, &err);
 m2 = e_dap_reader_pop(&r, &err);
 CHECK(m1 && strcmp(str_field(m1, "event"), "output") == 0, "first of two frames wrong");
 CHECK(m2 && strcmp(str_field(m2, "event"), "terminated") == 0, "second of two frames wrong");
 CHECK(e_dap_reader_pop(&r, &err) == NULL, "third message from two frames");
 if (m1) json_object_put(m1);
 if (m2) json_object_put(m2);
 e_dap_reader_free(&r);
}

static void test_malformed_resyncs(void)
{
 e_dap_reader r;
 char buf[256];
 size_t n;
 int err = 0;
 struct json_object *m;

 e_dap_reader_init(&r);
 /* A header block with no Content-Length, then a valid frame after it. */
 e_dap_reader_push(&r, "X-Bad: 1\r\n\r\n", 12);
 m = e_dap_reader_pop(&r, &err);
 CHECK(m == NULL && err == 1, "malformed header not flagged");
 n = frame(buf, "{\"seq\":9,\"event\":\"initialized\"}");
 e_dap_reader_push(&r, buf, n);
 err = 0;
 m = e_dap_reader_pop(&r, &err);
 CHECK(m != NULL && err == 0, "did not resync after malformed header");
 if (m)
 {
  CHECK(strcmp(str_field(m, "event"), "initialized") == 0, "resync event wrong");
  json_object_put(m);
 }
 e_dap_reader_free(&r);
}

/* ---- the start machine (we_dap.c), fed one read at a time ---------------
 *
 * The test plays the adapter on a local socket: the engine connects with
 * e_dap_start_tcp, the test answers each request and calls e_dap_start_pump
 * once per write, as an editor's event loop would.
 */

#define ADAPTER_BUF 4096
#define SPLIT_AT    10          /* bytes of the first half of a split frame */

/* Listen on 127.0.0.1 at a free port. */
static int listen_local(int *port)
{
 struct sockaddr_in a;
 socklen_t al = sizeof(a);
 int fd = socket(AF_INET, SOCK_STREAM, 0);

 memset(&a, 0, sizeof(a));
 a.sin_family = AF_INET;
 a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 if (fd < 0 || bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
     listen(fd, 1) < 0 || getsockname(fd, (struct sockaddr *)&a, &al) < 0)
  return -1;
 *port = ntohs(a.sin_port);
 return fd;
}

/* Read the engine's next request; NULL on end of file. */
static struct json_object *next_request(int fd, e_dap_reader *rd)
{
 char buf[ADAPTER_BUF];
 int err = 0;

 for (;;)
 {
  struct json_object *m = e_dap_reader_pop(rd, &err);
  ssize_t n;

  if (m)
   return m;
  n = read(fd, buf, sizeof buf);
  if (n <= 0)
   return NULL;
  e_dap_reader_push(rd, buf, (size_t)n);
 }
}

/* Read the next request, check its command, and return its seq (or -1). */
static int expect_request(int fd, e_dap_reader *rd, const char *command)
{
 struct json_object *m = next_request(fd, rd), *v = NULL;
 int seq = -1;

 CHECK(m != NULL, "the engine sent no request");
 if (!m)
  return -1;
 CHECK(str_field(m, "command") && !strcmp(str_field(m, "command"), command),
       command);
 if (json_object_object_get_ex(m, "seq", &v))
  seq = json_object_get_int(v);
 json_object_put(m);
 return seq;
}

/* Frame a response to request @seq into @dst; return its length. */
static size_t response(char *dst, int seq, const char *command, int success,
                       const char *message)
{
 char body[512];

 snprintf(body, sizeof body,
          "{\"seq\":1,\"type\":\"response\",\"request_seq\":%d,"
          "\"command\":\"%s\",\"success\":%s,\"message\":\"%s\"}",
          seq, command, success ? "true" : "false", message);
 return frame(dst, body);
}

static void put(int fd, const char *buf, size_t n)
{
 CHECK(write(fd, buf, n) == (ssize_t)n, "short write to the engine");
}

/* Connect a Scala-style session (launch, then wait `initialized`). */
static e_dap_session *start_session(int *conn, int *lfd)
{
 int port = 0;
 e_dap_session *s;

 *lfd = listen_local(&port);
 CHECK(*lfd >= 0, "cannot listen on 127.0.0.1");
 if (*lfd < 0)
  return NULL;
 s = e_dap_start_tcp("127.0.0.1", port, "/tmp/p", NULL, NULL);
 CHECK(s != NULL, "e_dap_start_tcp returned NULL");
 *conn = accept(*lfd, NULL, NULL);
 CHECK(*conn >= 0, "accept failed");
 return s;
}

static void test_start_machine(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_session *s;
 int conn = -1, lfd = -1, seq;
 size_t n, m;

 e_dap_reader_init(&rd);
 s = start_session(&conn, &lfd);
 if (!s || conn < 0)
  return;
 e_dap_add_breakpoint(s, "/tmp/p/Main.scala", 6);

 /* initialize answered in two reads */
 seq = expect_request(conn, &rd, "initialize");
 n = response(buf, seq, "initialize", 1, "");
 put(conn, buf, SPLIT_AT);
 CHECK(e_dap_start_pump(s) == E_DAP_START_WAITING, "half a frame finished the start");
 CHECK(!e_dap_start_answered(s), "half a frame counted as the answer");
 put(conn, buf + SPLIT_AT, n - SPLIT_AT);
 CHECK(e_dap_start_pump(s) == E_DAP_START_WAITING, "start should wait for launch");
 CHECK(e_dap_start_answered(s), "the split initialize answer was not taken");
 CHECK(!strcmp(e_dap_start_waits_for(s), "launching"), "should wait on launch");

 /* the launch answer and, after it, `initialized`, in one read */
 seq = expect_request(conn, &rd, "launch");
 n = response(buf, seq, "launch", 1, "");
 m = frame(buf + n, "{\"seq\":2,\"type\":\"event\",\"event\":\"initialized\"}");
 put(conn, buf, n + m);
 CHECK(e_dap_start_pump(s) == E_DAP_START_WAITING, "start should wait for breakpoints");
 CHECK(!strcmp(e_dap_start_waits_for(s), "configuring"), "two frames: initialized lost");

 seq = expect_request(conn, &rd, "setBreakpoints");
 n = response(buf, seq, "setBreakpoints", 1, "");
 put(conn, buf, n);
 CHECK(e_dap_start_pump(s) == E_DAP_START_WAITING, "start should wait for configurationDone");

 seq = expect_request(conn, &rd, "configurationDone");
 n = response(buf, seq, "configurationDone", 1, "");
 put(conn, buf, n);
 CHECK(e_dap_start_pump(s) == E_DAP_START_READY, "configurationDone did not finish the start");

 e_dap_close(s);
 close(conn);
 close(lfd);
 e_dap_reader_free(&rd);
}

static void test_start_refused(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_session *s;
 int conn = -1, lfd = -1, seq;
 size_t n;

 e_dap_reader_init(&rd);
 s = start_session(&conn, &lfd);
 if (!s || conn < 0)
  return;
 seq = expect_request(conn, &rd, "initialize");
 n = response(buf, seq, "initialize", 0, "adapter says no");
 put(conn, buf, n);
 CHECK(e_dap_start_pump(s) == E_DAP_START_FAILED, "a refusal did not end the start");
 CHECK(e_dap_start_error(s) && !strcmp(e_dap_start_error(s), "adapter says no"),
       "the adapter's reason was not kept");
 e_dap_close(s);
 close(conn);
 close(lfd);
 e_dap_reader_free(&rd);
}

/* ---- the run: pumped one read at a time, as the editor's loop does -----
 *
 * The test plays the adapter again.  Every message it writes is complete
 * before the engine is asked to read, so a read never blocks; where a
 * synchronous call is driven, the answers are written first, under the seq
 * the engine will give its request (the engine numbers them one by one).
 */

#define TEST_FILE    "/xwpe-test-none/Main.scala"   /* realpath() leaves it */
#define BP_SEEN_MAX  8
#define TEXT_MAX     256
#define FAKE_FLAG    "--fake-adapter"
#define FAKE_ANSWERS "answers"     /* answers disconnect late, exits on EOF */
#define FAKE_SILENT  "silent"      /* never answers, ignores SIGTERM        */
#define FAKE_ANSWER_DELAY_MS 300
#define QUIT_DEADLINE_MS     2000  /* the engine's wait for disconnect      */
#define QUIT_TERM_MS         1000  /* its wait after SIGTERM                */
#define QUIT_SLACK_MS        1000
#define FAKE_EXIT_DELAY_MS   100   /* exits a moment after its input ends  */
#define FAKE_TERM    "term"        /* closes its pipes, cleans up on SIGTERM */
#define FAKE_TERM_CLEANUP_MS 200   /* well inside the engine's grace        */
#define PUMP_BLOCK_GUARD_S   2     /* a pump that blocks is a failure       */
#define QUIT_EXIT_MS         2000  /* the engine's wait for the exit        */
#define FAKE_FLOOD   "flood"       /* never answers, writes output for ever */
#define FLOOD_GUARD_S        15    /* a Quit that never ends is a failure   */
/* An answered Quit: with an exit descriptor (pidfd, kqueue) the adapter is
   reaped as it exits, well inside the answer wait.  Without one (the
   DAP_NO_PIDFD build) its exit can be seen only by the bound, so the
   documented bound is the answer plus the whole exit wait. */
#if defined(DAP_NO_PIDFD)
# define ANSWERED_QUIT_BOUND_MS \
  (FAKE_ANSWER_DELAY_MS + QUIT_EXIT_MS + QUIT_SLACK_MS)
#else
# define ANSWERED_QUIT_BOUND_MS QUIT_DEADLINE_MS
#endif

static char self_path[PATH_MAX];   /* this program: the fake adapter */

/* What the host was told. */
struct seen {
 int  stops;
 int  line;
 char file[TEXT_MAX];
 char reason[32];
 int  ended;
 char out[TEXT_MAX];
 int  bp_calls;
 int  bp_n;
 char bp_file[TEXT_MAX];
 int  bp_line[BP_SEEN_MAX];
 int  bp_verified[BP_SEEN_MAX];
 char bp_msg[BP_SEEN_MAX][64];
};

static const char *or_empty(const char *text)
{
 if (!text)
  return "";
 return text;
}

static const char *bool_text(int b)
{
 if (b)
  return "true";
 return "false";
}

/* A message's own seq, or -1. */
static int req_seq(struct json_object *m)
{
 struct json_object *v = NULL;

 if (!m || !json_object_object_get_ex(m, "seq", &v))
  return -1;
 return json_object_get_int(v);
}

static void seen_stopped(const char *file, int line, const char *reason,
                         void *ud)
{
 struct seen *r = ud;

 r->stops++;
 r->line = line;
 snprintf(r->file, sizeof r->file, "%s", or_empty(file));
 snprintf(r->reason, sizeof r->reason, "%s", or_empty(reason));
}

static void seen_output(const char *text, void *ud)
{
 struct seen *r = ud;
 size_t have = strlen(r->out);

 snprintf(r->out + have, sizeof r->out - have, "%s", text);
}

static void seen_terminated(void *ud)
{
 ((struct seen *)ud)->ended = 1;
}

static void seen_breakpoints(const char *file, const int *line,
                             const int *verified, const char *const *message,
                             int n, void *ud)
{
 struct seen *r = ud;
 int i;

 r->bp_calls++;
 r->bp_n = n;
 snprintf(r->bp_file, sizeof r->bp_file, "%s", file);
 for (i = 0; i < n && i < BP_SEEN_MAX; i++)
 {
  r->bp_line[i] = line[i];
  r->bp_verified[i] = verified[i];
  snprintf(r->bp_msg[i], sizeof r->bp_msg[i], "%s", or_empty(message[i]));
 }
}

static void seen_host(e_dap_host *h, struct seen *r)
{
 memset(r, 0, sizeof(*r));
 memset(h, 0, sizeof(*h));
 h->on_stopped = seen_stopped;
 h->on_output = seen_output;
 h->on_terminated = seen_terminated;
 h->on_breakpoints = seen_breakpoints;
 h->ud = r;
}

/* Pump as the editor's loop does: once the transport is readable.  A
   loopback write can reach the reader a moment after write() returns, and
   the pump never blocks to wait for it.  With nothing to read within the
   bound, it pumps anyway (it answers what was already read). */
#define PUMP_WAIT_MS 2000
static int pump_when_readable(e_dap_session *s)
{
 struct pollfd p;

 p.fd = e_dap_fd(s);
 p.events = POLLIN;
 p.revents = 0;
 if (p.fd >= 0)
  poll(&p, 1, PUMP_WAIT_MS);
 return e_dap_run_pump(s);
}

/* Write one framed message. */
static void put_msg(int fd, const char *body)
{
 char buf[ADAPTER_BUF];
 size_t n = frame(buf, body);

 put(fd, buf, n);
}

/* Write two framed messages in one write, so one read takes both. */
static void put_two(int fd, const char *first, const char *second)
{
 char buf[ADAPTER_BUF];
 size_t n = frame(buf, first);

 n += frame(buf + n, second);
 put(fd, buf, n);
}

/* A successful response to @seq with @body (JSON object text). */
static void answer_body(char *dst, size_t sz, int seq, const char *command,
                        const char *body)
{
 snprintf(dst, sz,
          "{\"seq\":1,\"type\":\"response\",\"request_seq\":%d,"
          "\"command\":\"%s\",\"success\":true,\"body\":%s}",
          seq, command, body);
}

/* A plain response to @seq. */
static void answer_plain(char *dst, size_t sz, int seq, const char *command,
                         int success)
{
 snprintf(dst, sz,
          "{\"seq\":1,\"type\":\"response\",\"request_seq\":%d,"
          "\"command\":\"%s\",\"success\":%s}",
          seq, command, bool_text(success));
}

/* The stackTrace answer: the top frame at @line of TEST_FILE. */
static void answer_stack(char *dst, size_t sz, int seq, int line)
{
 char body[ADAPTER_BUF / 4];

 snprintf(body, sizeof body,
          "{\"stackFrames\":[{\"id\":7,\"name\":\"main\",\"line\":%d,"
          "\"source\":{\"path\":\"" TEST_FILE "\"}},"
          "{\"id\":8,\"name\":\"outer\",\"line\":3,"
          "\"source\":{\"path\":\"" TEST_FILE "\"}}]}", line);
 answer_body(dst, sz, seq, "stackTrace", body);
}

/* A `stopped` event; @thread < 0 leaves threadId out (dlv's pause). */
static void stopped_event(char *dst, size_t sz, const char *reason, int thread)
{
 if (thread < 0)
  snprintf(dst, sz, "{\"seq\":1,\"type\":\"event\",\"event\":\"stopped\","
           "\"body\":{\"reason\":\"%s\"}}", reason);
 else
  snprintf(dst, sz, "{\"seq\":1,\"type\":\"event\",\"event\":\"stopped\","
           "\"body\":{\"reason\":\"%s\",\"threadId\":%d}}", reason, thread);
}

/* Read the next request, check its command, return it (caller frees). */
static struct json_object *take_request(int fd, e_dap_reader *rd,
                                        const char *command)
{
 struct json_object *m = next_request(fd, rd);

 CHECK(m != NULL, "the engine sent no request");
 if (m)
  CHECK(str_field(m, "command") && !strcmp(str_field(m, "command"), command),
        command);
 return m;
}

/* An int field of a request's arguments, or -1. */
static int arg_int(struct json_object *m, const char *k)
{
 struct json_object *a = NULL, *v = NULL;

 if (!m || !json_object_object_get_ex(m, "arguments", &a))
  return -1;
 if (!json_object_object_get_ex(a, k, &v))
  return -1;
 return json_object_get_int(v);
}

#define INITIALIZED_EVENT \
 "{\"seq\":2,\"type\":\"event\",\"event\":\"initialized\"}"

/* Where a stop is written with the start's last answer, if at all. */
#define STOP_NONE   0
#define STOP_BEFORE 1    /* the stop, then the configurationDone answer */
#define STOP_AFTER  2    /* the configurationDone answer, then the stop */

/* Connect a session with @host and run its start up to configurationDone
   (no breakpoints: initialize, launch + initialized).  Returns the session
   with *cd_seq set to configurationDone's seq, or NULL. */
static e_dap_session *configuring_session(const e_dap_host *host, int *conn,
                                          int *lfd, e_dap_reader *rd,
                                          int *cd_seq)
{
 char buf[ADAPTER_BUF];
 int port = 0, seq;
 size_t n;
 e_dap_session *s;

 *lfd = listen_local(&port);
 CHECK(*lfd >= 0, "cannot listen on 127.0.0.1");
 if (*lfd < 0)
  return NULL;
 s = e_dap_start_tcp("127.0.0.1", port, "/tmp/p", NULL, host);
 *conn = accept(*lfd, NULL, NULL);
 CHECK(s != NULL && *conn >= 0, "loopback session not connected");
 if (!s || *conn < 0)
  return NULL;
 seq = expect_request(*conn, rd, "initialize");
 put(*conn, buf, response(buf, seq, "initialize", 1, ""));
 e_dap_start_pump(s);
 seq = expect_request(*conn, rd, "launch");
 n = response(buf, seq, "launch", 1, "");
 n += frame(buf + n, INITIALIZED_EVENT);
 put(*conn, buf, n);
 e_dap_start_pump(s);
 *cd_seq = expect_request(*conn, rd, "configurationDone");
 return s;
}

/* Answer configurationDone, with a stop in the same write as @stop says,
   and take the start to ready.  The program then runs (or has stopped). */
static void answer_configuration(e_dap_session *s, int conn, int cd_seq,
                                 int stop)
{
 char buf[ADAPTER_BUF], ev[ADAPTER_BUF / 4], ans[ADAPTER_BUF / 4];

 stopped_event(ev, sizeof ev, "breakpoint", 1);
 answer_plain(ans, sizeof ans, cd_seq, "configurationDone", 1);
 if (stop == STOP_BEFORE)
  put_two(conn, ev, ans);
 else if (stop == STOP_AFTER)
  put_two(conn, ans, ev);
 else
  put(conn, buf, frame(buf, ans));
 CHECK(e_dap_start_pump(s) == E_DAP_START_READY, "start not ready");
}

/* A session whose start is ready and whose program runs.  NULL on error. */
static e_dap_session *ready_session(const e_dap_host *host, int *conn,
                                    int *lfd, e_dap_reader *rd)
{
 int cd_seq = -1;
 e_dap_session *s = configuring_session(host, conn, lfd, rd, &cd_seq);

 if (s)
  answer_configuration(s, *conn, cd_seq, STOP_NONE);
 return s;
}

/* The engine's next request, if it sent one within @ms.  Returns its seq,
   or -1 (a CHECK failure) when it sent none: never a blocked test. */
static int request_within(int fd, e_dap_reader *rd, int ms,
                          const char *command)
{
 struct pollfd p;

 p.fd = fd;
 p.events = POLLIN;
 p.revents = 0;
 if (rd->len == 0 && poll(&p, 1, ms) <= 0)
 {
  printf("FAIL: the engine sent no %s request\n", command);
  failures++;
  return -1;
 }
 return expect_request(fd, rd, command);
}

/* The running program stops at @line: the stop, then its location, one
   pump each.  Returns the pump's answer to the location. */
static int stop_at(int conn, e_dap_reader *rd, e_dap_session *s, int line,
                   const char *reason, int thread)
{
 char buf[ADAPTER_BUF];
 int seq;

 stopped_event(buf, sizeof buf, reason, thread);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING,
       "a stop is told only once it is located");
 seq = expect_request(conn, rd, "stackTrace");
 answer_stack(buf, sizeof buf, seq, line);
 put_msg(conn, buf);
 return pump_when_readable(s);
}

/* Resume and take the adapter's plain answer.  Returns the resume's seq. */
static int resume(int conn, e_dap_reader *rd, e_dap_session *s,
                  const char *verb)
{
 char buf[ADAPTER_BUF];
 int seq;

 CHECK(e_dap_resume(s, verb) == 0, "resume at a stop refused");
 seq = expect_request(conn, rd, verb);
 answer_plain(buf, sizeof buf, seq, verb, 1);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING,
       "a resumed program is not going");
 return seq;
}

/* End a loopback session without making close wait for an answer: the
   adapter's side stops writing, so the engine reads the end at once. */
static void end_session(e_dap_session *s, int conn, int lfd, e_dap_reader *rd)
{
 shutdown(conn, SHUT_WR);
 e_dap_close(s);
 close(conn);
 close(lfd);
 e_dap_reader_free(rd);
}

/* @scenario SC-debug-05 */
static void test_pump_going_stopped_ended(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = ready_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_fd(s) >= 0, "a ready session has no descriptor to watch");
 CHECK(e_dap_running(s), "the program runs once the start is done");
 put_msg(conn, "{\"seq\":3,\"type\":\"event\",\"event\":\"output\","
               "\"body\":{\"category\":\"stdout\",\"output\":\"x\"}}");
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "output ended the run");
 CHECK(e_dap_resume(s, "continue") == -1, "resumed a running program");
 CHECK(stop_at(conn, &rd, s, 6, "breakpoint", 1) == E_DAP_RUN_STOPPED,
       "pump did not answer STOPPED at the stop");
 CHECK(r.stops == 1 && r.line == 6 && !strcmp(r.reason, "breakpoint"),
       "on_stopped did not get line 6, reason breakpoint");
 CHECK(!strcmp(r.file, TEST_FILE), "on_stopped did not get the file");
 CHECK(!e_dap_running(s), "a stopped program counted as running");
 CHECK(e_dap_pause(s) == -1, "paused a stopped program");
 resume(conn, &rd, s, "continue");
 CHECK(e_dap_running(s), "a resumed program is not running");
 put_msg(conn, "{\"seq\":4,\"type\":\"event\",\"event\":\"output\","
               "\"body\":{\"category\":\"stdout\",\"output\":\"tick\\n\"}}");
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "output while running");
 CHECK(strstr(r.out, "tick") != NULL, "program output was not passed on");
 snprintf(buf, sizeof buf,
          "{\"seq\":5,\"type\":\"event\",\"event\":\"terminated\"}");
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_ENDED, "pump did not answer ENDED");
 CHECK(r.ended && e_dap_ended(s), "the end was not told");
 end_session(s, conn, lfd, &rd);
}

/* The breakpoint answer of TEST_FILE lines 6 (pending) and 9 (verified). */
static void answer_breakpoints_pending(char *dst, size_t sz, int seq)
{
 answer_body(dst, sz, seq, "setBreakpoints",
             "{\"breakpoints\":[{\"id\":1,\"verified\":false,"
             "\"reason\":\"pending\"},{\"id\":2,\"verified\":true,"
             "\"line\":9}]}");
}

/* A session stopped at line 6, reached through the pump. */
static e_dap_session *stopped_session(const e_dap_host *host, int *conn,
                                      int *lfd, e_dap_reader *rd)
{
 e_dap_session *s = ready_session(host, conn, lfd, rd);

 if (!s)
  return NULL;
 CHECK(stop_at(*conn, rd, s, 6, "breakpoint", 1) == E_DAP_RUN_STOPPED,
       "the first stop was not reached");
 return s;
}

/* @scenario SC-debug-08 */
static void test_late_breakpoints_during_stack_trace(void)
{
 static const int lines[] = { 6, 9 };
 char a1[ADAPTER_BUF / 2], a2[ADAPTER_BUF / 2];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_frame frames[BP_SEEN_MAX];
 e_dap_session *s;
 struct json_object *m;
 int conn = -1, lfd = -1, seq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = stopped_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_set_file_breakpoints_async(s, TEST_FILE, lines, 2) == 0,
       "async setBreakpoints refused at a stop");
 seq = expect_request(conn, &rd, "setBreakpoints");
 CHECK(r.bp_calls == 0, "breakpoints told before the adapter answered");
 /* The answer comes while the Stack window's stackTrace is waited for. */
 answer_breakpoints_pending(a1, sizeof a1, seq);
 answer_stack(a2, sizeof a2, seq + 1, 6);
 put_two(conn, a1, a2);
 CHECK(e_dap_stack_trace(s, frames, BP_SEEN_MAX) == 2,
       "stackTrace lost its own answer");
 m = take_request(conn, &rd, "stackTrace");
 CHECK(req_seq(m) == seq + 1, "stackTrace was not the next request");
 if (m)
  json_object_put(m);
 CHECK(r.bp_calls == 1, "the late setBreakpoints answer was dropped");
 CHECK(r.bp_n == 2 && r.bp_line[0] == 6 && r.bp_line[1] == 9,
       "the answer did not name the lines asked for");
 CHECK(!r.bp_verified[0] && r.bp_verified[1], "verified flags wrong");
 CHECK(!strcmp(r.bp_msg[0], "pending"), "the pending reason was not given");
 CHECK(!strcmp(r.bp_file, TEST_FILE), "the answer did not name the file");
 end_session(s, conn, lfd, &rd);
}

/* @scenario SC-debug-12 */
static void test_events_during_evaluate_are_delivered(void)
{
 static const int lines[] = { 6, 9 };
 char buf[ADAPTER_BUF], ev[ADAPTER_BUF / 4];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 char *val;
 int conn = -1, lfd = -1, seq;
 size_t n;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = stopped_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_set_file_breakpoints_async(s, TEST_FILE, lines, 2) == 0,
       "async setBreakpoints refused");
 seq = expect_request(conn, &rd, "setBreakpoints");
 answer_breakpoints_pending(buf, sizeof buf, seq);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING,
       "an answer at a stop is no stop");
 CHECK(r.bp_calls == 1, "setBreakpoints answer not told at a stop");
 /* gdb: output, then the pending breakpoint turns verified (the event names
    only its id), then the watch's value. */
 n = frame(buf, "{\"seq\":3,\"type\":\"event\",\"event\":\"output\","
                "\"body\":{\"category\":\"stdout\",\"output\":\"during\"}}");
 n += frame(buf + n, "{\"seq\":4,\"type\":\"event\",\"event\":\"breakpoint\","
                     "\"body\":{\"reason\":\"changed\",\"breakpoint\":"
                     "{\"id\":1,\"verified\":true}}}");
 answer_body(ev, sizeof ev, seq + 1, "evaluate", "{\"result\":\"42\"}");
 n += frame(buf + n, ev);
 put(conn, buf, n);
 val = e_dap_evaluate(s, "x");
 CHECK(val && !strcmp(val, "42"), "evaluate lost its answer");
 free(val);
 CHECK(strstr(r.out, "during") != NULL, "output during a wait was lost");
 CHECK(r.bp_calls == 2, "the breakpoint event during a wait was lost");
 CHECK(r.bp_n == 1 && r.bp_line[0] == 6 && r.bp_verified[0],
       "the event did not turn line 6 verified");
 end_session(s, conn, lfd, &rd);
}

/* @scenario SC-debug-08 */
static void test_late_breakpoints_during_the_stops_location(void)
{
 static const int lines[] = { 12 };
 char a1[ADAPTER_BUF / 2], a2[ADAPTER_BUF / 2];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1, bseq, tseq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = stopped_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 resume(conn, &rd, s, "continue");
 CHECK(e_dap_set_file_breakpoints_async(s, TEST_FILE, lines, 1) == 0,
       "async setBreakpoints refused while running");
 bseq = expect_request(conn, &rd, "setBreakpoints");
 stopped_event(a1, sizeof a1, "breakpoint", 1);
 put_msg(conn, a1);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "stop told before located");
 tseq = expect_request(conn, &rd, "stackTrace");
 answer_stack(a1, sizeof a1, tseq, 12);
 answer_body(a2, sizeof a2, bseq, "setBreakpoints",
             "{\"breakpoints\":[{\"id\":5,\"verified\":true,\"line\":12}]}");
 put_two(conn, a1, a2);
 CHECK(pump_when_readable(s) == E_DAP_RUN_STOPPED, "the stop was not told");
 CHECK(r.line == 12, "the stop was not at line 12");
 CHECK(r.bp_calls == 1 && r.bp_n == 1 && r.bp_line[0] == 12 &&
       r.bp_verified[0], "the answer after the stop's location was lost");
 end_session(s, conn, lfd, &rd);
}

/* Write a stop, the continue answer after it and the stop's location, then
   the end, before e_dap_step reads any of it. */
static void put_stop_before_resume_answer(int conn, int resume_seq, int line)
{
 char buf[ADAPTER_BUF], m[ADAPTER_BUF / 2];
 size_t n;

 stopped_event(m, sizeof m, "breakpoint", 1);
 n = frame(buf, m);
 answer_plain(m, sizeof m, resume_seq, "continue", 1);
 n += frame(buf + n, m);
 answer_stack(m, sizeof m, resume_seq + 1, line);
 n += frame(buf + n, m);
 n += frame(buf + n, "{\"seq\":9,\"type\":\"event\",\"event\":\"terminated\"}");
 put(conn, buf, n);
}

/* dlv sends a stop before its answer to the request that caused it.
   @scenario SC-debug-05 */
static void test_stop_before_the_resume_answer_is_the_stop(void)
{
 char buf[ADAPTER_BUF], m[ADAPTER_BUF / 2];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1, seq;
 size_t n;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = ready_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 /* The first stop, through the synchronous API. */
 stopped_event(m, sizeof m, "breakpoint", 1);
 n = frame(buf, m);
 put(conn, buf, n);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "stop told before located");
 seq = expect_request(conn, &rd, "stackTrace");
 answer_stack(buf, sizeof buf, seq, 6);
 put_msg(conn, buf);
 CHECK(e_dap_run_to_first_stop(s) == 0 && r.line == 6, "first stop lost");
 put_stop_before_resume_answer(conn, seq + 1, 12);
 e_dap_step(s, "continue");
 CHECK(r.stops == 2 && r.line == 12,
       "a stop that came before the continue answer was lost");
 CHECK(r.ended && e_dap_ended(s),
       "the end read after the stop was left queued by e_dap_step");
 end_session(s, conn, lfd, &rd);
}

/* Pause the running program and take the stop the adapter sends for it.
   Returns the pump's answer to the stop's location. */
static int pause_and_stop(int conn, e_dap_reader *rd, e_dap_session *s,
                          const char *stop_body, int answer_first,
                          int *thread_asked)
{
 char a1[ADAPTER_BUF / 2], a2[ADAPTER_BUF / 2];
 struct json_object *m;
 int pseq;

 CHECK(e_dap_pause(s) == 0, "pause refused while running");
 pseq = expect_request(conn, rd, "pause");
 answer_plain(a1, sizeof a1, pseq, "pause", 1);
 if (answer_first)
  put_two(conn, a1, stop_body);
 else
  put_msg(conn, stop_body);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "pause stop told unlocated");
 m = take_request(conn, rd, "stackTrace");
 *thread_asked = arg_int(m, "threadId");
 answer_stack(a2, sizeof a2, req_seq(m), 20);
 if (m)
  json_object_put(m);
 if (answer_first)
  put_msg(conn, a2);
 else
  put_two(conn, a1, a2);
 return pump_when_readable(s);
}

/* @scenario SC-debug-05 */
static void test_pause_stop_whatever_its_reason(void)
{
 char ev[ADAPTER_BUF / 2], buf[ADAPTER_BUF / 2];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1, thread = 0, seq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = ready_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(stop_at(conn, &rd, s, 6, "breakpoint", 3) == E_DAP_RUN_STOPPED,
       "first stop");
 /* lldb-dap: the pause stops with reason "exception" (SIGSTOP). */
 resume(conn, &rd, s, "continue");
 snprintf(ev, sizeof ev, "{\"seq\":3,\"type\":\"event\",\"event\":"
          "\"stopped\",\"body\":{\"reason\":\"exception\",\"description\":"
          "\"signal SIGSTOP\",\"threadId\":3}}");
 CHECK(pause_and_stop(conn, &rd, s, ev, 1, &thread) == E_DAP_RUN_STOPPED,
       "the pause stop was not told");
 CHECK(!strcmp(r.reason, "pause"), "an exception stop after pause not pause");
 /* dlv: reason pause, no threadId, before the pause answer. */
 resume(conn, &rd, s, "continue");
 stopped_event(ev, sizeof ev, "pause", -1);
 CHECK(pause_and_stop(conn, &rd, s, ev, 0, &thread) == E_DAP_RUN_STOPPED,
       "dlv's pause stop was not told");
 CHECK(!strcmp(r.reason, "pause"), "dlv's pause stop not pause");
 CHECK(thread == 3, "a stop without threadId lost the stopped thread");
 /* A refused pause is forgotten: the next stop keeps its own reason. */
 resume(conn, &rd, s, "continue");
 CHECK(e_dap_pause(s) == 0, "pause refused while running");
 seq = expect_request(conn, &rd, "pause");
 answer_plain(buf, sizeof buf, seq, "pause", 0);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "a refused pause is no stop");
 CHECK(stop_at(conn, &rd, s, 9, "breakpoint", 3) == E_DAP_RUN_STOPPED,
       "the stop after a refused pause");
 CHECK(!strcmp(r.reason, "breakpoint"), "a refused pause still renamed");
 end_session(s, conn, lfd, &rd);
}

#define REQUEST_WAIT_MS 2000

/* A stop read in the same chunk as the start's last answer, before or
   after it, reaches the host without the adapter writing again.
   @scenario SC-debug-05 */
static void stop_with_the_starts_last_answer(int stop, const char *why)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1, cd_seq = -1, seq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = configuring_session(&host, &conn, &lfd, &rd, &cd_seq);
 if (!s)
  return;
 answer_configuration(s, conn, cd_seq, stop);
 /* The start already passed the stop on: its location was asked for. */
 seq = request_within(conn, &rd, REQUEST_WAIT_MS, "stackTrace");
 if (seq < 0)
  printf("  (%s)\n", why);
 /* The host's one pump after READY never blocks, readable or not. */
 CHECK(e_dap_run_pump(s) == E_DAP_RUN_GOING, "stop told before located");
 answer_stack(buf, sizeof buf, seq, 6);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_STOPPED && r.line == 6,
       why);
 end_session(s, conn, lfd, &rd);
}

static void test_stop_read_with_the_starts_last_answer(void)
{
 stop_with_the_starts_last_answer(STOP_AFTER,
  "a stop read with the configurationDone answer was left queued");
 stop_with_the_starts_last_answer(STOP_BEFORE,
  "a stop before the configurationDone answer was not located");
}

/* An answer read together with the answer a synchronous call waited for
   is passed on before the call returns.
   @scenario SC-debug-08 */
static void test_answer_read_with_an_evaluate_answer(void)
{
 static const int lines[] = { 6, 9 };
 char a1[ADAPTER_BUF / 2], a2[ADAPTER_BUF / 2];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 char *val;
 int conn = -1, lfd = -1, seq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = stopped_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_set_file_breakpoints_async(s, TEST_FILE, lines, 2) == 0,
       "async setBreakpoints refused");
 seq = expect_request(conn, &rd, "setBreakpoints");
 answer_body(a1, sizeof a1, seq + 1, "evaluate", "{\"result\":\"42\"}");
 answer_breakpoints_pending(a2, sizeof a2, seq);
 put_two(conn, a1, a2);
 val = e_dap_evaluate(s, "x");
 CHECK(val && !strcmp(val, "42"), "evaluate lost its answer");
 free(val);
 CHECK(r.bp_calls == 1,
       "a setBreakpoints answer read after the evaluate answer was left "
       "queued");
 end_session(s, conn, lfd, &rd);
}

/* An adapter that refuses to continue leaves the program where it was;
   that is told as a stop there, not waited for.
   @scenario SC-debug-05 */
static void test_refused_continue_is_a_stop_where_it_was(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1, seq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = stopped_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_resume(s, "continue") == 0, "resume at a stop refused");
 seq = expect_request(conn, &rd, "continue");
 snprintf(buf, sizeof buf,
          "{\"seq\":1,\"type\":\"response\",\"request_seq\":%d,"
          "\"command\":\"continue\",\"success\":false,"
          "\"message\":\"not stopped\"}", seq);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "refusal told unlocated");
 seq = request_within(conn, &rd, REQUEST_WAIT_MS, "stackTrace");
 answer_stack(buf, sizeof buf, seq, 6);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_STOPPED,
       "a refused continue was waited for as a run");
 CHECK(r.stops == 2 && r.line == 6 && !strcmp(r.reason, "refused"),
       "a refused continue was not told as a stop where it was");
 end_session(s, conn, lfd, &rd);
}

/* A watch is evaluated in the frame the stop was located in, not frame 0.
   @scenario SC-debug-10 */
static void test_evaluate_names_the_stopped_frame(void)
{
 static const int lines[] = { 6 };
 char a1[ADAPTER_BUF / 2], a2[ADAPTER_BUF / 2];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 struct json_object *m;
 char *val;
 int conn = -1, lfd = -1, seq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = stopped_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_set_file_breakpoints_async(s, TEST_FILE, lines, 1) == 0,
       "async setBreakpoints refused");
 seq = expect_request(conn, &rd, "setBreakpoints");
 answer_breakpoints_pending(a1, sizeof a1, seq);
 answer_body(a2, sizeof a2, seq + 1, "evaluate", "{\"result\":\"42\"}");
 put_two(conn, a1, a2);
 val = e_dap_evaluate(s, "x");
 free(val);
 m = take_request(conn, &rd, "evaluate");
 CHECK(arg_int(m, "frameId") == 7,
       "evaluate did not name the frame the stop was located in");
 if (m)
  json_object_put(m);
 end_session(s, conn, lfd, &rd);
}

/* A pump that blocked would leave the editor without keys; fail at once. */
static void pump_blocked(int sig)
{
 static const char msg[] = "FAIL: the run pump blocked before the start "
                           "was ready\n";
 ssize_t n;

 (void)sig;
 n = write(STDOUT_FILENO, msg, sizeof msg - 1);
 (void)n;
 _exit(1);
}

/* The editor may pump at any time; before the adapter has answered
   anything, the pump must not wait for it.
   @scenario SC-debug-17 */
static void test_pump_before_the_start_is_ready_never_blocks(void)
{
 e_dap_reader rd;
 e_dap_session *s;
 int port = 0, lfd, conn;

 e_dap_reader_init(&rd);
 lfd = listen_local(&port);
 s = e_dap_start_tcp("127.0.0.1", port, "/tmp/p", NULL, NULL);
 conn = accept(lfd, NULL, NULL);
 CHECK(s != NULL && conn >= 0, "loopback session not connected");
 if (!s || conn < 0)
  return;
 expect_request(conn, &rd, "initialize");   /* and never answered */
 signal(SIGALRM, pump_blocked);
 alarm(PUMP_BLOCK_GUARD_S);
 CHECK(e_dap_run_pump(s) == E_DAP_RUN_GOING,
       "a pump with nothing to read did not answer GOING");
 alarm(0);
 signal(SIGALRM, SIG_DFL);
 close(conn);
 e_dap_close(s);
 close(lfd);
 e_dap_reader_free(&rd);
}

/* @scenario SC-debug-15 */
static void test_eof_in_the_middle_of_a_message_ends_the_pump(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 int conn = -1, lfd = -1;
 size_t n;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = ready_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 n = frame(buf, "{\"seq\":3,\"type\":\"event\",\"event\":\"output\","
                "\"body\":{\"category\":\"stdout\",\"output\":\"half\"}}");
 put(conn, buf, n / 2);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING,
       "half a message ended the run");
 close(conn);
 CHECK(pump_when_readable(s) == E_DAP_RUN_ENDED,
       "the adapter's end in mid-message did not end the run");
 CHECK(e_dap_ended(s) && e_dap_fd(s) < 0,
       "an ended transport is still offered for watching");
 CHECK(pump_when_readable(s) == E_DAP_RUN_ENDED, "a second pump read again");
 e_dap_close(s);
 close(lfd);
 e_dap_reader_free(&rd);
}

/* @scenario SC-debug-18 */
static void test_disconnect_answered_through_the_pump(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 struct json_object *m;
 int conn = -1, lfd = -1;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = ready_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 CHECK(e_dap_disconnect(s) == 0, "disconnect not sent while running");
 m = take_request(conn, &rd, "disconnect");
 CHECK(m && arg_int(m, "terminateDebuggee") == 1,
       "disconnect does not end the program");
 put_msg(conn, "{\"seq\":3,\"type\":\"event\",\"event\":\"output\","
               "\"body\":{\"category\":\"stdout\",\"output\":\"x\"}}");
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "quit done before its answer");
 answer_plain(buf, sizeof buf, req_seq(m), "disconnect", 1);
 if (m)
  json_object_put(m);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_ENDED, "the answered quit not ENDED");
 e_dap_close(s);
 CHECK(next_request(conn, &rd) == NULL, "close sent a second request");
 close(conn);
 close(lfd);
 e_dap_reader_free(&rd);
}

/* A stop's location answered after Quit began is not a stop: Quit goes on
   waiting for its own answer instead of ending as if stopped.
   @scenario SC-debug-18 */
static void test_late_location_during_quit_does_not_stop_it(void)
{
 char buf[ADAPTER_BUF];
 e_dap_reader rd;
 e_dap_host host;
 struct seen r;
 e_dap_session *s;
 struct json_object *m;
 int conn = -1, lfd = -1, tseq;

 e_dap_reader_init(&rd);
 seen_host(&host, &r);
 s = ready_session(&host, &conn, &lfd, &rd);
 if (!s)
  return;
 stopped_event(buf, sizeof buf, "breakpoint", 1);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING, "stop told unlocated");
 tseq = expect_request(conn, &rd, "stackTrace");
 CHECK(e_dap_disconnect(s) == 0, "disconnect not sent while locating");
 m = take_request(conn, &rd, "disconnect");
 answer_stack(buf, sizeof buf, tseq, 6);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_GOING,
       "a late stackTrace answer ended Quit as a stop");
 CHECK(r.stops == 0, "a stop was told after Quit began");
 answer_plain(buf, sizeof buf, req_seq(m), "disconnect", 1);
 if (m)
  json_object_put(m);
 put_msg(conn, buf);
 CHECK(pump_when_readable(s) == E_DAP_RUN_ENDED, "the answered quit not ENDED");
 e_dap_close(s);
 close(conn);
 close(lfd);
 e_dap_reader_free(&rd);
}

/* ---- Quit against a real child: this program, playing an adapter ------- */

/* The fake adapter answers Quit's disconnect @mode's way; on a clean exit
   it leaves @marker behind. */
static void fake_reply(int seq, const char *command)
{
 char buf[ADAPTER_BUF], body[ADAPTER_BUF / 2];
 size_t n;

 answer_plain(body, sizeof body, seq, command, 1);
 n = frame(buf, body);
 if (write(STDOUT_FILENO, buf, n) != (ssize_t)n)
  _exit(2);
}

static void fake_mark(const char *marker)
{
 FILE *f = fopen(marker, "w");

 if (f)
  fclose(f);
}

static volatile sig_atomic_t fake_termed = 0;

static void fake_on_term(int sig)
{
 (void)sig;
 fake_termed = 1;
}

/* FAKE_TERM: never answer, close both pipes, and wait for SIGTERM; then
   clean up for a moment, leave the marker and exit.  An adapter killed
   without SIGTERM, or with no time after it, leaves no marker. */
static int fake_wait_for_term(const char *marker)
{
 sigset_t term, old;

 close(STDIN_FILENO);
 close(STDOUT_FILENO);
 sigemptyset(&term);
 sigaddset(&term, SIGTERM);
 sigprocmask(SIG_BLOCK, &term, &old);
 while (!fake_termed)
  sigsuspend(&old);
 poll(NULL, 0, FAKE_TERM_CLEANUP_MS);
 fake_mark(marker);
 return 0;
}

/* FAKE_FLOOD: never answer Quit; write program output in big batches, so
   the pipe is never empty for the reader, until the engine kills it or the
   pipe is gone. */
#define FLOOD_BATCH (64 * 1024)
static int fake_flood(void)
{
 static char batch[FLOOD_BATCH];
 char one[ADAPTER_BUF];
 size_t n = frame(one, "{\"seq\":9,\"type\":\"event\",\"event\":\"output\","
                       "\"body\":{\"category\":\"stdout\","
                       "\"output\":\"flooding output line\\n\"}}");
 size_t len = 0;

 while (len + n <= sizeof batch)
 {
  memcpy(batch + len, one, n);
  len += n;
 }
 while (write(STDOUT_FILENO, batch, len) > 0)
  ;
 return 0;
}

static int fake_disconnect(const char *mode, int seq, const char *marker,
                           e_dap_reader *rd)
{
 struct json_object *m;

 if (!strcmp(mode, FAKE_SILENT))
  for (;;)
   pause();
 if (!strcmp(mode, FAKE_TERM))
 {
  e_dap_reader_free(rd);
  return fake_wait_for_term(marker);
 }
 if (!strcmp(mode, FAKE_FLOOD))
 {
  e_dap_reader_free(rd);
  return fake_flood();
 }
 poll(NULL, 0, FAKE_ANSWER_DELAY_MS);   /* an adapter that takes its time */
 fake_reply(seq, "disconnect");
 while ((m = next_request(STDIN_FILENO, rd)) != NULL)
  json_object_put(m);
 e_dap_reader_free(rd);
 /* Like a real adapter, it cleans up for a moment after its input ends:
    a close that kills it as soon as it answered leaves no marker. */
 poll(NULL, 0, FAKE_EXIT_DELAY_MS);
 fake_mark(marker);
 return 0;
}

static int fake_adapter(const char *mode, const char *marker)
{
 e_dap_reader rd;
 struct json_object *m;
 const char *cmd;
 int seq;

 if (!strcmp(mode, FAKE_SILENT))
  signal(SIGTERM, SIG_IGN);
 if (!strcmp(mode, FAKE_TERM))
  signal(SIGTERM, fake_on_term);
 e_dap_reader_init(&rd);
 while ((m = next_request(STDIN_FILENO, &rd)) != NULL)
 {
  cmd = str_field(m, "command");
  seq = req_seq(m);
  if (cmd && !strcmp(cmd, "initialize"))
   fake_reply(seq, cmd);
  if (cmd && !strcmp(cmd, "disconnect"))
   break;
  json_object_put(m);
 }
 if (!m)
 {
  e_dap_reader_free(&rd);
  return 1;
 }
 json_object_put(m);
 return fake_disconnect(mode, seq, marker, &rd);
}

static long ms_now(void)
{
 struct timespec ts;

 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Open a session on the fake adapter; NULL when it did not answer. */
static e_dap_session *fake_session(const char *mode, const char *marker)
{
 char *argv[5];

 argv[0] = self_path;
 argv[1] = FAKE_FLAG;
 argv[2] = (char *)mode;
 argv[3] = (char *)marker;
 argv[4] = NULL;
 return e_dap_open_stdio(argv, "/tmp/p", NULL, NULL, NULL);
}

/* Never leave the fake behind, whatever the engine did. */
static void fake_gone_or_killed(pid_t pid)
{
 if (pid <= 0 || kill(pid, 0) != 0)
  return;
 kill(pid, SIGKILL);
 waitpid(pid, NULL, 0);
}

/* @scenario SC-debug-18 */
static void test_close_waits_for_the_disconnect_answer(void)
{
 char dir[] = "/tmp/xwpe-dapquit-XXXXXX", marker[PATH_MAX];
 e_dap_session *s;
 pid_t pid;
 long t0, took;

 CHECK(mkdtemp(dir) != NULL, "mkdtemp");
 snprintf(marker, sizeof marker, "%s/exited-cleanly", dir);
 s = fake_session(FAKE_ANSWERS, marker);
 CHECK(s != NULL, "the fake adapter did not answer initialize");
 if (!s)
  return;
 pid = e_dap_adapter_pid(s);
 t0 = ms_now();
 e_dap_close(s);
 took = ms_now() - t0;
 CHECK(access(marker, F_OK) == 0,
       "the adapter was killed before it answered disconnect and exited");
 CHECK(took < ANSWERED_QUIT_BOUND_MS, "close waited past its bound");
 CHECK(kill(pid, 0) != 0 && errno == ESRCH, "the adapter was not reaped");
 fake_gone_or_killed(pid);
 unlink(marker);
 rmdir(dir);
}

/* An adapter still there after Quit gets SIGTERM and time to end its
   program before SIGKILL, also when it has closed every descriptor (on a
   system without pidfd that is the only sign it gives).
   @scenario SC-debug-18 */
static void test_close_terms_before_it_kills(void)
{
 char dir[] = "/tmp/xwpe-dapterm-XXXXXX", marker[PATH_MAX];
 e_dap_session *s;
 pid_t pid;
 long t0, took;

 CHECK(mkdtemp(dir) != NULL, "mkdtemp");
 snprintf(marker, sizeof marker, "%s/ended-on-sigterm", dir);
 s = fake_session(FAKE_TERM, marker);
 CHECK(s != NULL, "the fake adapter did not answer initialize");
 if (!s)
  return;
 pid = e_dap_adapter_pid(s);
 t0 = ms_now();
 e_dap_close(s);
 took = ms_now() - t0;
 CHECK(access(marker, F_OK) == 0,
       "the adapter was killed without SIGTERM and time to end");
 CHECK(took < QUIT_TERM_MS + QUIT_SLACK_MS, "close waited too long");
 CHECK(kill(pid, 0) != 0 && errno == ESRCH, "the adapter was not reaped");
 fake_gone_or_killed(pid);
 unlink(marker);
 rmdir(dir);
}

/* A Quit that never ended would leave the editor hung; fail at once. */
static void quit_never_ended(int sig)
{
 static const char msg[] = "FAIL: Quit did not end under an adapter "
                           "that floods its output\n";
 ssize_t n;

 (void)sig;
 n = write(STDOUT_FILENO, msg, sizeof msg - 1);
 (void)n;
 _exit(1);
}

/* An adapter that never answers Quit and writes output without end cannot
   hold Quit past its bound: the deadline holds while data keeps coming.
   @scenario SC-debug-18 */
static void test_close_ends_under_an_adapter_that_floods(void)
{
 e_dap_session *s;
 pid_t pid;
 long t0, took;

 s = fake_session(FAKE_FLOOD, "/nonexistent");
 CHECK(s != NULL, "the fake adapter did not answer initialize");
 if (!s)
  return;
 pid = e_dap_adapter_pid(s);
 signal(SIGALRM, quit_never_ended);
 alarm(FLOOD_GUARD_S);
 t0 = ms_now();
 e_dap_close(s);
 took = ms_now() - t0;
 alarm(0);
 signal(SIGALRM, SIG_DFL);
 CHECK(took < QUIT_DEADLINE_MS + QUIT_TERM_MS + QUIT_SLACK_MS,
       "Quit under a flooding adapter went past its bound");
 CHECK(kill(pid, 0) != 0 && errno == ESRCH, "the adapter was not reaped");
 fake_gone_or_killed(pid);
}

/* @scenario SC-debug-18 */
static void test_close_kills_an_adapter_that_never_answers(void)
{
 e_dap_session *s;
 pid_t pid;
 long t0, took;

 s = fake_session(FAKE_SILENT, "/nonexistent");
 CHECK(s != NULL, "the fake adapter did not answer initialize");
 if (!s)
  return;
 pid = e_dap_adapter_pid(s);
 t0 = ms_now();
 e_dap_close(s);
 took = ms_now() - t0;
 CHECK(took < QUIT_DEADLINE_MS + QUIT_TERM_MS + QUIT_SLACK_MS,
       "close did not give up on a silent adapter");
 CHECK(kill(pid, 0) != 0 && errno == ESRCH,
       "a silent adapter was left running or unreaped");
 fake_gone_or_killed(pid);
}

int main(int argc, char **argv)
{
 if (argc >= 4 && !strcmp(argv[1], FAKE_FLAG))
  return fake_adapter(argv[2], argv[3]);
 if (!realpath(argv[0], self_path))
  snprintf(self_path, sizeof self_path, "%s", argv[0]);
 signal(SIGPIPE, SIG_IGN);
 test_build();
 test_whole_frame();
 test_split_frame();
 test_two_frames();
 test_malformed_resyncs();
 test_start_machine();
 test_start_refused();
 test_pump_going_stopped_ended();
 test_late_breakpoints_during_stack_trace();
 test_events_during_evaluate_are_delivered();
 test_late_breakpoints_during_the_stops_location();
 test_stop_before_the_resume_answer_is_the_stop();
 test_pause_stop_whatever_its_reason();
 test_stop_read_with_the_starts_last_answer();
 test_answer_read_with_an_evaluate_answer();
 test_refused_continue_is_a_stop_where_it_was();
 test_evaluate_names_the_stopped_frame();
 test_pump_before_the_start_is_ready_never_blocks();
 test_eof_in_the_middle_of_a_message_ends_the_pump();
 test_disconnect_answered_through_the_pump();
 test_late_location_during_quit_does_not_stop_it();
 test_close_waits_for_the_disconnect_answer();
 test_close_kills_an_adapter_that_never_answers();
 test_close_terms_before_it_kills();
 test_close_ends_under_an_adapter_that_floods();
 if (failures)
 {
  printf("test_dap: %d FAILURE(S)\n", failures);
  return 1;
 }
 printf("PASS: DAP transport (build + whole/split/two/malformed frames), "
        "start machine (split, two frames, initialized after launch, "
        "refusal), run pump (going/stopped/ended, late breakpoint answers, "
        "events during waits, stop before its resume answer, pause, stops "
        "read with the start's last answer, an answer read with evaluate's, "
        "refused continue, evaluate's frame, no block before ready, end in "
        "mid-message) and quit (answered, silent, SIGTERM before SIGKILL, "
        "a late stop location, an adapter that floods)\n");
 return 0;
}
