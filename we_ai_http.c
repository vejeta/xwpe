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
#include "we_fdloop.h"   /* the GET is driven by the loop's descriptors and timers */

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

/* ----- the non-blocking open -------------------------------------------
 * The editor never waits inside connect() or the TLS handshake: the socket is
 * non-blocking from its creation, and the caller's event loop (the chat, the
 * Edit, a GET) drives the open one step per wake-up with wpe_http_open_begin /
 * wpe_http_open_step; the deadline is the caller's timer.  Name
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

int wpe_http_connect_begin(const char *url, wpe_http_conn *c, char *errbuf, size_t errsz)
{
 return ai_http_begin(url, c, AI_HTTP_WITHOUT_TLS, errbuf, errsz);
}

/* ----- the reachability check, on the loop ------------------------------ */

/* Room for the reason a reachability check failed. */
#define AI_HTTP_REACH_WHY_MAX 400

struct wpe_http_reach {
 wpe_http_conn       conn;
 int                 deadline_ms;
 int                 timer;
 wpe_http_reached_fn done;
 void               *ud;
};

/**
 * ai_http_reach_free - Close the check's socket and leave the loop.
 * @r: the check.
 */
static void ai_http_reach_free(wpe_http_reach *r)
{
 if (r->conn.fd >= 0)
  wpe_fd_del(r->conn.fd);
 wpe_timer_del(r->timer);
 wpe_http_close(&r->conn);
 free(r);
}

/**
 * ai_http_reach_end - Tell the check's owner the outcome, then free it.
 * @r: the check; @ok: 1 when something listens; @why: the reason when not.
 */
static void ai_http_reach_end(wpe_http_reach *r, int ok, const char *why)
{
 wpe_http_reached_fn done = r->done;
 void *ud = r->ud;
 char line[AI_HTTP_REACH_WHY_MAX];
 snprintf(line, sizeof line, "%s", why ? why : "");
 ai_http_reach_free(r);
 done(ok, line, ud);
}

/**
 * ai_http_reach_on_fd - The connect settled (or woke): tell the owner.
 * @fd: the socket; @data: the check.
 */
static void ai_http_reach_on_fd(int fd, void *data)
{
 wpe_http_reach *r = data;
 char why[AI_HTTP_REACH_WHY_MAX];
 int rc;
 (void)fd;
 why[0] = '\0';
 rc = wpe_http_open_step(&r->conn, why, sizeof why);
 if (rc == WPE_HTTP_OPEN_PENDING) {
  wpe_fd_add(r->conn.fd, r->conn.want, ai_http_reach_on_fd, r);
  return;
 }
 ai_http_reach_end(r, rc == WPE_HTTP_OPEN_READY, why);
}

/**
 * ai_http_reach_on_deadline - Nothing answered the connect in time.
 * @data: the check.
 */
static void ai_http_reach_on_deadline(void *data)
{
 wpe_http_reach *r = data;
 char why[AI_HTTP_REACH_WHY_MAX];
 r->timer = 0;
 wpe_http_describe_timeout(&r->conn, r->deadline_ms, why, sizeof why);
 ai_http_reach_end(r, 0, why);
}

int wpe_http_reach_start(const char *url, int deadline_ms, wpe_http_reached_fn done,
                         void *ud, wpe_http_reach **out, char *errbuf, size_t errsz)
{
 wpe_http_reach *r = calloc(1, sizeof *r);
 int rc;
 *out = NULL;
 if (!r) { ai_http_seterr(errbuf, errsz, "out of memory"); return WPE_HTTP_OPEN_FAILED; }
 rc = ai_http_begin(url, &r->conn, AI_HTTP_WITHOUT_TLS, errbuf, errsz);
 if (rc != WPE_HTTP_OPEN_PENDING) {
  wpe_http_close(&r->conn);
  free(r);
  return rc;
 }
 r->deadline_ms = deadline_ms;
 r->done = done;
 r->ud = ud;
 if (wpe_fd_add(r->conn.fd, r->conn.want, ai_http_reach_on_fd, r) < 0) {
  if (errbuf && errsz) wpe_fd_refusal("the connection", r->conn.fd, errbuf, errsz);
  ai_http_reach_free(r);
  return WPE_HTTP_OPEN_FAILED;
 }
 r->timer = wpe_timer_add(deadline_ms, 0, ai_http_reach_on_deadline, r);
 if (r->timer <= WPE_TIMER_NONE) {
  r->timer = WPE_TIMER_NONE;
  if (errbuf && errsz) wpe_fd_refusal("the connection", r->conn.fd, errbuf, errsz);
  ai_http_reach_free(r);
  return WPE_HTTP_OPEN_FAILED;
 }
 *out = r;
 return WPE_HTTP_OPEN_PENDING;
}

