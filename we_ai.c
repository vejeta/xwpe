/* we_ai.c - AI assistant core: configuration, backend selection, observability.
 * Compiled only under --enable-ai (WPE_AI).  The transport lives in
 * we_ai_http.c, the UI in we_ai_ui.c, the diff in we_ai_diff.c and the agent
 * tool harness in we_ai_agent.c. */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WPE_AI

#include "messages.h"
#include "edit.h"
#include "WeExpArr.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <json-c/json.h>

#include "we_ai.h"
#include "we_ai_http.h"

/* ----- configuration globals (defaults chosen so an empty config works) --- */
int   e_ai_backend  = WPE_AI_OLLAMA;
char *e_ai_endpoint = NULL;            /* filled by wpe_ai_config_init()       */
char *e_ai_model    = NULL;            /* "" => auto-pick the first model       */

int wpe_ai_backend_from_name(const char *name)
{
 if (!name)                          return WPE_AI_OLLAMA;
 if (!strcasecmp(name, "openai"))    return WPE_AI_OPENAI;
 if (!strcasecmp(name, "claude") ||
     !strcasecmp(name, "anthropic")) return WPE_AI_CLAUDE;
 if (!strcasecmp(name, "mock"))      return WPE_AI_MOCK;
 return WPE_AI_OLLAMA;
}

const char *wpe_ai_backend_name(int backend)
{
 switch (backend) {
  case WPE_AI_OPENAI: return "openai";
  case WPE_AI_CLAUDE: return "claude";
  case WPE_AI_MOCK:   return "mock";
  default:            return "ollama";
 }
}

int wpe_ai_enabled(void)
{
 const char *e = getenv("XWPE_AI_ENABLE");   /* test/dev force-enable override */
 if (e && *e == '1') return 1;
 return (WpeEditor && (WpeEditor->edopt & ED_AI_ENABLE)) ? 1 : 0;
}

/* strdup that never returns the same buffer we are about to leak; small and
 * self-contained so this file does not depend on WpeStrdup's semantics. */
static char *ai_strdup(const char *s)
{
 char *p;
 size_t n;
 if (!s) s = "";
 n = strlen(s) + 1;
 p = malloc(n);
 if (p) memcpy(p, s, n);
 return p;
}

void wpe_ai_config_init(void)
{
 const char *e;

 if ((e = getenv("XWPE_AI_BACKEND")))
  e_ai_backend = wpe_ai_backend_from_name(e);

 if ((e = getenv("XWPE_AI_ENDPOINT"))) {
  free(e_ai_endpoint);
  e_ai_endpoint = ai_strdup(e);
 }
 if (!e_ai_endpoint)
  e_ai_endpoint = ai_strdup("http://localhost:11434");

 if ((e = getenv("XWPE_AI_MODEL"))) {
  free(e_ai_model);
  e_ai_model = ai_strdup(e);
 }
 if (!e_ai_model)
  e_ai_model = ai_strdup("");
}

/* ----- observability ----------------------------------------------------- */
void wpe_ai_trace(const char *fmt, ...)
{
 const char *path = getenv("XWPE_AI_TRACE");
 FILE *tf;
 va_list ap;
 if (!path || !*path)
  return;
 tf = fopen(path, "a");
 if (!tf)
  return;
 va_start(ap, fmt);
 vfprintf(tf, fmt, ap);
 va_end(ap);
 fputc('\n', tf);
 fclose(tf);
}

/* ===================== backend request/response shaping =================== */

