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
#include <sys/types.h>   /* ssize_t */

/* ----- connection -------------------------------------------------------- */
typedef struct {
 int   fd;          /* underlying socket, or -1                              */
 int   is_tls;      /* 1 when the connection is wrapped in TLS               */
 void *tls;         /* struct tls* (libtls) when is_tls, else NULL           */
 char  host[256];   /* parsed host (for the Host: header / SNI)              */
 int   port;
} wpe_http_conn;

/* Parse "http://host[:port][/...]" or "https://..." into host/port and the
 * leading path (usually "").  Returns 0 on success, -1 on a malformed URL.
 * *is_https is set to 1 for an https URL. */
int wpe_http_parse_url(const char *url, char *host, size_t hostsz,
                       int *port, int *is_https, char *path, size_t pathsz);

/* Connect (and, for https, TLS-handshake) to the endpoint base URL.  Leaves the
 * socket non-blocking.  Returns 0 on success (c is filled), -1 on failure with a
 * short reason in errbuf (may be NULL). */
int  wpe_http_open(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz);
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
int  wpe_http_stream_done(const wpe_http_stream *s);

#endif /* WPE_AI */

#endif /* WE_AI_HTTP_H */
