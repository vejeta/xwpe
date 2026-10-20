/* we_ai_http.h - minimal HTTP/1.1 client + streaming body framer for the AI
 * assistant.  Editor-free and self-contained so it can be unit-tested.
 *
 * Two layers:
 *   - a tiny connection object (plain TCP, or TLS under WPE_AI_TLS) with
 *     non-blocking read/write, so the fd can live in the editor's fd-loop;
 *   - a streaming decoder that turns raw response bytes into decoded body
 *     LINES: for Ollama that is one NDJSON object per line; for the SSE
 *     backends (OpenAI-compatible, Claude) that is the raw "data: ..." lines.
 *     The backend layer json-parses each line.
 *
 * Compiled only under --enable-ai (WPE_AI).
 */
#ifndef WE_AI_HTTP_H
#define WE_AI_HTTP_H

#ifdef WPE_AI

#include <stddef.h>
#include <poll.h>        /* POLLIN / POLLOUT, the events an open waits for */
#include <sys/types.h>   /* ssize_t */

/* ----- connection -------------------------------------------------------- */
/* Where an open stands: the TCP connect, the TLS handshake, or done. */
enum { WPE_HTTP_PHASE_CONNECT = 0, WPE_HTTP_PHASE_TLS, WPE_HTTP_PHASE_READY };

/* What one step of an open returns. */
enum { WPE_HTTP_OPEN_FAILED = -1, WPE_HTTP_OPEN_PENDING = 0, WPE_HTTP_OPEN_READY = 1 };

typedef struct {
 int   fd;          /* underlying socket, or -1                              */
 int   is_tls;      /* 1 when the connection is wrapped in TLS               */
 void *tls;         /* SSL* / struct tls* once the handshake began, else NULL */
 char  host[256];   /* parsed host (for the Host: header / SNI)              */
 int   port;
 int   phase;       /* WPE_HTTP_PHASE_*: how far an open has come            */
 int   want;        /* the poll() events the open waits for next             */
 int   use_tls;     /* the open wraps the socket in TLS once connected       */
 void *addrs;       /* struct addrinfo list of the host, while connecting    */
 void *addr_next;   /* the next address to try when one refuses              */
 int   last_errno;  /* the last connect() failure, for the message           */
} wpe_http_conn;

/* What a bounded wait watches besides the socket, so the user can abandon it:
 * fd is polled for input (-1 for none) and cancelled(ud) is asked on every
 * wake; a 1 from it ends the wait with "cancelled". */
typedef struct {
 int   fd;
 int (*cancelled)(void *ud);
 void *ud;
} wpe_http_watch;

/* What one bounded wait (wpe_http_wait) saw. */
enum { WPE_HTTP_WAIT_IDLE = 0, WPE_HTTP_WAIT_READY, WPE_HTTP_WAIT_TIMEOUT,
       WPE_HTTP_WAIT_CANCELLED };

/* Parse "http://host[:port][/...]" or "https://..." into host/port and the
 * leading path (usually "").  Returns 0 on success, -1 on a malformed URL.
 * *is_https is set to 1 for an https URL. */
int wpe_http_parse_url(const char *url, char *host, size_t hostsz,
                       int *port, int *is_https, char *path, size_t pathsz);

/* Connect (and, for https, TLS-handshake) to the endpoint base URL, giving up
 * after deadline_ms with "no answer from host:port within N s", or when the
 * watch (may be NULL) says the user cancelled.  The socket is non-blocking
 * throughout.  Returns 0 on success (c is filled), -1 on failure with a short
 * reason naming host:port in errbuf (may be NULL). */
int  wpe_http_open(const char *url, wpe_http_conn *c, int deadline_ms,
                   const wpe_http_watch *watch, char *errbuf, size_t errsz);

/* The same open in steps, for a caller whose own event loop waits: begin
 * starts the connect, step advances it when the socket shows the events
 * wpe_http_open_events names.  Both return WPE_HTTP_OPEN_*; on FAILED errbuf
 * holds the reason and the caller closes c. */
int  wpe_http_open_begin(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz);
int  wpe_http_open_step(wpe_http_conn *c, char *errbuf, size_t errsz);
int  wpe_http_open_events(const wpe_http_conn *c);

