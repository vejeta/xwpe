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
#include <time.h>

#ifdef WPE_AI_TLS
# ifdef WPE_AI_TLS_OPENSSL
#  include <openssl/ssl.h>
#  include <openssl/err.h>
# else
#  include <tls.h>
# endif
#endif

#include "we_ai_http.h"

/* The extra CA/self-signed cert to trust, owned by we_ai.c (AICAFile /
 * XWPE_AI_CAFILE); we_ai_http.c does not pull the whole we_ai.h/edit.h graph. */
extern char *e_ai_cafile;

#if defined(WPE_AI_TLS) && defined(WPE_AI_TLS_OPENSSL)
/* One lazily-created client context shared by all connections (single-threaded).
 * Rebuilt when the configured CA file changes (switching to/from a provider that
 * carries its own self-signed cert). */
static SSL_CTX *ai_ssl_ctx(void)
{
 static SSL_CTX *ctx;
 static char *loaded_cafile;         /* the CA file baked into ctx, or NULL       */
 const char *want = (e_ai_cafile && *e_ai_cafile) ? e_ai_cafile : NULL;
 int changed = (!want) != (!loaded_cafile) ||
               (want && loaded_cafile && strcmp(want, loaded_cafile) != 0);
 if (ctx && changed) {
  SSL_CTX_free(ctx); ctx = NULL;
  free(loaded_cafile); loaded_cafile = NULL;
 }
 if (!ctx) {
  ctx = SSL_CTX_new(TLS_client_method());
  if (ctx) {
   SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
   SSL_CTX_set_default_verify_paths(ctx);
   /* Also trust a user-provided CA / self-signed cert (a local HTTPS bridge such
      as proton-cli).  Verification still runs in full -- SSL_VERIFY_PEER plus the
      SSL_set1_host name check below -- just against this CA in addition to the
      system store, so it is not a blanket skip-verify. */
   if (want && SSL_CTX_load_verify_locations(ctx, want, NULL) == 1)
    loaded_cafile = strdup(want);
   /* Enforce peer verification.  Without SSL_VERIFY_PEER the client mode is
      SSL_VERIFY_NONE, so SSL_connect succeeds on ANY certificate -- SSL_set1_host
      records the expected name but nothing acts on the result, leaving the
      connection to a cloud API (carrying the API key) open to a MITM.  With this
      set, SSL_connect fails when the chain or the hostname does not check out. */
   SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
  }
 }
 return ctx;
}
#endif

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

/* ----- the bounded, non-blocking open ------------------------------------
 * The editor never waits inside connect() or the TLS handshake: the socket is
 * non-blocking from its creation, every wait is a poll() with a deadline, and
 * a caller with its own event loop (the chat, the Edit) drives the open one
 * step per wake-up with wpe_http_open_begin / wpe_http_open_step.  Name
 * resolution is the one step outside the deadline: a numeric host or
 * "localhost" resolves at once from the hosts file, and a name is bounded by
 * the system resolver's own timeout. */

/* The words every handshake failure starts with (the TLS tests find the
   OpenSSL branch in the binary by them). */
#define AI_HTTP_TLS_FAILED      "TLS handshake/verify failed"
/* The reason given when the peer hangs up before the handshake ends. */
#define AI_HTTP_TLS_CLOSED      "the server closed the connection during the handshake"
/* Said only when the reason is a certificate the editor could not verify. */
#define AI_HTTP_TLS_CA_ADVICE   " -- if the server uses its own certificate, set its CA file in Options > AI (Alt-C)"
/* How long one stalled write of a request waits for the socket to drain. */
#define AI_HTTP_SEND_STALL_MS   10000
/* How often a watched wait wakes to ask whether the user cancelled, for a
   keyboard whose keys are not readable on the watched descriptor (X11). */