static long ai_now_ms(void)
{
 struct timespec ts;
 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *ai_path(int backend)
{
 switch (backend) {
  case WPE_AI_OPENAI: return "/v1/chat/completions";
  case WPE_AI_CLAUDE: return "/v1/messages";
  default:            return "/api/chat";   /* ollama */
 }
}

/* Fill hdrs[] (NULL-terminated) with malloc'd "Key: Value" strings; count. */
static int ai_headers(int backend, char *hdrs[], int max)
{
 int n = 0;
 const char *k;
 char b[600];
 if (n < max) hdrs[n++] = ai_strdup("Content-Type: application/json");
 if (backend == WPE_AI_OPENAI) {
  k = getenv("OPENAI_API_KEY");
  if (k && *k && n < max) {
   snprintf(b, sizeof b, "Authorization: Bearer %s", k);
   hdrs[n++] = ai_strdup(b);
  }
 } else if (backend == WPE_AI_CLAUDE) {
  k = getenv("ANTHROPIC_API_KEY");
  if (k && *k && n < max) {
   snprintf(b, sizeof b, "x-api-key: %s", k);
   hdrs[n++] = ai_strdup(b);
  }
  if (n < max) hdrs[n++] = ai_strdup("anthropic-version: 2023-06-01");
 }
 hdrs[n] = NULL;
 return n;
}

static char *ai_build_body(int backend, const wpe_ai_req *req)
{
 struct json_object *root = json_object_new_object();
 struct json_object *arr  = json_object_new_array();
 const char *model = (req->model && *req->model) ? req->model
                     : (e_ai_model ? e_ai_model : "");
 char *out;
 int i;

 json_object_object_add(root, "model", json_object_new_string(model));
 json_object_object_add(root, "stream", json_object_new_boolean(1));

 if (backend == WPE_AI_CLAUDE) {
  struct json_object *sys = NULL;
  json_object_object_add(root, "max_tokens", json_object_new_int(4096));
  for (i = 0; i < req->nmsgs; i++) {
   if (!strcmp(req->msgs[i].role, "system")) {
    if (!sys) sys = json_object_new_string(req->msgs[i].content);
    continue;
   }
   {
    struct json_object *m = json_object_new_object();
    json_object_object_add(m, "role", json_object_new_string(req->msgs[i].role));
    json_object_object_add(m, "content", json_object_new_string(req->msgs[i].content));
    json_object_array_add(arr, m);
   }
  }
  if (sys) json_object_object_add(root, "system", sys);
  json_object_object_add(root, "messages", arr);
 } else {
  for (i = 0; i < req->nmsgs; i++) {
   struct json_object *m = json_object_new_object();
   json_object_object_add(m, "role", json_object_new_string(req->msgs[i].role));
   json_object_object_add(m, "content", json_object_new_string(req->msgs[i].content));
   json_object_array_add(arr, m);
  }
  json_object_object_add(root, "messages", arr);
 }
 out = ai_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
 json_object_put(root);
 return out;
}

/* Parse one streamed body line.  On assistant text, *delta = malloc'd copy
 * (caller frees).  Sets *done when the stream signals completion. */
static void ai_parse_line(int backend, const char *line, size_t len,
                          char **delta, int *done)
{
 const char *p = line;
 struct json_object *o, *tmp;
 (void)len;
 *delta = NULL;
 if (backend == WPE_AI_MOCK) backend = WPE_AI_OLLAMA;

 if (backend == WPE_AI_OPENAI || backend == WPE_AI_CLAUDE) {
  if (!strncmp(p, "data:", 5)) { p += 5; while (*p == ' ') p++; }
  else return;                              /* event:/comment/blank */
  if (!strncmp(p, "[DONE]", 6)) { *done = 1; return; }
 }
 if (!*p) return;
 o = json_tokener_parse(p);
 if (!o) return;

 if (backend == WPE_AI_OLLAMA) {
  if (json_object_object_get_ex(o, "message", &tmp)) {
   struct json_object *c;
   if (json_object_object_get_ex(tmp, "content", &c))
    *delta = ai_strdup(json_object_get_string(c));
  }
  if (json_object_object_get_ex(o, "done", &tmp) && json_object_get_boolean(tmp))
   *done = 1;
 } else if (backend == WPE_AI_OPENAI) {
  struct json_object *ch;
  if (json_object_object_get_ex(o, "choices", &ch) &&
      json_object_array_length(ch) > 0) {
   struct json_object *c0 = json_object_array_get_idx(ch, 0), *d, *cont, *fin;
   if (json_object_object_get_ex(c0, "delta", &d) &&
       json_object_object_get_ex(d, "content", &cont)) {
    const char *s = json_object_get_string(cont);
    if (s) *delta = ai_strdup(s);
   }
   if (json_object_object_get_ex(c0, "finish_reason", &fin) &&
       json_object_get_type(fin) != json_type_null)
    *done = 1;
  }
 } else if (backend == WPE_AI_CLAUDE) {
  struct json_object *ty;
  if (json_object_object_get_ex(o, "type", &ty)) {
   const char *t = json_object_get_string(ty);
   if (t && !strcmp(t, "content_block_delta")) {
    struct json_object *d, *txt;
    if (json_object_object_get_ex(o, "delta", &d) &&
        json_object_object_get_ex(d, "text", &txt))
     *delta = ai_strdup(json_object_get_string(txt));
   } else if (t && !strcmp(t, "message_stop")) {
    *done = 1;
   }
  }
 }
 json_object_put(o);
}

/* ===================== blocking fetch (for model lists) ================== */

static int ai_fetch(const char *method, const char *path, char *hdrs[],
                    const char *body, int timeout_ms, char **resp_out,
                    int *status_out, char *errbuf, size_t errsz)
{
 wpe_http_conn conn;
 wpe_http_stream hs;
 char buf[4096];
 long deadline;
 int rc = -1;

 if (resp_out) *resp_out = NULL;
 if (wpe_http_open(e_ai_endpoint, &conn, errbuf, errsz)) return -1;
 if (wpe_http_request(&conn, method, path, (const char *const *)hdrs, body)) {
  if (errbuf) snprintf(errbuf, errsz, "request failed");
  wpe_http_close(&conn);
  return -1;
 }
 wpe_http_stream_init(&hs);
 deadline = ai_now_ms() + timeout_ms;
 while (!wpe_http_stream_done(&hs)) {
  struct pollfd pf;
  int pr;
  long left = deadline - ai_now_ms();
  if (left <= 0) { if (errbuf) snprintf(errbuf, errsz, "timed out"); break; }
  pf.fd = conn.fd; pf.events = POLLIN; pf.revents = 0;
  pr = poll(&pf, 1, (int)(left > 1000 ? 1000 : left));
  if (pr < 0) { if (errno == EINTR) continue; break; }
  if (pr == 0) continue;
  {
   ssize_t r = wpe_http_read(&conn, buf, sizeof buf);
   if (r > 0) { if (wpe_http_stream_push(&hs, buf, (size_t)r) < 0) break; }
   else if (r < 0) { wpe_http_stream_eof(&hs); break; }   /* EOF => complete */
  }
 }
 if (wpe_http_stream_done(&hs) || hs.body_len > 0) {
  if (status_out) *status_out = wpe_http_stream_status(&hs);
  if (resp_out) {
   *resp_out = malloc(hs.body_len + 1);
   if (*resp_out) {
    memcpy(*resp_out, hs.body ? hs.body : "", hs.body_len);
    (*resp_out)[hs.body_len] = '\0';
   }
  }
  rc = 0;
 }
 wpe_http_stream_free(&hs);
 wpe_http_close(&conn);
 return rc;
}

/* ===================== reachability / models ============================= */

int wpe_ai_preflight(int backend, char *errbuf, size_t errsz)
{
 wpe_http_conn conn;
 if (backend == WPE_AI_MOCK) return 0;
 if (wpe_http_open(e_ai_endpoint, &conn, errbuf, errsz) != 0) {
  if (backend == WPE_AI_OLLAMA && errbuf)
   snprintf(errbuf, errsz,
            "Ollama not reachable at %s - is it running? (try `ollama serve`)",
            e_ai_endpoint ? e_ai_endpoint : "?");
  return -1;
 }
 wpe_http_close(&conn);
 return 0;
}

int wpe_ai_list_models(int backend, char **names, int max,
                       char *errbuf, size_t errsz)
{
 char *hdrs[8];
 char *resp = NULL;
 const char *path;
 int nh, i, st = 0, cnt = 0, rc;
 struct json_object *o, *arr, *it, *nm;

 if (backend == WPE_AI_MOCK) {
  if (max > 0) { names[0] = ai_strdup("mock-model"); return 1; }
  return 0;
 }
 if (backend == WPE_AI_CLAUDE) {
  if (cnt < max) names[cnt++] = ai_strdup("claude-sonnet-5");
  if (cnt < max) names[cnt++] = ai_strdup("claude-opus-5");
  return cnt;
 }
 path = (backend == WPE_AI_OPENAI) ? "/v1/models" : "/api/tags";
 nh = ai_headers(backend, hdrs, 8);
 rc = ai_fetch("GET", path, hdrs, NULL, 5000, &resp, &st, errbuf, errsz);
 for (i = 0; i < nh; i++) free(hdrs[i]);
 if (rc < 0 || !resp) { free(resp); return -1; }

 o = json_tokener_parse(resp);
 free(resp);
 if (!o) { if (errbuf) snprintf(errbuf, errsz, "unexpected model-list reply"); return -1; }
 if (backend == WPE_AI_OPENAI) {
  if (json_object_object_get_ex(o, "data", &arr)) {
   int L = json_object_array_length(arr);
   for (i = 0; i < L && cnt < max; i++) {
    it = json_object_array_get_idx(arr, i);
    if (json_object_object_get_ex(it, "id", &nm))
     names[cnt++] = ai_strdup(json_object_get_string(nm));
   }
  }
 } else {                                    /* ollama /api/tags */
  if (json_object_object_get_ex(o, "models", &arr)) {
   int L = json_object_array_length(arr);
   for (i = 0; i < L && cnt < max; i++) {
    it = json_object_array_get_idx(arr, i);
    if (json_object_object_get_ex(it, "name", &nm))
     names[cnt++] = ai_strdup(json_object_get_string(nm));
   }
  }
 }
 json_object_put(o);
 return cnt;
}

int wpe_ai_ensure_model(char *errbuf, size_t errsz)
{
 char *names[32];
 int n, i;
 if (e_ai_model && *e_ai_model) return 0;
 if (errbuf && errsz) errbuf[0] = '\0';
 n = wpe_ai_list_models(e_ai_backend, names, 32, errbuf, errsz);
 if (n <= 0) {
  if (errbuf && !errbuf[0]) snprintf(errbuf, errsz, "no models available");
  return -1;
 }
 free(e_ai_model);
 e_ai_model = ai_strdup(names[0]);
 for (i = 0; i < n; i++) free(names[i]);
 return 0;
}

/* ===================== streaming chat session ============================ */

struct wpe_ai_stream {
 int             backend;
 wpe_http_conn   conn;
 wpe_http_stream hs;
 int             done;
 int             eof;
};

/* Mock: stage a canned HTTP response (NDJSON) in a temp file and read it back,
 * so the whole streaming path (framer + parser + fd-loop) is exercised with no
 * network and full determinism.  The reply text comes from $XWPE_AI_MOCK_REPLY
 * (default a short greeting), JSON-escaped via json-c so code payloads are safe. */
static int ai_mock_open(struct wpe_ai_stream *st)
{
 const char *reply = getenv("XWPE_AI_MOCK_REPLY");
 struct json_object *o, *m;
 const char *body1;
 char *resp;
 size_t need;
 char tmpl[] = "/tmp/xwpe_ai_mockXXXXXX";
 int fd;

 if (!reply) reply = "Hello from the xwpe mock backend.";
 o = json_object_new_object();
 m = json_object_new_object();
 json_object_object_add(m, "role", json_object_new_string("assistant"));
 json_object_object_add(m, "content", json_object_new_string(reply));
 json_object_object_add(o, "message", m);
 json_object_object_add(o, "done", json_object_new_boolean(0));
 body1 = json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN);

 need = strlen(body1) + 128;
 resp = malloc(need);
 if (!resp) { json_object_put(o); return -1; }
 snprintf(resp, need,
          "HTTP/1.1 200 OK\r\nContent-Type: application/x-ndjson\r\n\r\n"
          "%s\n{\"done\":true}\n", body1);
 json_object_put(o);

 fd = mkstemp(tmpl);
 if (fd < 0) { free(resp); return -1; }
 unlink(tmpl);
 if (write(fd, resp, strlen(resp)) < 0) { free(resp); close(fd); return -1; }
 free(resp);
 lseek(fd, 0, SEEK_SET);

 memset(&st->conn, 0, sizeof st->conn);
 st->conn.fd = fd;
 st->conn.is_tls = 0;
 strncpy(st->conn.host, "mock", sizeof st->conn.host - 1);
 wpe_http_stream_init(&st->hs);
 return 0;
}

