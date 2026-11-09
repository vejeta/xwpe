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
 char *out;         /* the request being sent: header and body in one buffer */
 size_t out_len;    /* its length                                            */
 size_t out_off;    /* how much of it the socket has taken                   */
 long  out_moved;   /* wpe_http_now_ms() when the socket last took a byte    */
} wpe_http_conn;

/* What one step of a send returns. */
enum { WPE_HTTP_SEND_FAILED = -1, WPE_HTTP_SEND_PENDING = 0, WPE_HTTP_SEND_DONE = 1 };

/* How long a send may go without the socket taking a byte before the request
 * is given up: the server has stopped reading it. */
#define WPE_HTTP_SEND_STALL_MS 10000

/* Parse "http://host[:port][/...]" or "https://..." into host/port and the
 * leading path (usually "").  Returns 0 on success, -1 on a malformed URL.
 * *is_https is set to 1 for an https URL. */
int wpe_http_parse_url(const char *url, char *host, size_t hostsz,
                       int *port, int *is_https, char *path, size_t pathsz);

/* The open in steps, for a caller whose event loop waits: begin starts the
 * connect, step advances it when the socket shows the events
 * wpe_http_open_events names.  Both return WPE_HTTP_OPEN_*; on FAILED errbuf
 * holds the reason and the caller closes c.  wpe_http_connect_begin is the
 * same begin without the TLS handshake of an https URL (a reachability
 * check). */
int  wpe_http_open_begin(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz);
int  wpe_http_connect_begin(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz);
int  wpe_http_open_step(wpe_http_conn *c, char *errbuf, size_t errsz);
int  wpe_http_open_events(const wpe_http_conn *c);

/* Is anything listening at the URL's host:port?  A TCP connect only, no TLS,
 * on the editor's loop (we_fdloop.c), bounded by deadline_ms.  Returns
 * WPE_HTTP_OPEN_READY when it connected at once, WPE_HTTP_OPEN_FAILED with
 * the reason in errbuf when it failed at once, or WPE_HTTP_OPEN_PENDING with
 * *out set: done(ok, why, ud) is then called once, from the loop ("cannot
 * connect to host:port (...)", "no answer from host:port within N s"),
 * unless wpe_http_reach_cancel comes first. */
typedef void (*wpe_http_reached_fn)(int ok, const char *why, void *ud);
typedef struct wpe_http_reach wpe_http_reach;
int  wpe_http_reach_start(const char *url, int deadline_ms, wpe_http_reached_fn done,
                          void *ud, wpe_http_reach **out, char *errbuf, size_t errsz);
void wpe_http_reach_cancel(wpe_http_reach *r);

long wpe_http_now_ms(void);

/* The line a request that ran past its deadline ends with:
 * "no answer from host:port within N s". */
void wpe_http_describe_timeout(const wpe_http_conn *c, int deadline_ms,
                               char *errbuf, size_t errsz);

void wpe_http_close(wpe_http_conn *c);
int  wpe_http_fd(const wpe_http_conn *c);

/* Non-blocking read over the connection.  Returns >0 bytes, 0 for "not ready
 * yet, poll again" (EAGAIN / TLS_WANT_*), or -1 on hard error/EOF. */
ssize_t wpe_http_read(wpe_http_conn *c, void *buf, size_t n);

/* Send an HTTP/1.1 request on an open connection, from the caller's event
 * loop.  begin builds the whole request (headers is a NULL-terminated array
 * of "Key: Value" strings; Host and Content-Length are added; body may be
 * NULL) into the connection and returns 0, or -1 when it cannot be built.
 * step writes what the socket takes now and returns WPE_HTTP_SEND_*; while
 * PENDING, wpe_http_open_events names what to wait for (a TLS write that
 * could not finish is retried with the same buffer and length, as TLS
 * requires).  stalled is 1 once the socket has taken no byte for
 * WPE_HTTP_SEND_STALL_MS; describe_stall gives the line for it. */
int  wpe_http_send_begin(wpe_http_conn *c, const char *method, const char *path,
                         const char *const *headers, const char *body);
int  wpe_http_send_step(wpe_http_conn *c);
int  wpe_http_send_stalled(const wpe_http_conn *c, long now_ms);
void wpe_http_describe_stall(const wpe_http_conn *c, char *errbuf, size_t errsz);

/* ----- one whole GET on the editor's loop --------------------------------- */
/* How a GET ended. */
enum { WPE_HTTP_GOT_DONE = 0, WPE_HTTP_GOT_FAILED, WPE_HTTP_GOT_TIMED_OUT };

/* What the continuation of a GET is handed; valid for the call only. */
typedef struct {
 int         how;       /* WPE_HTTP_GOT_*                                     */
 int         status;    /* the HTTP status, 0 when no header came             */
 const char *body;      /* the body ('\0'-terminated), "" when none           */
 size_t      body_len;
 const char *phrase;    /* the status line's reason phrase, or ""             */
 const char *why;       /* FAILED / TIMED_OUT: host:port and the reason       */
} wpe_http_got;

typedef void (*wpe_http_got_fn)(const wpe_http_got *got, void *ud);
typedef struct wpe_http_get wpe_http_get;

/* Start a GET of @path on the host of @url: the connect, the handshake, the
 * request and the whole answer run from the loop's descriptor and timer
 * callbacks (we_fdloop.c), so nobody waits for them.  The answer ends at
 * its framing, at the peer's close, or once @maxbytes of body came (0: no
 * limit); a GET still unanswered @deadline_ms after the start ends as
 * TIMED_OUT, or as DONE with the part of the body that came.  @done is called
 * exactly once, from the loop, unless the GET is cancelled first.
 * Return: the GET, or NULL with the reason in errbuf when it failed at once
 * (a malformed URL, a refused local connect, no room on the loop); @done is
 * then never called. */
wpe_http_get *wpe_http_get_start(const char *url, const char *path,
                                 const char *const *headers, int deadline_ms,
                                 size_t maxbytes, wpe_http_got_fn done, void *ud,
                                 char *errbuf, size_t errsz);
/* Give the GET up: its socket is closed, its timer removed, and @done is
 * not called. */
void wpe_http_get_cancel(wpe_http_get *g);

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