#define AI_HTTP_WATCH_SLICE_MS  100
#define AI_HTTP_MS_PER_S        1000L
#define AI_HTTP_NS_PER_MS       1000000L
/* Whether an open goes on to the TLS handshake of an https URL. */
#define AI_HTTP_WITH_TLS        1
#define AI_HTTP_WITHOUT_TLS     0

long wpe_http_now_ms(void)
{
 struct timespec ts;
 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * AI_HTTP_MS_PER_S + ts.tv_nsec / AI_HTTP_NS_PER_MS;
}

void wpe_http_describe_timeout(const wpe_http_conn *c, int deadline_ms,
                               char *errbuf, size_t errsz)
{
 if (errbuf && errsz)
  snprintf(errbuf, errsz, "no answer from %s:%d within %ld s",
           c->host, c->port, (long)deadline_ms / AI_HTTP_MS_PER_S);
}

/**
 * ai_http_set_nonblocking - Make a socket return at once instead of waiting.
 * @fd: the socket.
 *
 * Every socket of the assistant is non-blocking from birth, so no connect,
 * handshake or read can hold the editor; the waits happen in poll().
 */
static void ai_http_set_nonblocking(int fd)
{
 int fl = fcntl(fd, F_GETFL, 0);
 if (fl != -1) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/**
 * ai_http_free_addrs - Let go of the addresses an open still had to try.
 * @c: the connection.
 */
static void ai_http_free_addrs(wpe_http_conn *c)
{
 if (c->addrs) freeaddrinfo((struct addrinfo *)c->addrs);
 c->addrs = NULL;
 c->addr_next = NULL;
}

/**
 * ai_http_resolve - Turn the endpoint's host into the addresses to try.
 * @c: the connection, host and port filled in.
 * @errbuf: the reason on failure; @errsz: its size.
 *
 * A numeric host or "localhost" resolves at once; a name waits for the system
 * resolver, which has its own timeout.  The user reads which host could not
 * be found.
 *
 * Return: 0, or -1 when the host does not resolve.
 */
static int ai_http_resolve(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 struct addrinfo hints, *res = NULL;
 char portstr[16];
 memset(&hints, 0, sizeof hints);
 hints.ai_family   = AF_UNSPEC;
 hints.ai_socktype = SOCK_STREAM;
 snprintf(portstr, sizeof portstr, "%d", c->port);
 if (getaddrinfo(c->host, portstr, &hints, &res) != 0) {
  if (errbuf && errsz)
   snprintf(errbuf, errsz, "cannot resolve host %s", c->host);
  return -1;
 }
 c->addrs = res;
 c->addr_next = res;
 return 0;
}

/**
 * ai_http_unreachable - Say that no address of the endpoint took the connection.
 * @c: the connection; @errbuf: destination; @errsz: its size.
 *
 * The real reason and where: "Connection refused" means nothing listens on
 * host:port (the local server or bridge is down); other reasons (no route,
 * timed out) point elsewhere.  The address makes it obvious which endpoint.
 */
static void ai_http_unreachable(const wpe_http_conn *c, char *errbuf, size_t errsz)
{
 if (errbuf && errsz)
  snprintf(errbuf, errsz, "cannot connect to %s:%d (%s)",
           c->host, c->port, strerror(c->last_errno));
}

/**
 * ai_http_place_socket - Put a new socket on the descriptor number of the old one.
 * @c: the connection; @fd: the new socket.
 *
 * When the first address of a name refuses (localhost's ::1 while the server
 * listens on 127.0.0.1 only), the next address gets a fresh socket.  The
 * editor's event loop already watches the first descriptor number, so the
 * new socket takes that number and the loop keeps watching the right thing.
 *
 * Return: 0, or -1 when the descriptor could not be placed.
 */
static int ai_http_place_socket(wpe_http_conn *c, int fd)
{
 if (c->fd < 0) {
  c->fd = fd;
  return 0;
 }
 if (dup2(fd, c->fd) < 0) {
  close(fd);
  return -1;
 }
 close(fd);
 return 0;
}

static int ai_http_connected(wpe_http_conn *c, char *errbuf, size_t errsz);

/**
 * ai_http_connect_next - Start the connection to the next address of the endpoint.
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * Return: WPE_HTTP_OPEN_READY or PENDING as the connect went, FAILED when no
 * address is left to try.
 */
static int ai_http_connect_next(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 while (c->addr_next) {
  struct addrinfo *ai = c->addr_next;
  int fd;
  c->addr_next = ai->ai_next;
  fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (fd < 0) { c->last_errno = errno; continue; }
  ai_http_set_nonblocking(fd);
  if (ai_http_place_socket(c, fd) < 0) { c->last_errno = errno; continue; }
  if (connect(c->fd, ai->ai_addr, ai->ai_addrlen) == 0)
   return ai_http_connected(c, errbuf, errsz);
  if (errno == EINPROGRESS) {
   c->phase = WPE_HTTP_PHASE_CONNECT;
   c->want = POLLOUT;
   return WPE_HTTP_OPEN_PENDING;
  }
  c->last_errno = errno;
 }
 ai_http_unreachable(c, errbuf, errsz);
 return WPE_HTTP_OPEN_FAILED;
}

/**
 * ai_http_connect_check - The socket woke while connecting: did it connect?
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * A refused address moves on to the next one; a wake-up before the connect
 * has settled keeps waiting.
 *
 * Return: WPE_HTTP_OPEN_*.
 */
static int ai_http_connect_check(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 int soerr = 0;
 socklen_t len = sizeof soerr;
 struct sockaddr_storage peer;
 socklen_t plen = sizeof peer;
 if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0)
  soerr = errno;
 if (soerr != 0) {
  c->last_errno = soerr;
  return ai_http_connect_next(c, errbuf, errsz);
 }
 if (getpeername(c->fd, (struct sockaddr *)&peer, &plen) < 0)
  return WPE_HTTP_OPEN_PENDING;            /* not settled yet: keep waiting */
 return ai_http_connected(c, errbuf, errsz);
}

