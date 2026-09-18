/* we_ai_http.c - minimal HTTP/1.1 client and streaming body framer for the AI
 * assistant (chunked + line extraction; NDJSON for Ollama, SSE for the cloud
 * backends).  Editor-free and self-contained (unit-testable).
 * Compiled only under --enable-ai (WPE_AI). */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#ifdef WPE_AI_TLS
#include <tls.h>
#endif

#include "we_ai_http.h"

/* ===================== connection ======================================== */

int wpe_http_parse_url(const char *url, char *host, size_t hostsz,
                       int *port, int *is_https, char *path, size_t pathsz)
{
 const char *p = url, *slash, *hostend, *colon;
 size_t hlen;
 int https = 0;

 if (!strncmp(p, "https://", 8)) { https = 1; p += 8; }
 else if (!strncmp(p, "http://", 7)) { p += 7; }
 else return -1;
 if (is_https) *is_https = https;

 slash   = strchr(p, '/');
 hostend = slash ? slash : p + strlen(p);
 colon   = memchr(p, ':', (size_t)(hostend - p));
 hlen    = colon ? (size_t)(colon - p) : (size_t)(hostend - p);
 if (hlen == 0 || hlen >= hostsz) return -1;
 memcpy(host, p, hlen);
 host[hlen] = '\0';

 if (colon) { *port = atoi(colon + 1); if (*port <= 0) return -1; }
 else       { *port = https ? 443 : 80; }

 if (slash) { if (strlen(slash) >= pathsz) return -1; strcpy(path, slash); }
 else if (pathsz) path[0] = '\0';
 return 0;
}

static void ai_http_seterr(char *buf, size_t sz, const char *msg)
{
 if (buf && sz) { strncpy(buf, msg, sz - 1); buf[sz - 1] = '\0'; }
}

int wpe_http_open(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz)
{
 int https = 0, port = 0, fd = -1;
 char host[256], path[8];
 struct addrinfo hints, *res = NULL, *ai;
 char portstr[16];

 memset(c, 0, sizeof *c);
 c->fd = -1;

 if (wpe_http_parse_url(url, host, sizeof host, &port, &https, path, sizeof path)) {
  ai_http_seterr(errbuf, errsz, "malformed endpoint URL");
  return -1;
 }
 strncpy(c->host, host, sizeof c->host - 1);
 c->port = port;

 memset(&hints, 0, sizeof hints);
 hints.ai_family   = AF_UNSPEC;
 hints.ai_socktype = SOCK_STREAM;
 snprintf(portstr, sizeof portstr, "%d", port);
 if (getaddrinfo(host, portstr, &hints, &res) != 0) {
  ai_http_seterr(errbuf, errsz, "cannot resolve host");
  return -1;
 }
 for (ai = res; ai; ai = ai->ai_next) {
  fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) continue;
  if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
  close(fd);
  fd = -1;
 }
 freeaddrinfo(res);
 if (fd < 0) {
  ai_http_seterr(errbuf, errsz, "connection refused");
  return -1;
 }
 c->fd = fd;

 if (https) {
#ifdef WPE_AI_TLS
  struct tls_config *cfg = tls_config_new();
  c->tls = tls_client();
  if (!c->tls || !cfg || tls_configure(c->tls, cfg) < 0 ||
      tls_connect_socket(c->tls, fd, host) < 0) {
   ai_http_seterr(errbuf, errsz, "TLS setup failed");
   if (cfg) tls_config_free(cfg);
   wpe_http_close(c);
   return -1;
  }
  tls_config_free(cfg);
  for (;;) {
   int r = tls_handshake(c->tls);
   if (r == 0) break;
   if (r == TLS_WANT_POLLIN || r == TLS_WANT_POLLOUT) continue;
   ai_http_seterr(errbuf, errsz, "TLS handshake failed");
   wpe_http_close(c);
   return -1;
  }
  c->is_tls = 1;
#else
  ai_http_seterr(errbuf, errsz, "https needs --enable-ai-tls");
  wpe_http_close(c);
  return -1;
#endif
 }
 return 0;
}

void wpe_http_close(wpe_http_conn *c)
{
 if (!c) return;
#ifdef WPE_AI_TLS
 if (c->tls) { tls_close(c->tls); tls_free(c->tls); c->tls = NULL; }
#endif
 if (c->fd >= 0) { close(c->fd); c->fd = -1; }
 c->is_tls = 0;
}

int wpe_http_fd(const wpe_http_conn *c) { return c ? c->fd : -1; }