wpe_ai_stream *wpe_ai_stream_start(const wpe_ai_req *req, char *errbuf, size_t errsz)
{
 struct wpe_ai_stream *st = calloc(1, sizeof *st);
 if (!st) { if (errbuf) snprintf(errbuf, errsz, "out of memory"); return NULL; }
 st->backend = e_ai_backend;

 if (st->backend == WPE_AI_MOCK) {
  if (ai_mock_open(st) != 0) {
   if (errbuf) snprintf(errbuf, errsz, "mock backend setup failed");
   free(st);
   return NULL;
  }
  wpe_ai_trace("stream start backend=mock");
  return st;
 }

 {
  char *hdrs[8];
  char *body;
  int nh, i, rc;
  nh = ai_headers(st->backend, hdrs, 8);
  body = ai_build_body(st->backend, req);
  if (wpe_http_open(e_ai_endpoint, &st->conn, errbuf, errsz) != 0) {
   for (i = 0; i < nh; i++) free(hdrs[i]);
   free(body); free(st);
   return NULL;
  }
  rc = wpe_http_request(&st->conn, "POST", ai_path(st->backend),
                        (const char *const *)hdrs, body);
  for (i = 0; i < nh; i++) free(hdrs[i]);
  free(body);
  if (rc != 0) {
   if (errbuf) snprintf(errbuf, errsz, "request failed");
   wpe_http_close(&st->conn);
   free(st);
   return NULL;
  }
  wpe_http_stream_init(&st->hs);
  wpe_ai_trace("stream start backend=%s model=%s",
               wpe_ai_backend_name(st->backend), e_ai_model ? e_ai_model : "");
 }
 return st;
}