#if defined(WPE_AI_TLS) && defined(WPE_AI_TLS_OPENSSL)
/**
 * ai_http_tls_failure - Say why the handshake failed, and with whom.
 * @c: the connection; @errbuf: destination; @errsz: its size.
 *
 * The user reads host:port and the library's reason; only a certificate the
 * editor could not verify adds where to give it the server's CA file, since
 * that advice is wrong for a server that hung up or speaks no TLS.
 */
static void ai_http_tls_failure(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 unsigned long top = ERR_peek_error();
 const char *reason = top ? ERR_reason_error_string(top) : NULL;
 long vr = SSL_get_verify_result((SSL *)c->tls);
 int verify = top && ERR_GET_REASON(top) == SSL_R_CERTIFICATE_VERIFY_FAILED;
 if (!reason)
  reason = AI_HTTP_TLS_CLOSED;
 if (!errbuf || !errsz) {
  ERR_clear_error();
  return;
 }
 if (verify && vr != X509_V_OK)
  snprintf(errbuf, errsz, "%s with %s:%d: %s (%s)%s", AI_HTTP_TLS_FAILED,
           c->host, c->port, reason, X509_verify_cert_error_string(vr),
           AI_HTTP_TLS_CA_ADVICE);
 else if (verify)
  snprintf(errbuf, errsz, "%s with %s:%d: %s%s", AI_HTTP_TLS_FAILED,
           c->host, c->port, reason, AI_HTTP_TLS_CA_ADVICE);
 else
  snprintf(errbuf, errsz, "%s with %s:%d: %s", AI_HTTP_TLS_FAILED,
           c->host, c->port, reason);
 ERR_clear_error();
}

/**
 * ai_http_tls_begin - Wrap the connected socket for the handshake.
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * Return: 0, or -1 when the library could not set up.
 */
