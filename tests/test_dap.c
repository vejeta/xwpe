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
#include <sys/socket.h>
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

int main(void)
{
 test_build();
 test_whole_frame();
 test_split_frame();
 test_two_frames();
 test_malformed_resyncs();
 test_start_machine();
 test_start_refused();
 if (failures)
 {
  printf("test_dap: %d FAILURE(S)\n", failures);
  return 1;
 }
 printf("PASS: DAP transport (build + whole/split/two/malformed frames) "
        "and start machine (split, two frames, initialized after launch, refusal)\n");
 return 0;
}
