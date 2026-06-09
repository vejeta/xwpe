/* test_dap.c -- unit tests for the DAP transport core (we_dap_proto.c).
 *
 * Pure, offline: no adapter, no network, no xwpe UI.  Exercises the frame
 * builder and the streaming reader (whole frame, split frame, back-to-back
 * frames, and a malformed frame that must resync). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../we_dap_proto.h"

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

int main(void)
{
 test_build();
 test_whole_frame();
 test_split_frame();
 test_two_frames();
 test_malformed_resyncs();
 if (failures)
 {
  printf("test_dap: %d FAILURE(S)\n", failures);
  return 1;
 }
 printf("PASS: DAP transport (build + whole/split/two/malformed frames)\n");
 return 0;
}