static int ai_http_tls_begin(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 SSL_CTX *ctx = ai_ssl_ctx();
 SSL *ssl;
 if (!ctx) { ai_http_seterr(errbuf, errsz, "TLS init failed"); return -1; }
 ssl = SSL_new(ctx);
 if (!ssl) { ai_http_seterr(errbuf, errsz, "TLS alloc failed"); return -1; }
 SSL_set_fd(ssl, c->fd);
 SSL_set_tlsext_host_name(ssl, c->host);   /* SNI */
 SSL_set1_host(ssl, c->host);              /* verify the cert matches the host */
 ERR_clear_error();
 c->tls = ssl;
 c->phase = WPE_HTTP_PHASE_TLS;
 return 0;
}

/**
 * ai_http_tls_step - Advance the handshake as far as the socket allows now.
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * Return: WPE_HTTP_OPEN_*; on PENDING c->want says what to wait for.
 */
static int ai_http_tls_step(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 int r = SSL_connect((SSL *)c->tls), e;
 if (r == 1) {
  c->is_tls = 1;
  c->phase = WPE_HTTP_PHASE_READY;
  c->want = POLLIN;
  return WPE_HTTP_OPEN_READY;
 }
 e = SSL_get_error((SSL *)c->tls, r);
 if (e == SSL_ERROR_WANT_READ) { c->want = POLLIN; return WPE_HTTP_OPEN_PENDING; }
 if (e == SSL_ERROR_WANT_WRITE) { c->want = POLLOUT; return WPE_HTTP_OPEN_PENDING; }
 ai_http_tls_failure(c, errbuf, errsz);
 return WPE_HTTP_OPEN_FAILED;
}
#elif defined(WPE_AI_TLS)
/**
 * ai_http_tls_failure - Say why the handshake failed, and with whom.
 * @c: the connection; @errbuf: destination; @errsz: its size.
 *
 * As the OpenSSL branch: host:port, the library's reason, and the CA advice
 * only for a certificate that could not be verified.
 */
static void ai_http_tls_failure(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 const char *reason = c->tls ? tls_error(c->tls) : NULL;
 int verify;
 if (!reason) reason = AI_HTTP_TLS_CLOSED;
 verify = strstr(reason, "verify") != NULL || strstr(reason, "certificate") != NULL;
 if (!errbuf || !errsz) return;
 if (verify)
  snprintf(errbuf, errsz, "%s with %s:%d: %s%s", AI_HTTP_TLS_FAILED,
           c->host, c->port, reason, AI_HTTP_TLS_CA_ADVICE);
 else
  snprintf(errbuf, errsz, "%s with %s:%d: %s", AI_HTTP_TLS_FAILED,
           c->host, c->port, reason);
}

/**
 * ai_http_tls_begin - Wrap the connected socket for the handshake.
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * Trusts an extra CA / self-signed cert (a local HTTPS bridge) in addition to
 * the system store; verification, hostname included, still runs.
 *
 * Return: 0, or -1 when the library could not set up.
 */
static int ai_http_tls_begin(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 struct tls_config *cfg = tls_config_new();
 if (cfg && e_ai_cafile && *e_ai_cafile)
  tls_config_set_ca_file(cfg, e_ai_cafile);
 c->tls = tls_client();
 if (!c->tls || !cfg || tls_configure(c->tls, cfg) < 0 ||
     tls_connect_socket(c->tls, c->fd, c->host) < 0) {
  ai_http_seterr(errbuf, errsz, "TLS setup failed");
  if (cfg) tls_config_free(cfg);
  return -1;
 }
 tls_config_free(cfg);
 c->phase = WPE_HTTP_PHASE_TLS;
 return 0;
}

/**
 * ai_http_tls_step - Advance the handshake as far as the socket allows now.
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * Return: WPE_HTTP_OPEN_*; on PENDING c->want says what to wait for.
 */
