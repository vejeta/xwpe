/* we_ai_http_sync.h - the HTTP client's waiting calls, for the C tests only.
 *
 * The editor drives every connection from its event loop (we_ai_http.h); a
 * test program has no loop of its own, so these wrap the same steps in a
 * bounded poll().  Linked only into the test programs, never into the editor.
 *
 * Compiled only under --enable-ai (WPE_AI).
 */
#ifndef WE_AI_HTTP_SYNC_H
#define WE_AI_HTTP_SYNC_H

#ifdef WPE_AI

#include "we_ai_http.h"

/* What one bounded wait (wpe_http_wait) saw. */
enum { WPE_HTTP_WAIT_IDLE = 0, WPE_HTTP_WAIT_READY, WPE_HTTP_WAIT_TIMEOUT };

/* One poll() of fd for events, no later than the absolute deadline (in
 * wpe_http_now_ms time).  Returns WPE_HTTP_WAIT_*. */
int  wpe_http_wait(int fd, int events, long deadline);

/* Connect (and, for https, TLS-handshake) to the endpoint base URL, giving up
 * after deadline_ms with "no answer from host:port within N s".  Returns 0 on
 * success (c is filled), -1 on failure with the reason in errbuf. */
int  wpe_http_open(const char *url, wpe_http_conn *c, int deadline_ms,
                   char *errbuf, size_t errsz);

/* Build and send a whole HTTP/1.1 request, waiting while the socket is full
 * (WPE_HTTP_SEND_STALL_MS at most).  Returns 0 on success. */
int  wpe_http_request(wpe_http_conn *c, const char *method, const char *path,
                      const char *const *headers, const char *body);

#endif /* WPE_AI */

#endif /* WE_AI_HTTP_SYNC_H */