void wpe_http_reach_cancel(wpe_http_reach *r)
{
 if (r)
  ai_http_reach_free(r);
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

void wpe_http_close(wpe_http_conn *c)
{
 if (!c) return;
 ai_http_free_addrs(c);
 free(c->out);
 c->out = NULL;
 c->out_len = c->out_off = 0;
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

/* ----- the send, one wake-up at a time ------------------------------------
 * The whole request is built once into the connection and written from the
 * caller's loop as the socket takes it.  A TLS write that could not finish is
 * retried with the same buffer and length (both libraries require that),
 * which the buffer held here guarantees; no write ever waits. */

/* Room for the request line and the headers of any request the assistant sends. */
#define AI_HTTP_HEADER_MAX 8192

/**
 * ai_http_header_text - Write the request line and the headers into @hdr.
 * @c: the connection (Host); @method, @path: the request; @headers: the
 * caller's extra headers, NULL-terminated, or NULL; @body: the body or NULL.
 *
 * Return: the header's length, or 0 when it does not fit.
 */
static size_t ai_http_header_text(char *hdr, size_t sz, const wpe_http_conn *c,
                                  const char *method, const char *path,
                                  const char *const *headers, const char *body)
{
 size_t off = 0;
 int i;
 off += snprintf(hdr + off, sz - off, "%s %s HTTP/1.1\r\n",
                 method, (path && *path) ? path : "/");
 off += snprintf(hdr + off, sz - off, "Host: %s:%d\r\n", c->host, c->port);
 off += snprintf(hdr + off, sz - off, "User-Agent: xwpe\r\n");
 off += snprintf(hdr + off, sz - off, "Accept: */*\r\n");
 off += snprintf(hdr + off, sz - off, "Connection: close\r\n");
 for (i = 0; headers && headers[i] && off < sz; i++)
  off += snprintf(hdr + off, sz - off, "%s\r\n", headers[i]);
 if (body && off < sz)
  off += snprintf(hdr + off, sz - off, "Content-Length: %zu\r\n", strlen(body));
 if (off < sz)
  off += snprintf(hdr + off, sz - off, "\r\n");
 if (off >= sz)
  return 0;
 return off;
}

int wpe_http_send_begin(wpe_http_conn *c, const char *method, const char *path,
                        const char *const *headers, const char *body)
{
 char hdr[AI_HTTP_HEADER_MAX];
 size_t hlen = ai_http_header_text(hdr, sizeof hdr, c, method, path, headers, body);
 size_t blen = body ? strlen(body) : 0;
 if (hlen == 0)
  return -1;
 free(c->out);
 c->out = malloc(hlen + blen + 1);
 if (!c->out)
  return -1;
 memcpy(c->out, hdr, hlen);
 if (blen)
  memcpy(c->out + hlen, body, blen);
 c->out_len = hlen + blen;
 c->out_off = 0;
 c->out_moved = wpe_http_now_ms();
 c->want = POLLOUT;
 return 0;
}

/* What one write attempt of the send did. */
#define AI_HTTP_WROTE_FAILED  (-1)
#define AI_HTTP_WROTE_WAIT_OUT  0     /* the socket is full: wait until it is writable */
#define AI_HTTP_WROTE_WAIT_IN   (-2)  /* TLS must read before it can write */

/**
 * ai_http_write_once - One write of what is left of the request.
 * @c: the connection.
 *
 * Return: the bytes taken, or AI_HTTP_WROTE_*.
 */
static ssize_t ai_http_write_once(wpe_http_conn *c)
{
 const char *p = c->out + c->out_off;
 size_t n = c->out_len - c->out_off;
 ssize_t w;
#ifdef WPE_AI_TLS
 if (c->is_tls) {
# ifdef WPE_AI_TLS_OPENSSL
  int r = SSL_write((SSL *)c->tls, p, (int)n);
  int e;
  if (r > 0) return r;
  e = SSL_get_error((SSL *)c->tls, r);
  if (e == SSL_ERROR_WANT_WRITE) return AI_HTTP_WROTE_WAIT_OUT;
  if (e == SSL_ERROR_WANT_READ) return AI_HTTP_WROTE_WAIT_IN;
  return AI_HTTP_WROTE_FAILED;
# else
  w = tls_write(c->tls, p, n);
  if (w == TLS_WANT_POLLOUT) return AI_HTTP_WROTE_WAIT_OUT;
  if (w == TLS_WANT_POLLIN) return AI_HTTP_WROTE_WAIT_IN;
  if (w < 0) return AI_HTTP_WROTE_FAILED;
  return w;
# endif
 }
#endif
 w = write(c->fd, p, n);
 if (w > 0) return w;
 if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
  return AI_HTTP_WROTE_WAIT_OUT;
 return AI_HTTP_WROTE_FAILED;
}

int wpe_http_send_step(wpe_http_conn *c)
{
 while (c->out && c->out_off < c->out_len) {
  ssize_t w = ai_http_write_once(c);
  if (w == AI_HTTP_WROTE_FAILED)
   return WPE_HTTP_SEND_FAILED;
  if (w == AI_HTTP_WROTE_WAIT_OUT) {
   c->want = POLLOUT;
   return WPE_HTTP_SEND_PENDING;
  }
  if (w == AI_HTTP_WROTE_WAIT_IN) {
   c->want = POLLIN;
   return WPE_HTTP_SEND_PENDING;
  }
  c->out_off += (size_t)w;
  c->out_moved = wpe_http_now_ms();
 }
 free(c->out);
 c->out = NULL;
 c->out_len = c->out_off = 0;
 c->want = POLLIN;
 return WPE_HTTP_SEND_DONE;
}

int wpe_http_send_stalled(const wpe_http_conn *c, long now_ms)
{
 if (!c->out)
  return 0;
 return now_ms - c->out_moved >= WPE_HTTP_SEND_STALL_MS;
}

void wpe_http_describe_stall(const wpe_http_conn *c, char *errbuf, size_t errsz)
{
 if (errbuf && errsz)
  snprintf(errbuf, errsz, "%s:%d stopped reading the request (nothing sent for %d s)",
           c->host, c->port, WPE_HTTP_SEND_STALL_MS / (int)AI_HTTP_MS_PER_S);
}

/* ----- one whole GET on the editor's loop --------------------------------- */

/* How much the GET reads in one go. */
#define AI_HTTP_GET_CHUNK 4096
/* Room for the reason a GET failed. */
#define AI_HTTP_GET_WHY_MAX 400
/* The status of an answer whose status line never came: the server closed
   the connection without answering, which the owner reads as "no response". */
#define AI_HTTP_NO_STATUS 0

struct wpe_http_get {
 wpe_http_conn   conn;
 wpe_http_stream hs;
 char           *path;
 char           *hdrs;        /* the caller's headers, copied, '\0'-separated */
 int             nhdrs;
 int             deadline_ms;
 size_t          maxbytes;
 int             timer;       /* the deadline's one-shot timer */
 int             sent;        /* the request has gone out whole */
 wpe_http_got_fn done;
 void           *ud;
 char            why[AI_HTTP_GET_WHY_MAX];
};

static void ai_http_get_on_fd(int fd, void *data);

/**
 * ai_http_get_free - Let go of everything a GET holds, from the loop too.
 * @g: the GET.
 */
static void ai_http_get_free(wpe_http_get *g)
{
 if (g->conn.fd >= 0)
  wpe_fd_del(g->conn.fd);
 wpe_timer_del(g->timer);
 wpe_http_close(&g->conn);
 wpe_http_stream_free(&g->hs);
 free(g->path);
 free(g->hdrs);
 free(g);
}

/**
 * ai_http_get_end - Tell the GET's owner how it ended, then free it.
 * @g: the GET; @how: WPE_HTTP_GOT_*.
 *
 * The GET leaves the loop before its owner is told, so whatever the owner
 * opens from its continuation (a picker, the next request) never finds
 * this socket still watched.  A deadline or a failure after part of the
 * body came hands that part over as a DONE, as an answer cut short.
 */
static void ai_http_get_end(wpe_http_get *g, int how)
{
 wpe_http_got got;
 char phrase[96];
 if (g->conn.fd >= 0)
  wpe_fd_del(g->conn.fd);
 wpe_timer_del(g->timer);
 g->timer = 0;
 if (how != WPE_HTTP_GOT_DONE && g->hs.body_len > 0)
  how = WPE_HTTP_GOT_DONE;
 phrase[0] = '\0';
 wpe_http_stream_status_reason(&g->hs, phrase, sizeof phrase);
 got.how = how;
 got.status = wpe_http_stream_status(&g->hs);
 got.body = g->hs.body ? g->hs.body : "";
 got.body_len = g->hs.body_len;
 got.phrase = phrase;
 got.why = g->why;
 g->done(&got, g->ud);
 ai_http_get_free(g);
}

/**
 * ai_http_get_fail - End the GET with a reason the owner shows.
 * @g: the GET; @how: WPE_HTTP_GOT_FAILED or _TIMED_OUT; @why: the reason.
 */
static void ai_http_get_fail(wpe_http_get *g, int how, const char *why)
{
 snprintf(g->why, sizeof g->why, "%s", why);
 ai_http_get_end(g, how);
}

/**
 * ai_http_get_on_deadline - The GET's deadline came before its answer.
 * @data: the GET.
 */
static void ai_http_get_on_deadline(void *data)
{
 wpe_http_get *g = data;
 char why[AI_HTTP_GET_WHY_MAX];
 g->timer = 0;                         /* a one-shot timer is gone once it fired */
 wpe_http_describe_timeout(&g->conn, g->deadline_ms, why, sizeof why);
 ai_http_get_fail(g, WPE_HTTP_GOT_TIMED_OUT, why);
}

/**
 * ai_http_get_headers - The caller's headers as the NULL-terminated list
 * wpe_http_send_begin takes.
 * @g: the GET; @list: room for g->nhdrs + 1 pointers.
 */
static void ai_http_get_headers(const wpe_http_get *g, const char **list)
{
 const char *p = g->hdrs;
 int i;
 for (i = 0; i < g->nhdrs; i++) {
  list[i] = p;
  p += strlen(p) + 1;
 }
 list[g->nhdrs] = NULL;
}

/**
 * ai_http_get_send - The connection stands: build the request and send it.
 * @g: the GET.  Return: WPE_HTTP_SEND_*.
 */
static int ai_http_get_send(wpe_http_get *g)
{
 const char **list = malloc(((size_t)g->nhdrs + 1) * sizeof *list);
 int rc;
 if (!list)
  return WPE_HTTP_SEND_FAILED;
 ai_http_get_headers(g, list);
 rc = wpe_http_send_begin(&g->conn, "GET", g->path, list, NULL);
 free(list);
 if (rc < 0)
  return WPE_HTTP_SEND_FAILED;
 g->sent = 1;
 return wpe_http_send_step(&g->conn);
}

/**
 * ai_http_get_read - Take what the server has sent so far.
 * @g: the GET.  Return: 1 when the answer is complete, 0 to read on.
 */
static int ai_http_get_read(wpe_http_get *g)
{
 char buf[AI_HTTP_GET_CHUNK];
 for (;;) {
  ssize_t r = wpe_http_read(&g->conn, buf, sizeof buf);
  if (r == 0)
   return wpe_http_stream_done(&g->hs);
  if (r < 0) {
   wpe_http_stream_eof(&g->hs);
   return 1;
  }
  if (wpe_http_stream_push(&g->hs, buf, (size_t)r) < 0)
   return 1;
  if (g->maxbytes && g->hs.body_len >= g->maxbytes)
   return 1;
  if (wpe_http_stream_done(&g->hs))
   return 1;
 }
}

/**
 * ai_http_get_advance - Take the GET as far as the socket allows now.
 * @g: the GET.  Return: 1 when it has ended (and is freed), 0 to wait on.
 */
static int ai_http_get_advance(wpe_http_get *g)
{
 char why[AI_HTTP_GET_WHY_MAX];
 int rc;
 why[0] = '\0';
 if (g->conn.phase != WPE_HTTP_PHASE_READY) {
  rc = wpe_http_open_step(&g->conn, why, sizeof why);
  if (rc == WPE_HTTP_OPEN_FAILED) {
   ai_http_get_fail(g, WPE_HTTP_GOT_FAILED, why[0] ? why : "connection failed");
   return 1;
  }
  if (rc == WPE_HTTP_OPEN_PENDING)
   return 0;
 }
 if (!g->sent)
  rc = ai_http_get_send(g);
 else
  rc = wpe_http_send_step(&g->conn);
 if (rc == WPE_HTTP_SEND_FAILED) {
  ai_http_get_fail(g, WPE_HTTP_GOT_FAILED, "request failed");
  return 1;
 }
 if (rc == WPE_HTTP_SEND_PENDING)
  return 0;
 if (!ai_http_get_read(g))
  return 0;
 if (wpe_http_stream_status(&g->hs) == AI_HTTP_NO_STATUS) {
  ai_http_get_fail(g, WPE_HTTP_GOT_FAILED, "no response");
  return 1;
 }
 ai_http_get_end(g, WPE_HTTP_GOT_DONE);
 return 1;
}

/**
 * ai_http_get_on_fd - The GET's socket woke: open, send or read on.
 * @fd: the socket; @data: the GET.
 */
static void ai_http_get_on_fd(int fd, void *data)
{
 wpe_http_get *g = data;
 (void)fd;
 if (ai_http_get_advance(g))
  return;
 wpe_fd_add(g->conn.fd, g->conn.want, ai_http_get_on_fd, g);
}

/**
 * ai_http_get_copy_headers - Keep the caller's headers for the send.
 * @g: the GET; @headers: NULL-terminated, or NULL.  Return: 0, -1 out of memory.
 */
static int ai_http_get_copy_headers(wpe_http_get *g, const char *const *headers)
{
 size_t need = 1, off = 0;
 int i;
 for (i = 0; headers && headers[i]; i++)
  need += strlen(headers[i]) + 1;
 g->hdrs = malloc(need);
 if (!g->hdrs)
  return -1;
 for (i = 0; headers && headers[i]; i++) {
  size_t l = strlen(headers[i]) + 1;
  memcpy(g->hdrs + off, headers[i], l);
  off += l;
 }
 g->nhdrs = i;
 return 0;
}

/**
 * ai_http_get_first_events - What the new GET's socket is first watched for.
 * @g: a GET whose connect has begun.
 *
 * A connect that is already through waits to send; one under way waits for
 * what its open step names.
 */
static int ai_http_get_first_events(const wpe_http_get *g)
{
 if (g->conn.phase == WPE_HTTP_PHASE_READY)
  return POLLOUT;
 return wpe_http_open_events(&g->conn);
}

/**
 * ai_http_get_refused - The loop has no room for the GET: say so and drop it.
 * @g: the GET; @errbuf, @errsz: the line the owner shows.
 *
 * Without a watch or a deadline the answer would never be read and the
 * owner's waiting line would stand for ever; the owner reports this at once
 * instead, the way it reports a refused connect.
 */
static void ai_http_get_refused(wpe_http_get *g, char *errbuf, size_t errsz)
{
 if (errbuf && errsz)
  wpe_fd_refusal("the answer", g->conn.fd, errbuf, errsz);
 ai_http_get_free(g);
}

wpe_http_get *wpe_http_get_start(const char *url, const char *path,
                                 const char *const *headers, int deadline_ms,
                                 size_t maxbytes, wpe_http_got_fn done, void *ud,
                                 char *errbuf, size_t errsz)
{
 wpe_http_get *g = calloc(1, sizeof *g);
 if (!g) {
  ai_http_seterr(errbuf, errsz, "out of memory");
  return NULL;
 }
 g->conn.fd = -1;
 wpe_http_stream_init(&g->hs);
 g->deadline_ms = deadline_ms;
 g->maxbytes = maxbytes;
 g->done = done;
 g->ud = ud;
 g->path = malloc(strlen(path ? path : "") + 1);
 if (!g->path || ai_http_get_copy_headers(g, headers) < 0) {
  ai_http_seterr(errbuf, errsz, "out of memory");
  ai_http_get_free(g);
  return NULL;
 }
 strcpy(g->path, path ? path : "");
 if (wpe_http_open_begin(url, &g->conn, errbuf, errsz) == WPE_HTTP_OPEN_FAILED) {
  ai_http_get_free(g);
  return NULL;
 }
 if (wpe_fd_add(g->conn.fd, ai_http_get_first_events(g), ai_http_get_on_fd, g) < 0) {
  ai_http_get_refused(g, errbuf, errsz);
  return NULL;
 }
 g->timer = wpe_timer_add(deadline_ms, 0, ai_http_get_on_deadline, g);
 if (g->timer <= WPE_TIMER_NONE) {
  g->timer = WPE_TIMER_NONE;
  ai_http_get_refused(g, errbuf, errsz);
  return NULL;
 }
 return g;
}

void wpe_http_get_cancel(wpe_http_get *g)
{
 if (g)
  ai_http_get_free(g);
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