int wpe_ai_stream_fd(wpe_ai_stream *st) { return st ? st->conn.fd : -1; }
int wpe_ai_stream_http_status(wpe_ai_stream *st)
{ return st ? wpe_http_stream_status(&st->hs) : 0; }

int wpe_ai_stream_pump(wpe_ai_stream *st,
                       void (*cb)(const char *delta, void *ud), void *ud,
                       int *done)
{
 char buf[4096];
 char *line;
 size_t llen;

 for (;;) {
  ssize_t r = wpe_http_read(&st->conn, buf, sizeof buf);
  if (r > 0) { if (wpe_http_stream_push(&st->hs, buf, (size_t)r) < 0) return -1; }
  else if (r == 0) break;                    /* nothing more available now   */
  else { st->eof = 1; wpe_http_stream_eof(&st->hs); break; }  /* EOF/error    */
 }
 while (wpe_http_stream_next_line(&st->hs, &line, &llen)) {
  char *delta = NULL;
  int d = 0;
  ai_parse_line(st->backend, line, llen, &delta, &d);
  if (delta) { if (*delta && cb) cb(delta, ud); free(delta); }
  if (d) st->done = 1;
 }
 *done = (st->done || st->eof) ? 1 : 0;
 return 0;
}

void wpe_ai_stream_free(wpe_ai_stream *st)
{
 if (!st) return;
 wpe_http_close(&st->conn);
 wpe_http_stream_free(&st->hs);
 free(st);
}

