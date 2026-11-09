/* we_ai_http_sync.c - the HTTP client's waiting calls, for the C tests only.
 *
 * A test program has no event loop, so it waits for the socket itself, here,
 * over the same steps the editor drives from its loop (we_ai_http.c).  This
 * file is linked into the test programs only; the editor never waits on a
 * server.  Compiled only under --enable-ai (WPE_AI).
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

#include "we_ai_http_sync.h"

int wpe_http_wait(int fd, int events, long deadline)
{
 struct pollfd pf;
 int pr;
 long left = deadline - wpe_http_now_ms();
 if (left <= 0)
  return WPE_HTTP_WAIT_TIMEOUT;
 pf.fd = fd;
 pf.events = (short)events;
 pf.revents = 0;
 pr = poll(&pf, 1, (int)left);
 if (pr < 0 && errno != EINTR)
  return WPE_HTTP_WAIT_READY;              /* let the caller's step see the error */
 if (pr > 0 && pf.revents)
  return WPE_HTTP_WAIT_READY;
 return WPE_HTTP_WAIT_IDLE;
}

/**
 * sync_wait_open - Drive an open to its end, no later than the deadline.
 * @c: the connection; @rc: what the begin returned; @deadline: absolute ms;
 * @deadline_ms: the same as a length, for the message; @errbuf, @errsz: the
 * reason on failure.
 *
 * Return: WPE_HTTP_OPEN_READY or WPE_HTTP_OPEN_FAILED.
 */
static int sync_wait_open(wpe_http_conn *c, int rc, long deadline, int deadline_ms,
                          char *errbuf, size_t errsz)
{
 while (rc == WPE_HTTP_OPEN_PENDING) {
  int seen = wpe_http_wait(c->fd, c->want, deadline);
  if (seen == WPE_HTTP_WAIT_TIMEOUT) {
   wpe_http_describe_timeout(c, deadline_ms, errbuf, errsz);
   return WPE_HTTP_OPEN_FAILED;
  }
  if (seen == WPE_HTTP_WAIT_READY)
   rc = wpe_http_open_step(c, errbuf, errsz);
 }
 return rc;
}

int wpe_http_open(const char *url, wpe_http_conn *c, int deadline_ms,
                  char *errbuf, size_t errsz)
{
 long deadline = wpe_http_now_ms() + deadline_ms;
 int rc = wpe_http_open_begin(url, c, errbuf, errsz);
 rc = sync_wait_open(c, rc, deadline, deadline_ms, errbuf, errsz);
 if (rc != WPE_HTTP_OPEN_READY) {
  wpe_http_close(c);
  return -1;
 }
 return 0;
}

int wpe_http_request(wpe_http_conn *c, const char *method, const char *path,
                     const char *const *headers, const char *body)
{
 int rc;
 if (wpe_http_send_begin(c, method, path, headers, body) < 0)
  return -1;
 for (;;) {
  rc = wpe_http_send_step(c);
  if (rc == WPE_HTTP_SEND_DONE)
   return 0;
  if (rc == WPE_HTTP_SEND_FAILED)
   return -1;
  if (wpe_http_send_stalled(c, wpe_http_now_ms()))
   return -1;
  wpe_http_wait(c->fd, c->want, c->out_moved + WPE_HTTP_SEND_STALL_MS);
 }
}

#endif /* WPE_AI */

/* Keep this a non-empty translation unit for ISO C when WPE_AI is off. */
typedef int wpe_ai_http_sync_translation_unit;
