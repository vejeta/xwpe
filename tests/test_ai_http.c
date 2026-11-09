/* Unit test: the AI HTTP client (we_ai_http.c).  The framer needs no socket:
 * raw response bytes are pushed and decoded body lines pulled.  The send and
 * the GET run against a socketpair and a listening socket on 127.0.0.1, the
 * GET on the editor's own loop (we_fdloop.c), which the test drives. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "we_ai_http.h"
#include "we_fdloop.h"

/* The extra CA file is owned by the configuration layer (we_ai.c), which
 * this test does not link; the framer only ever reads it when a TLS
 * connection is opened, so an empty stand-in keeps the test self-contained. */
char *e_ai_cafile = NULL;

static int fail;
static void check(int cond, const char *msg)
{
 if (!cond) { printf("FAIL: %s\n", msg); fail = 1; }
 else printf("ok: %s\n", msg);
}

/* A request body larger than any socket buffer between the two ends. */
#define BIG_BODY        (4 * 1024 * 1024)
/* The send buffer the test asks for, so the socket fills at once. */
#define SMALL_SNDBUF    4096
/* How long the loop is driven before a case gives up waiting. */
#define LOOP_LIMIT_MS   4000
#define LOOP_SLICE_MS   50
/* The GET deadline of the case that waits for it. */
#define GET_DEADLINE_MS 1000