ssize_t wpe_http_read(wpe_http_conn *c, void *buf, size_t n)
{
 ssize_t r;
#ifdef WPE_AI_TLS
 if (c->is_tls) {
  r = tls_read(c->tls, buf, n);
  if (r == TLS_WANT_POLLIN || r == TLS_WANT_POLLOUT) return 0;
  if (r <= 0) return -1;
  return r;
 }
#endif
 r = read(c->fd, buf, n);
 if (r > 0) return r;
 if (r == 0) return -1;                 /* EOF */
 if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
 return -1;
}

ssize_t wpe_http_write_all(wpe_http_conn *c, const void *buf, size_t n)
{
 size_t off = 0;
 const char *p = buf;
 while (off < n) {
#ifdef WPE_AI_TLS
  if (c->is_tls) {
   ssize_t w = tls_write(c->tls, p + off, n - off);
   if (w == TLS_WANT_POLLIN || w == TLS_WANT_POLLOUT) continue;
   if (w < 0) return -1;
   off += (size_t)w;
   continue;
  }
#endif
  {
   ssize_t w = write(c->fd, p + off, n - off);
   if (w < 0) { if (errno == EINTR) continue; return -1; }
   off += (size_t)w;
  }
 }
 return (ssize_t)off;
}

int wpe_http_request(wpe_http_conn *c, const char *method, const char *path,
                     const char *const *headers, const char *body)
{
 char hdr[8192];
 size_t off = 0, blen = body ? strlen(body) : 0;
 int i, fl;

 off += snprintf(hdr + off, sizeof hdr - off, "%s %s HTTP/1.1\r\n",
                 method, (path && *path) ? path : "/");
 off += snprintf(hdr + off, sizeof hdr - off, "Host: %s:%d\r\n", c->host, c->port);
 off += snprintf(hdr + off, sizeof hdr - off, "User-Agent: xwpe\r\n");
 off += snprintf(hdr + off, sizeof hdr - off, "Accept: */*\r\n");
 off += snprintf(hdr + off, sizeof hdr - off, "Connection: close\r\n");
 if (headers)
  for (i = 0; headers[i]; i++)
   off += snprintf(hdr + off, sizeof hdr - off, "%s\r\n", headers[i]);
 if (body)
  off += snprintf(hdr + off, sizeof hdr - off, "Content-Length: %zu\r\n", blen);
 off += snprintf(hdr + off, sizeof hdr - off, "\r\n");
 if (off >= sizeof hdr) return -1;

 if (wpe_http_write_all(c, hdr, off) < 0) return -1;
 if (body && blen && wpe_http_write_all(c, body, blen) < 0) return -1;

 /* Switch to non-blocking so the streaming reads fit the editor's fd-loop. */
 fl = fcntl(c->fd, F_GETFL, 0);
 if (fl != -1) fcntl(c->fd, F_SETFL, fl | O_NONBLOCK);
 return 0;
}

/* ===================== streaming decoder ================================= */

void wpe_http_stream_init(wpe_http_stream *s)
{
 memset(s, 0, sizeof *s);
 s->content_length = -1;
 s->chunk_left = -1;
}

void wpe_http_stream_free(wpe_http_stream *s)
{
 free(s->raw);
 free(s->body);
 memset(s, 0, sizeof *s);
}

static int ai_buf_append(char **buf, size_t *len, size_t *cap,
                         const char *data, size_t n)
{
 if (*len + n + 1 > *cap) {
  size_t nc = *cap ? *cap : 256;
  char *nb;
  while (*len + n + 1 > nc) nc *= 2;
  nb = realloc(*buf, nc);
  if (!nb) return -1;
  *buf = nb;
  *cap = nc;
 }
 memcpy(*buf + *len, data, n);
 *len += n;
 (*buf)[*len] = '\0';
 return 0;
}

/* Case-insensitive bounded substring search (no NUL assumptions). */
static int ai_line_has(const char *p, size_t len, const char *needle)
{
 size_t nl = strlen(needle), k;
 if (len < nl) return 0;
 for (k = 0; k + nl <= len; k++)
  if (!strncasecmp(p + k, needle, nl)) return 1;
 return 0;
}

/* Parse the HTTP status line + headers.  Scans by index over raw[0..hdr_end)
 * without mutating the buffer, so the trailing body bytes stay intact. */