static int ai_http_tls_step(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 int r = tls_handshake(c->tls);
 if (r == 0) {
  c->is_tls = 1;
  c->phase = WPE_HTTP_PHASE_READY;
  c->want = POLLIN;
  return WPE_HTTP_OPEN_READY;
 }
 if (r == TLS_WANT_POLLIN) { c->want = POLLIN; return WPE_HTTP_OPEN_PENDING; }
 if (r == TLS_WANT_POLLOUT) { c->want = POLLOUT; return WPE_HTTP_OPEN_PENDING; }
 ai_http_tls_failure(c, errbuf, errsz);
 return WPE_HTTP_OPEN_FAILED;
}
#else
static int ai_http_tls_begin(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 (void)c;
 ai_http_seterr(errbuf, errsz, "https needs --enable-ai-tls");
 return -1;
}

static int ai_http_tls_step(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 (void)c;
 ai_http_seterr(errbuf, errsz, "https needs --enable-ai-tls");
 return WPE_HTTP_OPEN_FAILED;
}
#endif

/**
 * ai_http_connected - The TCP connection stands: start TLS or be done.
 * @c: the connection; @errbuf: the reason on failure; @errsz: its size.
 *
 * Return: WPE_HTTP_OPEN_*.
 */
static int ai_http_connected(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 ai_http_free_addrs(c);
 if (!c->use_tls) {
  c->phase = WPE_HTTP_PHASE_READY;
  c->want = POLLIN;
  return WPE_HTTP_OPEN_READY;
 }
 if (ai_http_tls_begin(c, errbuf, errsz) < 0)
  return WPE_HTTP_OPEN_FAILED;
 return ai_http_tls_step(c, errbuf, errsz);
}

/**
 * ai_http_begin - Parse the URL, resolve the host and start the connect.
 * @url: the endpoint; @c: the connection to fill; @tls: AI_HTTP_WITH_TLS to
 * go on to the handshake of an https URL; @errbuf, @errsz: the reason.
 *
 * Return: WPE_HTTP_OPEN_*.
 */
static int ai_http_begin(const char *url, wpe_http_conn *c, int tls,
                         char *errbuf, size_t errsz)
{
 int https = 0, port = 0;
 char host[256], path[512];   /* the endpoint path is not used for the connection
                                 (only host/port are), but it must be big enough
                                 to hold a prefixed base URL, e.g. /openai/v1,
                                 or the parse rejects the endpoint as malformed */
 memset(c, 0, sizeof *c);
 c->fd = -1;
 c->last_errno = ECONNREFUSED;
 if (wpe_http_parse_url(url, host, sizeof host, &port, &https, path, sizeof path)) {
  ai_http_seterr(errbuf, errsz, "malformed endpoint URL");
  return WPE_HTTP_OPEN_FAILED;
 }
 strncpy(c->host, host, sizeof c->host - 1);
 c->port = port;
 c->use_tls = https && tls == AI_HTTP_WITH_TLS;
 if (ai_http_resolve(c, errbuf, errsz) < 0)
  return WPE_HTTP_OPEN_FAILED;
 return ai_http_connect_next(c, errbuf, errsz);
}

int wpe_http_open_begin(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz)
{
 return ai_http_begin(url, c, AI_HTTP_WITH_TLS, errbuf, errsz);
}

int wpe_http_open_step(wpe_http_conn *c, char *errbuf, size_t errsz)
{
 if (c->phase == WPE_HTTP_PHASE_CONNECT)
  return ai_http_connect_check(c, errbuf, errsz);
 if (c->phase == WPE_HTTP_PHASE_TLS)
  return ai_http_tls_step(c, errbuf, errsz);
 return WPE_HTTP_OPEN_READY;
}

int wpe_http_open_events(const wpe_http_conn *c)
{
 return c->want;
}