/* ----- blocking completion (Edit / Agent) -------------------------------- */

struct ai_collect { char *buf; size_t len, cap; };

static void ai_collect_cb(const char *delta, void *ud)
{
 struct ai_collect *c = ud;
 size_t dl = strlen(delta);
 if (c->len + dl + 1 > c->cap) {
  size_t nc = c->cap ? c->cap : 1024;
  char *nb;
  while (c->len + dl + 1 > nc) nc *= 2;
  nb = realloc(c->buf, nc);
  if (!nb) return;
  c->buf = nb;
  c->cap = nc;
 }
 memcpy(c->buf + c->len, delta, dl);
 c->len += dl;
 c->buf[c->len] = '\0';
}

char *wpe_ai_complete(const wpe_ai_req *req, int timeout_ms,
                      char *errbuf, size_t errsz)
{
 wpe_ai_stream *st = wpe_ai_stream_start(req, errbuf, errsz);
 struct ai_collect c;
 long deadline;
 int done = 0, fd;

 if (!st) return NULL;
 c.buf = NULL; c.len = 0; c.cap = 0;
 fd = wpe_ai_stream_fd(st);
 deadline = ai_now_ms() + timeout_ms;
 while (!done) {
  struct pollfd pf;
  int pr;
  long left = deadline - ai_now_ms();
  if (left <= 0) { if (errbuf) snprintf(errbuf, errsz, "timed out"); break; }
  pf.fd = fd; pf.events = POLLIN; pf.revents = 0;
  pr = poll(&pf, 1, (int)(left > 500 ? 500 : left));
  if (pr < 0) { if (errno == EINTR) continue; break; }
  if (pr == 0) continue;
  if (wpe_ai_stream_pump(st, ai_collect_cb, &c, &done) < 0) {
   if (errbuf) snprintf(errbuf, errsz, "transport error");
   break;
  }
 }
 wpe_ai_stream_free(st);
 if (!done) { free(c.buf); return NULL; }
 return c.buf ? c.buf : ai_strdup("");
}

#endif /* WPE_AI */

/* Keep this a non-empty translation unit for ISO C when WPE_AI is off. */
typedef int wpe_ai_core_translation_unit;