static void ai_parse_headers(wpe_http_stream *s, size_t hdr_end)
{
 char *h = s->raw;
 size_t i, n = hdr_end;

 { size_t j; for (j = 0; j < n && h[j] != ' '; j++) ;
   if (j < n) s->status = atoi(h + j + 1); }

 i = 0;
 while (i < n) {
  size_t j = i, linelen;
  while (j < n && h[j] != '\n') j++;
  linelen = j - i;
  if (linelen && h[i + linelen - 1] == '\r') linelen--;
  if (linelen > 18 && !strncasecmp(h + i, "Transfer-Encoding:", 18)) {
   if (ai_line_has(h + i, linelen, "chunked")) s->chunked = 1;
  } else if (linelen > 15 && !strncasecmp(h + i, "Content-Length:", 15)) {
   s->content_length = atol(h + i + 15);
  }
  i = j + 1;
 }
 s->raw_consumed = hdr_end + 4;         /* skip the "\r\n\r\n" */
}

/* Turn newly available raw body bytes into decoded body. */
static int ai_decode_body(wpe_http_stream *s)
{
 while (s->raw_consumed < s->raw_len && !s->body_complete) {
  size_t avail = s->raw_len - s->raw_consumed;
  const char *src = s->raw + s->raw_consumed;

  if (!s->chunked) {
   if (ai_buf_append(&s->body, &s->body_len, &s->body_cap, src, avail) < 0)
    return -1;
   s->raw_consumed += avail;
   if (s->content_length >= 0 && (long)s->body_len >= s->content_length)
    s->body_complete = 1;
   continue;
  }

  /* chunked: need a size line first */
  if (s->chunk_left < 0) {
   const char *eol = memchr(src, '\n', avail);
   long sz;
   if (!eol) return 0;                  /* wait for the full size line */
   sz = strtol(src, NULL, 16);
   s->raw_consumed += (size_t)(eol - src) + 1;
   if (sz == 0) { s->body_complete = 1; return 0; }
   s->chunk_left = sz;
   continue;
  }
  /* copy up to chunk_left bytes of chunk data */
  {
   size_t take = avail;
   if ((long)take > s->chunk_left) take = (size_t)s->chunk_left;
   if (take) {
    if (ai_buf_append(&s->body, &s->body_len, &s->body_cap, src, take) < 0)
     return -1;
    s->raw_consumed += take;
    s->chunk_left  -= (long)take;
   }
   if (s->chunk_left == 0) {
    /* consume the trailing CRLF after the chunk data */
    size_t rem = s->raw_len - s->raw_consumed;
    const char *q = s->raw + s->raw_consumed;
    if (rem >= 2 && q[0] == '\r' && q[1] == '\n') s->raw_consumed += 2;
    else if (rem >= 1 && q[0] == '\n')            s->raw_consumed += 1;
    else return 0;                      /* wait for the CRLF */
    s->chunk_left = -1;                  /* next: another size line */
   }
  }
 }
 return 0;
}

int wpe_http_stream_push(wpe_http_stream *s, const char *data, size_t n)
{
 if (ai_buf_append(&s->raw, &s->raw_len, &s->raw_cap, data, n) < 0)
  return -1;
 if (!s->header_done) {
  char *he = strstr(s->raw, "\r\n\r\n");
  if (!he) return 0;                     /* headers not complete yet */
  ai_parse_headers(s, (size_t)(he - s->raw));
  s->header_done = 1;
 }
 return ai_decode_body(s);
}

int wpe_http_stream_next_line(wpe_http_stream *s, char **line, size_t *len)
{
 size_t i;
 for (i = s->line_pos; i < s->body_len; i++) {
  if (s->body[i] == '\n') {
   size_t start = s->line_pos, l = i - start;
   if (l && s->body[start + l - 1] == '\r') l--;   /* strip CR */
   s->body[start + l] = '\0';
   *line = s->body + start;
   *len  = l;
   s->line_pos = i + 1;
   return 1;
  }
 }
 /* At end of stream, hand back a final unterminated line if any. */
 if (s->body_complete && s->line_pos < s->body_len) {
  *line = s->body + s->line_pos;
  *len  = s->body_len - s->line_pos;
  s->line_pos = s->body_len;
  return 1;
 }
 return 0;
}

void wpe_http_stream_eof(wpe_http_stream *s) { s->body_complete = 1; }
int  wpe_http_stream_status(const wpe_http_stream *s) { return s->status; }
int  wpe_http_stream_done(const wpe_http_stream *s) { return s->body_complete; }

#endif /* WPE_AI */

typedef int wpe_ai_http_translation_unit;