/**
 * ai_http_watch_slice - How long one poll may sleep before asking the watch.
 * @w: the watch, or NULL; @left: the milliseconds to the deadline.
 *
 * Return: @left, cut to AI_HTTP_WATCH_SLICE_MS when someone must be asked.
 */
static long ai_http_watch_slice(const wpe_http_watch *w, long left)
{
 if (w && w->cancelled && left > AI_HTTP_WATCH_SLICE_MS)
  return AI_HTTP_WATCH_SLICE_MS;
 return left;
}

int wpe_http_wait(int fd, int events, long deadline, const wpe_http_watch *w)
{
 struct pollfd pf[2];
 int n = 1, pr;
 long left = deadline - wpe_http_now_ms();
 if (left <= 0) return WPE_HTTP_WAIT_TIMEOUT;
 pf[0].fd = fd; pf[0].events = (short)events; pf[0].revents = 0;
 if (w && w->fd >= 0) {
  pf[1].fd = w->fd; pf[1].events = POLLIN; pf[1].revents = 0;
  n = 2;
 }
 pr = poll(pf, (nfds_t)n, (int)ai_http_watch_slice(w, left));
 if (w && w->cancelled && w->cancelled(w->ud))
  return WPE_HTTP_WAIT_CANCELLED;
 if (pr < 0 && errno != EINTR)
  return WPE_HTTP_WAIT_READY;              /* let the caller's step see the error */
 if (pr > 0 && pf[0].revents)
  return WPE_HTTP_WAIT_READY;
 return WPE_HTTP_WAIT_IDLE;
}

/**
 * ai_http_wait_open - Drive an open to its end, no later than the deadline.
 * @c: the connection; @rc: what the begin returned; @deadline: absolute ms;
 * @deadline_ms: the same as a length, for the message; @w: the watch, or NULL;
 * @errbuf, @errsz: the reason on failure.
 *
 * Return: WPE_HTTP_OPEN_READY or WPE_HTTP_OPEN_FAILED.
 */
static int ai_http_wait_open(wpe_http_conn *c, int rc, long deadline, int deadline_ms,
                             const wpe_http_watch *w, char *errbuf, size_t errsz)
{
 while (rc == WPE_HTTP_OPEN_PENDING) {
  int seen = wpe_http_wait(c->fd, c->want, deadline, w);
  if (seen == WPE_HTTP_WAIT_TIMEOUT) {
   wpe_http_describe_timeout(c, deadline_ms, errbuf, errsz);
   return WPE_HTTP_OPEN_FAILED;
  }
  if (seen == WPE_HTTP_WAIT_CANCELLED) {
   ai_http_seterr(errbuf, errsz, "cancelled");
   return WPE_HTTP_OPEN_FAILED;
  }
  if (seen == WPE_HTTP_WAIT_READY)
   rc = wpe_http_open_step(c, errbuf, errsz);
 }
 return rc;
}

int wpe_http_open(const char *url, wpe_http_conn *c, int deadline_ms,
                  const wpe_http_watch *watch, char *errbuf, size_t errsz)
{
 long deadline = wpe_http_now_ms() + deadline_ms;
 int rc = ai_http_begin(url, c, AI_HTTP_WITH_TLS, errbuf, errsz);
 rc = ai_http_wait_open(c, rc, deadline, deadline_ms, watch, errbuf, errsz);
 if (rc != WPE_HTTP_OPEN_READY) {
  wpe_http_close(c);
  return -1;
 }
 return 0;
}

int wpe_http_reach(const char *url, int deadline_ms, char *errbuf, size_t errsz)
{
 wpe_http_conn c;
 long deadline = wpe_http_now_ms() + deadline_ms;
 int rc = ai_http_begin(url, &c, AI_HTTP_WITHOUT_TLS, errbuf, errsz);
 rc = ai_http_wait_open(&c, rc, deadline, deadline_ms, NULL, errbuf, errsz);
 wpe_http_close(&c);
 return rc == WPE_HTTP_OPEN_READY ? 0 : -1;
}

