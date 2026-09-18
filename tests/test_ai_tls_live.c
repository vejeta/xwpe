/* LIVE TLS smoke test for the AI HTTP client (needs --enable-ai-tls + network).
 * Does a real HTTPS GET and checks a 200 + non-empty body, exercising the TLS
 * handshake, request write, and streaming framer end to end.  Not part of the
 * default TESTS (it needs the internet); run it by hand:
 *   ./test_ai_tls_live [https://host] [/path]
 */
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include "we_ai_http.h"

int main(int argc, char **argv)
{
#ifndef WPE_AI_TLS
 (void)argc; (void)argv;
 printf("SKIP: built without --enable-ai-tls\n");
 return 0;
#else
 const char *url  = argc > 1 ? argv[1] : "https://api.github.com";
 const char *path = argc > 2 ? argv[2] : "/zen";
 const char *hdrs[] = { "Accept: text/plain", NULL };
 wpe_http_conn c;
 wpe_http_stream s;
 char err[160] = "", buf[4096];
 int i, st, ok;

 if (wpe_http_open(url, &c, err, sizeof err)) {
  printf("open FAIL: %s\n", err);
  return 1;
 }
 if (wpe_http_request(&c, "GET", path, hdrs, NULL)) {
  printf("request FAIL\n");
  wpe_http_close(&c);
  return 1;
 }
 wpe_http_stream_init(&s);
 for (i = 0; i < 200 && !wpe_http_stream_done(&s); i++) {
  struct pollfd pf;
  int pr;
  ssize_t r;
  pf.fd = c.fd; pf.events = POLLIN; pf.revents = 0;
  pr = poll(&pf, 1, 3000);
  if (pr == 0) continue;
  if (pr < 0) break;
  r = wpe_http_read(&c, buf, sizeof buf);
  if (r > 0) wpe_http_stream_push(&s, buf, (size_t)r);
  else if (r < 0) { wpe_http_stream_eof(&s); break; }
 }
 st = wpe_http_stream_status(&s);
 ok = (st == 200 && s.body_len > 0);
 printf("host=%s path=%s status=%d body_len=%zu\n", url, path, st, s.body_len);
 printf("body: %.160s\n", s.body ? s.body : "");
 printf(ok ? "TLS LIVE OK\n" : "TLS LIVE FAIL\n");
 wpe_http_stream_free(&s);
 wpe_http_close(&c);
 return ok ? 0 : 1;
#endif
}