/* Is anything listening at the URL's host:port?  A TCP connect only, no TLS,
 * bounded by deadline_ms.  0 when it is, -1 with the reason in errbuf. */
int  wpe_http_reach(const char *url, int deadline_ms, char *errbuf, size_t errsz);

/* One poll() of fd for events, no later than the absolute deadline (in
 * wpe_http_now_ms time), also watching the user through watch (may be NULL).
 * Returns WPE_HTTP_WAIT_*. */
int  wpe_http_wait(int fd, int events, long deadline, const wpe_http_watch *watch);
long wpe_http_now_ms(void);

/* The line a request that ran past its deadline ends with:
 * "no answer from host:port within N s". */
void wpe_http_describe_timeout(const wpe_http_conn *c, int deadline_ms,
                               char *errbuf, size_t errsz);

void wpe_http_close(wpe_http_conn *c);
int  wpe_http_fd(const wpe_http_conn *c);

/* Non-blocking read/write over the connection.  Return >0 bytes, 0 for
 * "not ready yet, poll again" (EAGAIN / TLS_WANT_*), or -1 on hard error/EOF. */
ssize_t wpe_http_read(wpe_http_conn *c, void *buf, size_t n);
ssize_t wpe_http_write_all(wpe_http_conn *c, const void *buf, size_t n);

/* Build and send an HTTP/1.1 request.  headers is a NULL-terminated array of
 * "Key: Value" strings (Host and Content-Length are added automatically).
 * body may be NULL.  Returns 0 on success. */
int wpe_http_request(wpe_http_conn *c, const char *method, const char *path,
                     const char *const *headers, const char *body);

/* ----- streaming response decoder --------------------------------------- */
typedef struct {
 char  *raw;  size_t raw_len,  raw_cap;   /* bytes straight off the socket    */
 size_t raw_consumed;                     /* raw bytes already turned to body */
 int    header_done;                      /* seen end of HTTP headers         */
 int    status;                           /* HTTP status code                 */
 int    chunked;                          /* Transfer-Encoding: chunked       */
 long   content_length;                   /* Content-Length, or -1 if absent  */
 long   chunk_left;                       /* bytes left in current chunk, or  */
                                          /* -1 when a chunk-size line is due  */
 int    body_complete;                    /* final chunk / EOF / length met   */
 int    framed_end;                       /* the framing itself ended the body:
                                             Content-Length met or the final
                                             chunk seen, not a mere socket EOF */
 char  *body; size_t body_len, body_cap;  /* de-chunked body                  */
 size_t line_pos;                         /* cursor for line extraction       */
} wpe_http_stream;

void wpe_http_stream_init(wpe_http_stream *s);
void wpe_http_stream_free(wpe_http_stream *s);

/* Feed raw socket bytes.  Returns 0 on success, -1 on malformed framing. */
int  wpe_http_stream_push(wpe_http_stream *s, const char *data, size_t n);

/* Pull the next complete body line (without the trailing '\n').  Returns 1 and
 * sets *line (NUL-terminated, owned by s, valid until the next push) and *len,
 * or 0 when no complete line is buffered yet. */
int  wpe_http_stream_next_line(wpe_http_stream *s, char **line, size_t *len);

/* Mark the connection closed (socket EOF): completes a non-chunked body. */
void wpe_http_stream_eof(wpe_http_stream *s);

int  wpe_http_stream_status(const wpe_http_stream *s);
/* The reason phrase of the status line ("Unauthorized" in "HTTP/1.1 401
 * Unauthorized"), or "" when there is none. */
void wpe_http_stream_status_reason(const wpe_http_stream *s, char *out, size_t sz);
int  wpe_http_stream_done(const wpe_http_stream *s);

/* True once the HTTP framing itself ended the body (Content-Length reached or
 * the terminating chunk read).  A body that ended only because the peer closed
 * the socket does not count: the caller tells a complete answer from a
 * connection that died with it. */
int  wpe_http_stream_framed(const wpe_http_stream *s);

#endif /* WPE_AI */

#endif /* WE_AI_HTTP_H */