void wpe_http_close(wpe_http_conn *c)
{
 if (!c) return;
 ai_http_free_addrs(c);
#ifdef WPE_AI_TLS
 if (c->tls) {
# ifdef WPE_AI_TLS_OPENSSL
  if (c->is_tls)                           /* only a finished handshake says goodbye */
   SSL_shutdown((SSL *)c->tls);
  SSL_free((SSL *)c->tls);
  ERR_clear_error();
# else
  tls_close(c->tls);
  tls_free(c->tls);
# endif
  c->tls = NULL;
 }
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
# ifdef WPE_AI_TLS_OPENSSL
  int ret = SSL_read((SSL *)c->tls, buf, (int)n);
  if (ret > 0) return ret;
  {
   int e = SSL_get_error((SSL *)c->tls, ret);
   if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) return 0;
   return -1;                             /* clean close or error => EOF */
  }
# else
  r = tls_read(c->tls, buf, n);
  if (r == TLS_WANT_POLLIN || r == TLS_WANT_POLLOUT) return 0;
  if (r <= 0) return -1;
  return r;
# endif
 }
#endif
 r = read(c->fd, buf, n);
 if (r > 0) return r;
 if (r == 0) return -1;                 /* EOF */
 if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
 return -1;
}

/**
 * ai_http_await - Wait for a stalled socket to take or give bytes again.
 * @fd: the socket; @events: POLLOUT for a full send buffer, POLLIN when TLS
 * must read before it can write.
 *
 * A request is small and the server reads it at once, so this is normally
 * never reached; a server that stops reading ends the send after
 * AI_HTTP_SEND_STALL_MS instead of holding the editor.
 *
 * Return: 1 when the socket is ready, 0 when it stayed stalled.
 */
static int ai_http_await(int fd, int events)
{
 long deadline = wpe_http_now_ms() + AI_HTTP_SEND_STALL_MS;
 int seen;
 do
  seen = wpe_http_wait(fd, events, deadline, NULL);
 while (seen == WPE_HTTP_WAIT_IDLE);
 return seen == WPE_HTTP_WAIT_READY;
}

ssize_t wpe_http_write_all(wpe_http_conn *c, const void *buf, size_t n)
{
 size_t off = 0;
 const char *p = buf;
 while (off < n) {
#ifdef WPE_AI_TLS
  if (c->is_tls) {
# ifdef WPE_AI_TLS_OPENSSL
   int w = SSL_write((SSL *)c->tls, p + off, (int)(n - off));
   if (w > 0) { off += (size_t)w; continue; }
   {
    int e = SSL_get_error((SSL *)c->tls, w);
    if (e == SSL_ERROR_WANT_WRITE && ai_http_await(c->fd, POLLOUT)) continue;
    if (e == SSL_ERROR_WANT_READ && ai_http_await(c->fd, POLLIN)) continue;
    return -1;
   }
# else
   ssize_t w = tls_write(c->tls, p + off, n - off);
   if (w == TLS_WANT_POLLOUT && ai_http_await(c->fd, POLLOUT)) continue;
   if (w == TLS_WANT_POLLIN && ai_http_await(c->fd, POLLIN)) continue;
   if (w < 0) return -1;
   off += (size_t)w;
   continue;
# endif
  }
#endif
  {
   ssize_t w = write(c->fd, p + off, n - off);
   if (w >= 0) { off += (size_t)w; continue; }
   if (errno == EINTR) continue;
   if ((errno == EAGAIN || errno == EWOULDBLOCK) && ai_http_await(c->fd, POLLOUT))
    continue;
   return -1;
  }
 }
 return (ssize_t)off;
}

