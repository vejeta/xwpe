/* Unit test: the AI HTTP streaming framer (we_ai_http.c).  No socket needed --
 * we push raw response bytes and pull decoded body lines. */
#include <stdio.h>
#include <string.h>
#include "we_ai_http.h"

static int fail;
static void check(int cond, const char *msg)
{
 if (!cond) { printf("FAIL: %s\n", msg); fail = 1; }
 else printf("ok: %s\n", msg);
}

int main(void)
{
 char *line;
 size_t len;
 int got;

 /* --- non-chunked NDJSON, pushed in two fragments (reassembly) --- */
 {
  wpe_http_stream s;
  const char *resp =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/x-ndjson\r\n"
    "\r\n"
    "{\"x\":1}\n{\"y\":2}\n";
  size_t rl = strlen(resp);
  wpe_http_stream_init(&s);
  wpe_http_stream_push(&s, resp, 20);          /* split mid-headers/body */
  wpe_http_stream_push(&s, resp + 20, rl - 20);
  wpe_http_stream_eof(&s);                      /* connection closed */
  check(wpe_http_stream_status(&s) == 200, "plain: status 200");
  got = 0;
  while (wpe_http_stream_next_line(&s, &line, &len)) {
   if (got == 0) check(!strcmp(line, "{\"x\":1}"), "plain: line 0");
   if (got == 1) check(!strcmp(line, "{\"y\":2}"), "plain: line 1");
   got++;
  }
  check(got == 2, "plain: two lines");
  wpe_http_stream_free(&s);
 }

 /* --- chunked transfer encoding --- */
 {
  wpe_http_stream s;
  const char *cr =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "8\r\n{\"z\":3}\n\r\n"     /* one 8-byte chunk: {"z":3}\n */
    "0\r\n\r\n";               /* terminating chunk */
  wpe_http_stream_init(&s);
  wpe_http_stream_push(&s, cr, strlen(cr));
  check(wpe_http_stream_done(&s), "chunked: complete");
  got = 0;
  while (wpe_http_stream_next_line(&s, &line, &len)) {
   if (got == 0) check(!strcmp(line, "{\"z\":3}"), "chunked: line 0");
   got++;
  }
  check(got == 1, "chunked: one line");
  wpe_http_stream_free(&s);
 }

 printf(fail ? "SOME TESTS FAILED\n" : "ALL OK\n");
 return fail;
}