static void nonblocking(int fd)
{
 fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

/* Read what the peer has now; returns the bytes read (appended to buf). */
static size_t drain(int fd, char *buf, size_t have, size_t cap)
{
 for (;;) {
  ssize_t r = read(fd, buf + have, cap - have);
  if (r <= 0) return have;
  have += (size_t)r;
 }
}

static void test_send_waits_for_a_full_socket_and_sends_it_all_in_order(void)
{
 int sv[2], sz = SMALL_SNDBUF, rc = WPE_HTTP_SEND_PENDING, rounds = 0, first;
 wpe_http_conn c;
 char *body = malloc(BIG_BODY + 1), *got = malloc(2 * BIG_BODY);
 const char *hdrs[] = { "X-Test: 1", NULL };
 size_t have = 0, want;
 socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
 nonblocking(sv[0]); nonblocking(sv[1]);
 setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
 memset(body, 'b', BIG_BODY); body[BIG_BODY] = '\0';
 body[BIG_BODY - 1] = 'Z';
 memset(&c, 0, sizeof c);
 c.fd = sv[0];
 strcpy(c.host, "test");
 c.port = 1;
 check(wpe_http_send_begin(&c, "POST", "/p", hdrs, body) == 0, "send: begin builds the request");
 want = c.out_len;
 first = wpe_http_send_step(&c);
 check(first == WPE_HTTP_SEND_PENDING, "send: a full socket leaves it pending");
 check(c.want == POLLOUT, "send: pending waits for the socket to be writable");
 while (rc == WPE_HTTP_SEND_PENDING && rounds++ < 100000) {
  have = drain(sv[1], got, have, 2 * BIG_BODY);
  rc = wpe_http_send_step(&c);
 }
 have = drain(sv[1], got, have, 2 * BIG_BODY);
 check(rc == WPE_HTTP_SEND_DONE, "send: done once the peer has read");
 check(have == want, "send: every byte arrived");
 check(!strncmp(got, "POST /p HTTP/1.1\r\n", 18), "send: the request line comes first");
 check(have > 0 && got[have - 1] == 'Z', "send: the body's last byte comes last");
 check(c.want == POLLIN, "send: after the send the answer is waited for");
 wpe_http_close(&c);
 close(sv[1]);
 free(body);
 free(got);
}

static void test_send_stall_is_told_after_its_bound(void)
{
 int sv[2], sz = SMALL_SNDBUF;
 wpe_http_conn c;
 char *body = malloc(BIG_BODY + 1);
 socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
 nonblocking(sv[0]);
 setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
 memset(body, 'b', BIG_BODY); body[BIG_BODY] = '\0';
 memset(&c, 0, sizeof c);
 c.fd = sv[0];
 strcpy(c.host, "test");
 c.port = 1;
 wpe_http_send_begin(&c, "POST", "/p", NULL, body);
 wpe_http_send_step(&c);
 check(!wpe_http_send_stalled(&c, c.out_moved), "stall: not before its bound");
 check(wpe_http_send_stalled(&c, c.out_moved + WPE_HTTP_SEND_STALL_MS),
       "stall: told once the socket took nothing for the bound");
 wpe_http_close(&c);
 close(sv[1]);
 free(body);
}

/* ---- a GET against a server on 127.0.0.1, on the loop ---- */

/* What the test's server does with a connection. */
enum { SRV_ANSWER, SRV_SILENT, SRV_HANGS_UP };

static struct {
 int  listen_fd, conn_fd, mode;
 char req[2048];
 size_t req_len;
} srv;

static struct {
 int  called, how, status;
 char body[256], why[400];
} got_answer;

static void on_answer(const wpe_http_got *g, void *ud)
{
 (void)ud;
 got_answer.called++;
 got_answer.how = g->how;
 got_answer.status = g->status;
 snprintf(got_answer.body, sizeof got_answer.body, "%s", g->body);
 snprintf(got_answer.why, sizeof got_answer.why, "%s", g->why ? g->why : "");
}

static void srv_on_request(int fd, void *data)
{
 static const char resp[] =
   "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
 ssize_t w;
 (void)data;
 srv.req_len = drain(fd, srv.req, srv.req_len, sizeof srv.req - 1);
 srv.req[srv.req_len] = '\0';
 if (!strstr(srv.req, "\r\n\r\n") || srv.mode == SRV_SILENT)
  return;
 wpe_fd_del(fd);
 if (srv.mode == SRV_HANGS_UP) {
  close(fd);
  srv.conn_fd = -1;
  return;
 }
 w = write(fd, resp, sizeof resp - 1);
 (void)w;
}

static void srv_on_accept(int fd, void *data)
{
 int c = accept(fd, NULL, NULL);
 (void)data;
 if (c < 0) return;
 nonblocking(c);
 srv.conn_fd = c;
 wpe_fd_add(c, POLLIN, srv_on_request, NULL);
}

static int srv_start(int mode, char *url, size_t urlsz)
{
 struct sockaddr_in a;
 socklen_t al = sizeof a;
 memset(&srv, 0, sizeof srv);
 srv.conn_fd = -1;
 srv.mode = mode;
 srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
 memset(&a, 0, sizeof a);
 a.sin_family = AF_INET;
 a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
 bind(srv.listen_fd, (struct sockaddr *)&a, sizeof a);
 listen(srv.listen_fd, 4);
 getsockname(srv.listen_fd, (struct sockaddr *)&a, &al);
 nonblocking(srv.listen_fd);
 wpe_fd_add(srv.listen_fd, POLLIN, srv_on_accept, NULL);
 snprintf(url, urlsz, "http://127.0.0.1:%d", ntohs(a.sin_port));
 return ntohs(a.sin_port);
}

static void srv_stop(void)
{
 wpe_fd_del(srv.listen_fd);
 close(srv.listen_fd);
 if (srv.conn_fd >= 0) { wpe_fd_del(srv.conn_fd); close(srv.conn_fd); }
}

/* Drive the loop until the GET's owner is told, or the limit. */
static void run_loop_until_answer(int limit_ms)
{
 int waited = 0;
 while (!got_answer.called && waited < limit_ms) {
  wpe_fd_poll(LOOP_SLICE_MS);
  waited += LOOP_SLICE_MS;
 }
}

static void test_get_reads_the_whole_answer_on_the_loop(void)
{
 char url[64], err[200];
 const char *hdrs[] = { "Authorization: Bearer k", NULL };
 wpe_http_get *g;
 memset(&got_answer, 0, sizeof got_answer);
 srv_start(SRV_ANSWER, url, sizeof url);
 g = wpe_http_get_start(url, "/api/tags", hdrs, LOOP_LIMIT_MS, 0, on_answer, NULL,
                        err, sizeof err);
 check(g != NULL, "get: starts");
 check(got_answer.called == 0, "get: the owner is told from the loop, not from the start");
 run_loop_until_answer(LOOP_LIMIT_MS);
 check(got_answer.called == 1, "get: the owner is told once");
 check(got_answer.how == WPE_HTTP_GOT_DONE, "get: done");
 check(got_answer.status == 200, "get: status 200");
 check(!strcmp(got_answer.body, "hello"), "get: the whole body");
 check(strstr(srv.req, "GET /api/tags HTTP/1.1\r\n") != NULL, "get: the path asked");
 check(strstr(srv.req, "Authorization: Bearer k\r\n") != NULL, "get: the caller's header sent");
 srv_stop();
}

static void test_get_ends_at_its_deadline_with_host_and_port(void)
{
 char url[64], err[200], where[64];
 int port;
 memset(&got_answer, 0, sizeof got_answer);
 port = srv_start(SRV_SILENT, url, sizeof url);
 wpe_http_get_start(url, "/x", NULL, GET_DEADLINE_MS, 0, on_answer, NULL, err, sizeof err);
 run_loop_until_answer(LOOP_LIMIT_MS);
 snprintf(where, sizeof where, "no answer from 127.0.0.1:%d within 1 s", port);
 check(got_answer.called == 1, "deadline: the owner is told once");
 check(got_answer.how == WPE_HTTP_GOT_TIMED_OUT, "deadline: timed out");
 check(strstr(got_answer.why, where) != NULL, "deadline: the reason names host and port");
 srv_stop();
}

static void test_get_cancelled_never_calls_its_owner(void)
{
 char url[64], err[200];
 wpe_http_get *g;
 memset(&got_answer, 0, sizeof got_answer);
 srv_start(SRV_ANSWER, url, sizeof url);
 g = wpe_http_get_start(url, "/x", NULL, GET_DEADLINE_MS, 0, on_answer, NULL, err, sizeof err);
 wpe_http_get_cancel(g);
 run_loop_until_answer(GET_DEADLINE_MS + 500);
 check(got_answer.called == 0, "cancel: the owner is never told, the deadline neither");
 srv_stop();
}

static void test_get_peer_hangs_up_ends_it_once_and_leaves_no_timer(void)
{
 char url[64], err[200];
 memset(&got_answer, 0, sizeof got_answer);
 srv_start(SRV_HANGS_UP, url, sizeof url);
 wpe_http_get_start(url, "/x", NULL, GET_DEADLINE_MS, 0, on_answer, NULL, err, sizeof err);
 run_loop_until_answer(LOOP_LIMIT_MS);
 check(got_answer.called == 1, "hang-up: the owner is told once");
 check(got_answer.how == WPE_HTTP_GOT_FAILED, "hang-up: failed, not timed out");
 check(strstr(got_answer.why, "no response") != NULL, "hang-up: the reason says no response");
 got_answer.called = 0;
 run_loop_until_answer(GET_DEADLINE_MS + 500);
 check(got_answer.called == 0, "hang-up: the deadline left the loop with the GET");
 srv_stop();
}

static void test_get_refused_says_where(void)
{
 char url[64], err[200], where[64];
 int port, s;
 wpe_http_get *g;
 memset(&got_answer, 0, sizeof got_answer);
 port = srv_start(SRV_ANSWER, url, sizeof url);
 srv_stop();                                   /* nothing listens there any more */
 snprintf(where, sizeof where, "cannot connect to 127.0.0.1:%d", port);
 err[0] = '\0';
 g = wpe_http_get_start(url, "/x", NULL, GET_DEADLINE_MS, 0, on_answer, NULL, err, sizeof err);
 if (g)
  run_loop_until_answer(LOOP_LIMIT_MS);
 s = (!g && strstr(err, where)) ||
     (g && got_answer.how == WPE_HTTP_GOT_FAILED && strstr(got_answer.why, where));
 check(s, "refused: the reason names host and port");
}

static struct { int called, ok; char why[400]; } reached;

static void on_reached(int ok, const char *why, void *ud)
{
 (void)ud;
 reached.called++;
 reached.ok = ok;
 snprintf(reached.why, sizeof reached.why, "%s", why ? why : "");
}

/* Start a reachability check and drive the loop until it is told. */
static int reach_and_wait(const char *url, char *err, size_t errsz)
{
 wpe_http_reach *r;
 int rc, waited = 0;
 memset(&reached, 0, sizeof reached);
 rc = wpe_http_reach_start(url, GET_DEADLINE_MS, on_reached, NULL, &r, err, errsz);
 while (rc == WPE_HTTP_OPEN_PENDING && !reached.called && waited < LOOP_LIMIT_MS) {
  wpe_fd_poll(LOOP_SLICE_MS);
  waited += LOOP_SLICE_MS;
 }
 return rc;
}

static void test_reach_tells_a_listening_and_a_refusing_port_apart(void)
{
 char url[64], err[200], where[64];
 int port, rc;
 port = srv_start(SRV_SILENT, url, sizeof url);
 err[0] = '\0';
 rc = reach_and_wait(url, err, sizeof err);
 check(rc == WPE_HTTP_OPEN_READY || (reached.called == 1 && reached.ok),
       "reach: a listening port is reachable");
 srv_stop();
 snprintf(where, sizeof where, "cannot connect to 127.0.0.1:%d", port);
 err[0] = '\0';
 rc = reach_and_wait(url, err, sizeof err);
 check((rc == WPE_HTTP_OPEN_FAILED && strstr(err, where)) ||
       (reached.called == 1 && !reached.ok && strstr(reached.why, where)),
       "reach: a refusing port is told, with host and port");
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

 test_send_waits_for_a_full_socket_and_sends_it_all_in_order();
 test_send_stall_is_told_after_its_bound();
 test_get_reads_the_whole_answer_on_the_loop();
 test_get_ends_at_its_deadline_with_host_and_port();
 test_get_cancelled_never_calls_its_owner();
 test_get_peer_hangs_up_ends_it_once_and_leaves_no_timer();
 test_get_refused_says_where();
 test_reach_tells_a_listening_and_a_refusing_port_apart();

 printf(fail ? "SOME TESTS FAILED\n" : "ALL OK\n");
 return fail;
}