int wpe_http_request(wpe_http_conn *c, const char *method, const char *path,
                     const char *const *headers, const char *body)
{
 char hdr[8192];
 size_t off = 0, blen = body ? strlen(body) : 0;
 int i;

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
 /* The socket has been non-blocking since the open, so the streaming reads
    already fit the editor's fd-loop. */
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

#define AI_HTTP_STATUS_PREFIX "HTTP/"

/**
 * ai_parse_status_line - Read the status code off a line that is an HTTP status line.
 * @s: the stream; @n: the length of the header block.
 *
 * Only a line that opens with "HTTP/" carries a status.  Bytes that merely
 * happen to hold a blank and digits, as a server answering something that is
 * not HTTP can send, leave the status at 0, so the caller reads "no status
 * line" and reports a transport error instead of a reply.
 */
static void ai_parse_status_line(wpe_http_stream *s, size_t n)
{
 const char *h = s->raw;
 size_t plen = strlen(AI_HTTP_STATUS_PREFIX), j;
 if (n < plen || strncmp(h, AI_HTTP_STATUS_PREFIX, plen) != 0) return;
 for (j = plen; j < n && h[j] != ' '; j++) ;
 if (j < n) s->status = atoi(h + j + 1);
}

/* Parse the HTTP status line + headers.  Scans by index over raw[0..hdr_end)
 * without mutating the buffer, so the trailing body bytes stay intact. */
static void ai_parse_headers(wpe_http_stream *s, size_t hdr_end)
{
 char *h = s->raw;
 size_t i, n = hdr_end;

 ai_parse_status_line(s, n);

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

/**
 * ai_body_ends_by_framing - Record that the HTTP framing closed the body.
 * @s: the stream whose Content-Length was reached or final chunk was read.
 *
 * Both marks are set: body_complete, which the line reader uses to hand back
 * a final unterminated line, and framed_end, which tells a complete answer
 * from a socket that merely closed, so a reply cut by a dying connection is
 * reported to the user as a transport error and never as a finished turn.
 */
static void ai_body_ends_by_framing(wpe_http_stream *s)
{
 s->body_complete = 1;
 s->framed_end = 1;
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
    ai_body_ends_by_framing(s);
   continue;
  }

  /* chunked: need a size line first */
  if (s->chunk_left < 0) {
   const char *eol = memchr(src, '\n', avail);
   long sz;
   if (!eol) return 0;                  /* wait for the full size line */
   sz = strtol(src, NULL, 16);
   s->raw_consumed += (size_t)(eol - src) + 1;
   if (sz == 0) { ai_body_ends_by_framing(s); return 0; }
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

/**
 * wpe_http_stream_status_reason - The words after the code on the status line.
 * @s: the response; @out: destination; @sz: its size.
 *
 * When a server refuses with no JSON reason, the user still reads what its
 * status line said ("Unauthorized"), not only a number.
 */
void wpe_http_stream_status_reason(const wpe_http_stream *s, char *out, size_t sz)
{
 const char *p, *end, *lim;
 size_t n;
 if (!out || !sz) return;
 out[0] = '\0';
 if (!s->header_done || !s->raw) return;
 lim = s->raw + s->raw_len;
 end = memchr(s->raw, '\n', s->raw_len);
 if (!end) end = lim;
 p = memchr(s->raw, ' ', (size_t)(end - s->raw));          /* after "HTTP/1.1" */
 if (p) p = memchr(p + 1, ' ', (size_t)(end - p - 1));     /* after the code   */
 if (!p) return;
 p++;
 while (end > p && (end[-1] == '\r' || end[-1] == '\n')) end--;
 n = (size_t)(end - p);
 if (n >= sz) n = sz - 1;
 memcpy(out, p, n);
 out[n] = '\0';
}
int  wpe_http_stream_done(const wpe_http_stream *s) { return s->body_complete; }
int  wpe_http_stream_framed(const wpe_http_stream *s) { return s->framed_end; }

#endif /* WPE_AI */

typedef int wpe_ai_http_translation_unit;
