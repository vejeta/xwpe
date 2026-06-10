/* we_lsp.c -- Language Server Protocol client engine.  See we_lsp.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "we_lsp.h"
#include "we_dap_proto.h"        /* e_dap_reader: shared Content-Length framing */

#define LSP_TMO_INIT  120000     /* server start + initialize                */
#define LSP_TMO_BUILD 240000     /* import build + first compile (cold cache) */
#define LSP_TMO_REQ    30000     /* an interactive request once warm          */

struct e_lsp_session {
 pid_t        pid;
 int          in_fd;             /* write requests to the server's stdin  */
 int          out_fd;            /* read messages from the server's stdout */
 e_dap_reader rd;
 int          id;                /* JSON-RPC id counter                   */
 e_lsp_host   host;
 int          doc_version;       /* textDocument version (didOpen = 1)    */
 char         root_uri[PATH_MAX + 8];
 e_lsp_completion_item items[256];  /* engine-owned completion result      */
 int          nitems;
 e_lsp_location locs[256];          /* engine-owned references result      */
 int          nlocs;
 e_lsp_symbol  syms[256];           /* engine-owned outline/workspace result */
 int          nsyms;
 e_lsp_code_action acts[64];        /* engine-owned code-action result      */
 int          nacts;
 struct json_object *acts_raw;      /* last codeAction array, kept for apply */
 e_lsp_code_lens lenses[64];        /* engine-owned code-lens result        */
 int          nlenses;
 e_lsp_inlay_hint inlays[512];      /* engine-owned inlay-hint result        */
 int          ninlays;
};

/* ---- transport --------------------------------------------------------- */

static int lsp_write_all(int fd, const char *buf, size_t n)
{
 size_t off = 0;
 while (off < n)
 {
  ssize_t w = write(fd, buf + off, n - off);
  if (w < 0)
  {
   if (errno == EINTR)
    continue;
   return -1;
  }
  off += (size_t)w;
 }
 return 0;
}

/* Wire trace (defined below): logged only when XWPE_LSP_TRACE names a file. */
static void lsp_trace_request(int id, const char *method,
                              struct json_object *params);
static void lsp_trace_response(int want_id, struct json_object *m);

/* Send a JSON-RPC message; id < 0 = notification.  Takes ownership of params. */
static int lsp_send(e_lsp_session *s, int id, const char *method,
                    struct json_object *params)
{
 struct json_object *msg = json_object_new_object();
 const char *body;
 char *frame;
 size_t blen, flen;
 int rc;

 json_object_object_add(msg, "jsonrpc", json_object_new_string("2.0"));
 if (id >= 0)
  json_object_object_add(msg, "id", json_object_new_int(id));
 if (method)
  json_object_object_add(msg, "method", json_object_new_string(method));
 if (params)
  json_object_object_add(msg, "params", params);
 lsp_trace_request(id, method, params);

 body = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
 blen = strlen(body);
 flen = blen + 40;
 frame = malloc(flen);
 if (!frame)
 {  json_object_put(msg);  return -1;  }
 rc = snprintf(frame, flen, "Content-Length: %zu\r\n\r\n%s", blen, body);
 lsp_write_all(s->in_fd, frame, (size_t)rc);
 free(frame);
 json_object_put(msg);
 return 0;
}

static void lsp_notify(e_lsp_session *s, const char *method, struct json_object *p)
{  lsp_send(s, -1, method, p);  }

/* Reply to a server->client request with `result` (takes ownership). */
static void lsp_reply(e_lsp_session *s, struct json_object *id_field,
                      struct json_object *result)
{
 struct json_object *msg = json_object_new_object();
 const char *body;
 char *frame;
 size_t blen, flen;
 int rc;

 json_object_object_add(msg, "jsonrpc", json_object_new_string("2.0"));
 json_object_object_add(msg, "id", id_field ? json_object_get(id_field) : NULL);
 json_object_object_add(msg, "result", result ? result : NULL);
 body = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
 blen = strlen(body);
 flen = blen + 40;
 frame = malloc(flen);
 if (frame)
 {
  rc = snprintf(frame, flen, "Content-Length: %zu\r\n\r\n%s", blen, body);
  lsp_write_all(s->in_fd, frame, (size_t)rc);
  free(frame);
 }
 json_object_put(msg);
}

static long lsp_now_ms(void)
{
 struct timespec ts;
 clock_gettime(CLOCK_MONOTONIC, &ts);
 return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct json_object *lsp_read_message(e_lsp_session *s, int timeout_ms)
{
 long deadline = lsp_now_ms() + timeout_ms;
 for (;;)
 {
  struct pollfd p;
  char buf[8192];
  ssize_t n;
  int err = 0, left;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);

  if (m)
   return m;
  left = (int)(deadline - lsp_now_ms());
  if (left <= 0)
   return NULL;
  p.fd = s->out_fd;
  p.events = POLLIN;
  if (poll(&p, 1, left) <= 0)
   return NULL;
  n = read(s->out_fd, buf, sizeof(buf));
  if (n <= 0)
  {
   if (n < 0 && errno == EINTR)
    continue;
   return NULL;
  }
  e_dap_reader_push(&s->rd, buf, (size_t)n);
 }
}

/* ---- json helpers ------------------------------------------------------ */

static const char *obj_str(struct json_object *o, const char *k)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return json_object_get_string(v);
 return NULL;
}
static int obj_int(struct json_object *o, const char *k, int dflt)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return json_object_get_int(v);
 return dflt;
}
static struct json_object *obj_obj(struct json_object *o, const char *k)
{
 struct json_object *v = NULL;
 if (o && json_object_object_get_ex(o, k, &v))
  return v;
 return NULL;
}

/* Strip HTML to readable plain text: tags are removed, a handful of block tags
   become line breaks, and the common entities are decoded.  Used to turn the
   Metals Doctor (HTML) into text we can show in a window.  Output is never
   longer than the input.  Returns a malloc'd string (caller frees), or NULL. */
static char *lsp_html_to_text(const char *html)
{
 size_t n = strlen(html), o = 0, i;
 char *out = malloc(n + 1);

 if (!out)
  return(NULL);
 for (i = 0; i < n; )
 {
  if (html[i] == '<')                     /* a tag */
  {
   const char *t = html + i + 1;
   int nl = (!strncmp(t, "br", 2)   || !strncmp(t, "/p", 2)  ||
             !strncmp(t, "/tr", 3)  || !strncmp(t, "/li", 3) ||
             !strncmp(t, "/h", 2)   || !strncmp(t, "/div", 4)||
             !strncmp(t, "/table", 6) || !strncmp(t, "li", 2));
   while (i < n && html[i] != '>')
    i++;
   if (i < n)
    i++;                                  /* skip '>' */
   if (nl && o > 0 && out[o - 1] != '\n')
    out[o++] = '\n';
   continue;
  }
  if (html[i] == '&')                      /* entity */
  {
   if (!strncmp(html + i, "&amp;", 5))  { out[o++] = '&';  i += 5; continue; }
   if (!strncmp(html + i, "&lt;", 4))   { out[o++] = '<';  i += 4; continue; }
   if (!strncmp(html + i, "&gt;", 4))   { out[o++] = '>';  i += 4; continue; }
   if (!strncmp(html + i, "&quot;", 6)) { out[o++] = '"';  i += 6; continue; }
   if (!strncmp(html + i, "&nbsp;", 6)) { out[o++] = ' ';  i += 6; continue; }
   if (html[i + 1] == '#')               /* numeric: &#NN; or &#xHH; */
   {
    int code = 0, hex = (html[i + 2] == 'x' || html[i + 2] == 'X');
    size_t j = i + 2 + (hex ? 1 : 0);
    while (j < n && html[j] != ';')
    {
     char c = html[j];
     if (hex)
      code = code * 16 + (c <= '9' ? c - '0' :
                          (c | 0x20) - 'a' + 10);
     else if (c >= '0' && c <= '9')
      code = code * 10 + (c - '0');
     j++;
    }
    if (j < n && code > 0 && code < 128)
    {  out[o++] = (char)code;  i = j + 1;  continue;  }
   }
  }
  out[o++] = html[i++];
 }
 out[o] = '\0';
 return(out);
}

/* file:///path -> /path (best-effort; ignores host, no %xx unescaping). */
static const char *uri_to_path(const char *uri)
{
 if (uri && !strncmp(uri, "file://", 7))
 {
  const char *p = uri + 7;
  return (*p == '/') ? p : p - 0;   /* file:///abs -> /abs */
 }
 return uri;
}

/* Dispatch a publishDiagnostics notification to the host. */
static void lsp_dispatch_diagnostics(e_lsp_session *s, struct json_object *params)
{
 const char *uri = obj_str(params, "uri");
 struct json_object *diags = obj_obj(params, "diagnostics");
 int i, n, errors = 0, warnings = 0;

 if (!uri || !diags || !json_object_is_type(diags, json_type_array))
  return;
 n = json_object_array_length(diags);
 for (i = 0; i < n; i++)
 {
  struct json_object *d = json_object_array_get_idx(diags, i);
  struct json_object *rng = obj_obj(d, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  struct json_object *en = rng ? obj_obj(rng, "end") : NULL;
  int sev = obj_int(d, "severity", 1);
  if (sev == 1) errors++;
  else if (sev == 2) warnings++;
  if (s->host.on_diagnostic)
   s->host.on_diagnostic(uri_to_path(uri),
                         obj_int(st, "line", 0), obj_int(st, "character", 0),
                         obj_int(en, "line", obj_int(st, "line", 0)),
                         obj_int(en, "character", obj_int(st, "character", 0)),
                         sev, obj_str(d, "message"), s->host.ud);
 }
 if (s->host.on_diagnostics_summary)
  s->host.on_diagnostics_summary(uri_to_path(uri), errors, warnings, s->host.ud);
}

/* Answer a server->client request so the server never blocks.  We advertise no
   UI providers, so the safe reply is null / an array of nulls. */
/* The configuration we report for a pulled section (workspace/configuration).
   Metals' inlay hints are OFF by default; turn on inferred-type hints for the
   "metals" section (val/def result types).  Other sections -> NULL (= default).
   Returns a new json object (handed to the reply array) or NULL. */
static struct json_object *lsp_config_for_section(const char *section)
{
 struct json_object *cfg, *ih, *it;

 if (!section || strcmp(section, "metals") != 0)
  return(NULL);
 it = json_object_new_object();
 json_object_object_add(it, "enable", json_object_new_boolean(1));
 ih = json_object_new_object();
 json_object_object_add(ih, "inferredTypes", it);
 cfg = json_object_new_object();
 json_object_object_add(cfg, "inlayHints", ih);
 return(cfg);
}

static void lsp_answer_request(e_lsp_session *s, struct json_object *m)
{
 const char *meth = obj_str(m, "method");
 struct json_object *id = obj_obj(m, "id");

 if (meth && !strcmp(meth, "window/showMessageRequest"))
 {
  struct json_object *acts = obj_obj(obj_obj(m, "params"), "actions");
  struct json_object *first = (acts && json_object_is_type(acts, json_type_array)
                               && json_object_array_length(acts) > 0)
                              ? json_object_array_get_idx(acts, 0) : NULL;
  lsp_reply(s, id, first ? json_object_get(first) : NULL);
 }
 else if (meth && !strcmp(meth, "workspace/configuration"))
 {
  struct json_object *items = obj_obj(obj_obj(m, "params"), "items");
  struct json_object *arr = json_object_new_array();
  int i, n = (items && json_object_is_type(items, json_type_array))
             ? json_object_array_length(items) : 0;
  for (i = 0; i < n; i++)
  {
   struct json_object *it = json_object_array_get_idx(items, i);
   json_object_array_add(arr, lsp_config_for_section(obj_str(it, "section")));
  }
  lsp_reply(s, id, arr);
 }
 else if (meth && !strcmp(meth, "window/showDocument"))
 {
  /* Reply "handled" but do NOT open anything -- this swallows Metals' Doctor
     URL so it does not spawn an external browser/KIO.  (Rendering the Doctor
     inside xwpe is a separate task.) */
  struct json_object *res = json_object_new_object();
  json_object_object_add(res, "success", json_object_new_boolean(1));
  lsp_reply(s, id, res);
 }
 else
  /* registerCapability, workDoneProgress/create, *_refresh, unknown -> null */
  lsp_reply(s, id, NULL);
}

/* Pump messages: answer server requests, deliver diagnostics, and return the
   response whose id == want_id (caller owns it).  If want_diag_path != NULL,
   return a sentinel (non-NULL, caller must json_object_put) as soon as that
   file's diagnostics arrive.  NULL on timeout/EOF. */
/* The wire-trace file (XWPE_LSP_TRACE=path), opened once, or NULL when unset. */
static FILE *lsp_trace_file(void)
{
 static FILE *fp = NULL;
 static int opened = 0;
 if (!opened)
 {
  const char *tp = getenv("XWPE_LSP_TRACE");
  if (tp)
   fp = fopen(tp, "a");
  opened = 1;
 }
 return fp;
}

/* Log one pumped message (its id/method and the id we are waiting for) so a
   stuck request can be diagnosed without attaching a debugger. */
static void lsp_trace_message(int want_id, struct json_object *id,
                              struct json_object *method)
{
 FILE *fp = lsp_trace_file();
 if (!fp)
  return;
 fprintf(fp, "[%ld want=%d] id=%s method=%s\n", lsp_now_ms(), want_id,
         id ? json_object_get_string(id) : "-",
         method ? json_object_get_string(method) : "-");
 fflush(fp);
}

/* Log a request we send (method + params) so the trace shows exactly what was
   asked -- e.g. the line/character of a go-to-definition. */
static void lsp_trace_request(int id, const char *method, struct json_object *params)
{
 FILE *fp = lsp_trace_file();
 if (!fp || id < 0 || !method)
  return;
 fprintf(fp, "[%ld SEND id=%d] %s params=%s\n", lsp_now_ms(), id, method,
         params ? json_object_to_json_string(params) : "{}");
 fflush(fp);
}

/* Log the result of the response we were waiting for, so an empty answer
   (the cause of a spurious "No definition found") is visible. */
static void lsp_trace_response(int want_id, struct json_object *m)
{
 FILE *fp = lsp_trace_file();
 struct json_object *result;
 if (!fp)
  return;
 result = m ? obj_obj(m, "result") : NULL;
 fprintf(fp, "[%ld RESP id=%d] result=%s\n", lsp_now_ms(), want_id,
         result ? json_object_to_json_string(result) : "(null)");
 fflush(fp);
}

/* Render a server-pushed client command.  The only one we act on is the Metals
   Doctor (delivered as HTML because we asked for doctorProvider:html): strip it
   to plain text and hand it to the host to show in a window instead of a
   browser.  Any other client command is ignored. */
static void lsp_dispatch_client_command(e_lsp_session *s, struct json_object *m)
{
 struct json_object *params = obj_obj(m, "params");
 const char *cmd = obj_str(params, "command");
 struct json_object *args, *a0;
 const char *html;
 char *text;

 if (!cmd || strncmp(cmd, "metals-doctor", 13) || !s->host.on_show_text)
  return;
 args = obj_obj(params, "arguments");
 a0 = (args && json_object_is_type(args, json_type_array)
       && json_object_array_length(args) > 0)
      ? json_object_array_get_idx(args, 0) : NULL;
 html = a0 ? json_object_get_string(a0) : NULL;
 if (!html || !*html)
  return;
 text = lsp_html_to_text(html);
 s->host.on_show_text("Metals Doctor", text ? text : html, s->host.ud);
 free(text);
}

/* Forward the server's transient status (metals/status) to the host: its @text
   ("Indexing", ...) and the @hide flag when it finishes. */
static void lsp_dispatch_status(e_lsp_session *s, struct json_object *m)
{
 struct json_object *params = obj_obj(m, "params");

 if (!s->host.on_status)
  return;
 s->host.on_status(obj_str(params, "text"), obj_int(params, "hide", 0),
                   s->host.ud);
}

/* Handle one server notification message.  Returns 1 when the diagnostics for
   @want_diag_path have just arrived (the pump caller is blocking on them), so
   the caller can stop waiting; 0 in every other case. */
static int lsp_dispatch_notification(e_lsp_session *s, struct json_object *m,
                                     const char *want_diag_path)
{
 const char *meth = json_object_get_string(obj_obj(m, "method"));

 if (!meth)
  return 0;
 if (!strcmp(meth, "textDocument/publishDiagnostics"))
 {
  struct json_object *params = obj_obj(m, "params");
  lsp_dispatch_diagnostics(s, params);
  if (want_diag_path)
  {
   const char *u = obj_str(params, "uri");
   if (u && !strcmp(uri_to_path(u), want_diag_path))
    return 1;
  }
 }
 else if (!strcmp(meth, "metals/executeClientCommand"))
  lsp_dispatch_client_command(s, m);
 else if (!strcmp(meth, "metals/status"))
  lsp_dispatch_status(s, m);
 return 0;
}

static struct json_object *lsp_pump(e_lsp_session *s, int want_id,
                                    const char *want_diag_path, int timeout_ms)
{
 long deadline = lsp_now_ms() + timeout_ms;
 for (;;)
 {
  int left = (int)(deadline - lsp_now_ms());
  struct json_object *m, *id, *method;
  if (left <= 0)
   return NULL;
  m = lsp_read_message(s, left);
  if (!m)
   return NULL;
  id = obj_obj(m, "id");
  method = obj_obj(m, "method");
  lsp_trace_message(want_id, id, method);
  if (id && method)                       /* server -> client REQUEST */
  {
   lsp_answer_request(s, m);
   json_object_put(m);
   continue;
  }
  if (method)                             /* notification */
  {
   int diag_arrived = lsp_dispatch_notification(s, m, want_diag_path);
   json_object_put(m);
   if (diag_arrived)
    return json_object_new_boolean(1);
   continue;
  }
  if (id)                                 /* response to our request */
  {
   if (want_id >= 0 && json_object_get_int(id) == want_id)
   {  lsp_trace_response(want_id, m);  return m;  }
  }
  json_object_put(m);
 }
}

/* ---- spawn + handshake ------------------------------------------------- */

static pid_t lsp_spawn(char *const argv[], const char *cwd, int *in_fd, int *out_fd)
{
 int to_child[2], from_child[2];
 pid_t pid;

 if (pipe(to_child) != 0)
  return -1;
 if (pipe(from_child) != 0)
 {  close(to_child[0]); close(to_child[1]); return -1;  }
 pid = fork();
 if (pid == 0)
 {
  int devnull;
  if (cwd && chdir(cwd) != 0)
   _exit(127);
  dup2(to_child[0], 0);
  dup2(from_child[1], 1);
  devnull = open("/dev/null", O_WRONLY);  /* servers are chatty on stderr */
  if (devnull >= 0)
  {  dup2(devnull, 2);  if (devnull > 2) close(devnull);  }
  close(to_child[0]); close(to_child[1]);
  close(from_child[0]); close(from_child[1]);
  execvp(argv[0], argv);
  _exit(127);
 }
 close(to_child[0]);
 close(from_child[1]);
 if (pid < 0)
 {  close(to_child[1]); close(from_child[0]); return -1;  }
 *in_fd = to_child[1];
 *out_fd = from_child[0];
 return pid;
}

/* The headless InitializationOptions (no UI providers to implement up front),
   as validated against Metals -- see HACKING-LSP.md. */
static struct json_object *lsp_init_options(void)
{
 struct json_object *o = json_object_new_object();
 struct json_object *co = json_object_new_object();
 json_object_object_add(co, "isCompletionItemDetailEnabled", json_object_new_boolean(1));
 json_object_object_add(co, "isHoverDocumentationEnabled", json_object_new_boolean(1));
 json_object_object_add(o, "compilerOptions", co);
 /* "on" => Metals pushes its transient status (Indexing/Importing/...) as
    metals/status notifications, which we surface as progress and a busy flag
    (so "still indexing" is distinguishable from "no result"). */
 json_object_object_add(o, "statusBarProvider", json_object_new_string("on"));
 json_object_object_add(o, "inputBoxProvider", json_object_new_boolean(0));
 json_object_object_add(o, "quickPickProvider", json_object_new_boolean(0));
 /* Accept client commands AND ask for the Doctor as HTML: Metals then PUSHES the
    Doctor to us via metals/executeClientCommand (we render it in a window)
    instead of opening an external browser -- with isHttpEnabled false there is
    no browser fallback either. */
 json_object_object_add(o, "executeClientCommandProvider", json_object_new_boolean(1));
 json_object_object_add(o, "doctorProvider", json_object_new_string("html"));
 json_object_object_add(o, "didFocusProvider", json_object_new_boolean(1));
 json_object_object_add(o, "isHttpEnabled", json_object_new_boolean(0));
 /* Advertise that the client can start debug sessions: this is what makes
    Metals emit the run/test code lenses (Alt-Q K), and it is accurate -- xwpe
    runs Scala mains over the same BSP/DAP path (we_bsp.c).  It only affects lens
    EMISSION; the run command is executed solely when the client invokes it. */
 json_object_object_add(o, "debuggingProvider", json_object_new_boolean(1));
 json_object_object_add(o, "fallbackScalaVersion", json_object_new_string("3.3.7"));
 return o;
}

static struct json_object *lsp_client_caps(void)
{
 struct json_object *caps = json_object_new_object();
 struct json_object *td = json_object_new_object();
 struct json_object *hover = json_object_new_object();
 struct json_object *fmts = json_object_new_array();

 json_object_array_add(fmts, json_object_new_string("plaintext"));
 json_object_array_add(fmts, json_object_new_string("markdown"));
 json_object_object_add(hover, "contentFormat", fmts);
 json_object_object_add(td, "hover", hover);
 json_object_object_add(td, "completion", json_object_new_object());
 json_object_object_add(td, "definition", json_object_new_object());
 json_object_object_add(td, "publishDiagnostics", json_object_new_object());
 json_object_object_add(td, "inlayHint", json_object_new_object());
 json_object_object_add(td, "callHierarchy", json_object_new_object());
 json_object_object_add(td, "typeHierarchy", json_object_new_object());
 json_object_object_add(caps, "textDocument", td);
 {
  struct json_object *ws = json_object_new_object();
  json_object_object_add(ws, "configuration", json_object_new_boolean(1));
  json_object_object_add(caps, "workspace", ws);
 }
 {  /* Tell the server WE display documents -- so Metals routes its Doctor URL
       through window/showDocument (which we intercept) instead of spawning an
       external browser/KIO for it. */
  struct json_object *win = json_object_new_object();
  struct json_object *sd = json_object_new_object();
  json_object_object_add(sd, "support", json_object_new_boolean(1));
  json_object_object_add(win, "showDocument", sd);
  json_object_object_add(caps, "window", win);
 }
 return caps;
}

e_lsp_session *e_lsp_open(char *const argv[], const char *root_dir,
                          const char *lang, const e_lsp_host *host)
{
 e_lsp_session *s;
 char rootdir[PATH_MAX];
 struct json_object *args, *resp;
 int id;

 (void)lang;
 if (!realpath(root_dir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", root_dir);

 s = calloc(1, sizeof(*s));
 if (!s)
  return NULL;
 e_dap_reader_init(&s->rd);
 if (host)
  s->host = *host;
 s->in_fd = s->out_fd = -1;
 snprintf(s->root_uri, sizeof(s->root_uri), "file://%s", rootdir);

 s->pid = lsp_spawn(argv, rootdir, &s->in_fd, &s->out_fd);
 if (s->pid <= 0)
 {  e_lsp_close(s);  return NULL;  }

 args = json_object_new_object();
 json_object_object_add(args, "processId", json_object_new_int((int)getpid()));
 json_object_object_add(args, "rootUri", json_object_new_string(s->root_uri));
 json_object_object_add(args, "capabilities", lsp_client_caps());
 json_object_object_add(args, "initializationOptions", lsp_init_options());
 id = ++s->id;
 lsp_send(s, id, "initialize", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_INIT);
 if (!resp)
 {  e_lsp_close(s);  return NULL;  }
 json_object_put(resp);
 lsp_notify(s, "initialized", json_object_new_object());
 return s;
}

/* ---- requests ---------------------------------------------------------- */

static struct json_object *lsp_text_pos(e_lsp_session *s, const char *path,
                                        int line, int character)
{
 struct json_object *args = json_object_new_object();
 struct json_object *doc = json_object_new_object();
 struct json_object *pos = json_object_new_object();
 char uri[PATH_MAX + 8];

 (void)s;
 snprintf(uri, sizeof(uri), "file://%s", path);
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 json_object_object_add(pos, "line", json_object_new_int(line));
 json_object_object_add(pos, "character", json_object_new_int(character));
 json_object_object_add(args, "position", pos);
 return args;
}

int e_lsp_did_open(e_lsp_session *s, const char *path, const char *text)
{
 struct json_object *args = json_object_new_object();
 struct json_object *doc = json_object_new_object();
 char uri[PATH_MAX + 8];

 if (!s)
  return -1;
 snprintf(uri, sizeof(uri), "file://%s", path);
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(doc, "languageId", json_object_new_string("scala"));
 json_object_object_add(doc, "version", json_object_new_int(1));
 json_object_object_add(doc, "text", json_object_new_string(text ? text : ""));
 json_object_object_add(args, "textDocument", doc);
 lsp_notify(s, "textDocument/didOpen", args);
 s->doc_version = 1;
 return 0;
}

int e_lsp_did_focus(e_lsp_session *s, const char *path)
{
 char uri[PATH_MAX + 8];

 if (!s)
  return -1;
 /* same URI form as didOpen so Metals ties the focus to the open document */
 snprintf(uri, sizeof(uri), "file://%s", path);
 lsp_notify(s, "metals/didFocusTextDocument", json_object_new_string(uri));
 return 0;
}

int e_lsp_did_change(e_lsp_session *s, const char *path, const char *text)
{
 struct json_object *args, *doc, *changes, *change;
 char uri[PATH_MAX + 8];

 if (!s)
  return -1;
 snprintf(uri, sizeof(uri), "file://%s", path);
 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(doc, "version", json_object_new_int(++s->doc_version));
 json_object_object_add(args, "textDocument", doc);
 changes = json_object_new_array();        /* full-document sync: one change */
 change = json_object_new_object();
 json_object_object_add(change, "text", json_object_new_string(text ? text : ""));
 json_object_array_add(changes, change);
 json_object_object_add(args, "contentChanges", changes);
 lsp_notify(s, "textDocument/didChange", args);
 return 0;
}

int e_lsp_wait_diagnostics(e_lsp_session *s, const char *path, int timeout_ms)
{
 char abspath[PATH_MAX];
 struct json_object *r;

 if (!s)
  return 0;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 r = lsp_pump(s, -1, abspath, timeout_ms);
 if (r)
 {  json_object_put(r);  return 1;  }
 return 0;
}

int e_lsp_poll(e_lsp_session *s)
{
 struct pollfd p;
 int handled = 0, guard = 0;

 if (!s || s->out_fd < 0)
  return 0;
 /* read whatever is immediately available, without blocking */
 while (guard++ < 64)
 {
  char buf[8192];
  ssize_t n;
  p.fd = s->out_fd;
  p.events = POLLIN;
  if (poll(&p, 1, 0) <= 0)
   break;
  n = read(s->out_fd, buf, sizeof(buf));
  if (n <= 0)
   break;
  e_dap_reader_push(&s->rd, buf, (size_t)n);
 }
 /* dispatch every complete message now buffered (diagnostics, server requests) */
 for (;;)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);
  struct json_object *id, *method;
  if (!m)
   break;
  handled++;
  id = obj_obj(m, "id");
  method = obj_obj(m, "method");
  if (id && method)
   lsp_answer_request(s, m);
  else if (method)
  {
   const char *meth = json_object_get_string(method);
   if (meth && !strcmp(meth, "textDocument/publishDiagnostics"))
    lsp_dispatch_diagnostics(s, obj_obj(m, "params"));
  }
  json_object_put(m);
 }
 return handled;
}

/* Strip a leading/trailing ```lang fenced block and collapse to one line. */
static char *lsp_markdown_to_text(const char *md)
{
 char *out, *w;
 const char *p = md;
 size_t len;

 if (!md)
  return NULL;
 len = strlen(md);
 out = malloc(len + 1);
 if (!out)
  return NULL;
 w = out;
 while (*p)
 {
  if (!strncmp(p, "```", 3))
  {
   p += 3;
   while (*p && *p != '\n')   /* skip the language tag line */
    p++;
   if (*p == '\n')
    p++;
   continue;
  }
  if (*p == '\n')
  {
   if (w > out && *(w - 1) != ' ')
    *w++ = ' ';
   p++;
   continue;
  }
  *w++ = *p++;
 }
 while (w > out && *(w - 1) == ' ')
  w--;
 *w = '\0';
 return out;
}

char *e_lsp_hover(e_lsp_session *s, const char *path, int line, int character)
{
 char abspath[PATH_MAX];
 struct json_object *resp, *result, *contents;
 char *out = NULL;
 int id;

 if (!s)
  return NULL;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 id = ++s->id;
 lsp_send(s, id, "textDocument/hover", lsp_text_pos(s, abspath, line, character));
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 contents = result ? obj_obj(result, "contents") : NULL;
 if (contents)
 {
  const char *v = NULL;
  if (json_object_is_type(contents, json_type_string))
   v = json_object_get_string(contents);
  else if (json_object_is_type(contents, json_type_object))
   v = obj_str(contents, "value");
  else if (json_object_is_type(contents, json_type_array) &&
           json_object_array_length(contents) > 0)
  {
   struct json_object *e0 = json_object_array_get_idx(contents, 0);
   v = json_object_is_type(e0, json_type_string) ? json_object_get_string(e0)
                                                  : obj_str(e0, "value");
  }
  if (v)
   out = lsp_markdown_to_text(v);
 }
 json_object_put(resp);
 return out;
}

char *e_lsp_signature_help(e_lsp_session *s, const char *path,
                           int line, int character)
{
 char abspath[PATH_MAX];
 struct json_object *resp, *result, *sigs, *sig;
 char *out = NULL;
 int id, idx, n;

 if (!s)
  return NULL;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 id = ++s->id;
 lsp_send(s, id, "textDocument/signatureHelp",
          lsp_text_pos(s, abspath, line, character));
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 sigs = result ? obj_obj(result, "signatures") : NULL;
 n = (sigs && json_object_is_type(sigs, json_type_array))
     ? json_object_array_length(sigs) : 0;
 if (n > 0)
 {
  idx = obj_int(result, "activeSignature", 0);
  if (idx < 0 || idx >= n)
   idx = 0;
  sig = json_object_array_get_idx(sigs, idx);
  if (obj_str(sig, "label"))
   out = strdup(obj_str(sig, "label"));
 }
 json_object_put(resp);
 return out;
}

/* Shared driver for the three "jump to a location" requests, which differ only
 * in the method name and all answer with Location | Location[] | LocationLink[].
 * Fills out_* with the first location; returns 0 on success, -1 otherwise. */
static int lsp_locate(e_lsp_session *s, const char *method, const char *path,
                      int line, int character, char *out_path, size_t out_sz,
                      int *out_line, int *out_char)
{
 char abspath[PATH_MAX];
 struct json_object *resp, *result, *loc = NULL, *rng, *st;
 int id, rc = -1;

 if (!s)
  return -1;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 id = ++s->id;
 lsp_send(s, id, method, lsp_text_pos(s, abspath, line, character));
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array) &&
     json_object_array_length(result) > 0)
  loc = json_object_array_get_idx(result, 0);
 else if (result && json_object_is_type(result, json_type_object))
  loc = result;
 if (loc)
 {
  /* Location { uri, range } or LocationLink { targetUri, targetRange } */
  const char *uri = obj_str(loc, "uri");
  if (!uri)
   uri = obj_str(loc, "targetUri");
  rng = obj_obj(loc, "range");
  if (!rng)
   rng = obj_obj(loc, "targetRange");
  st = rng ? obj_obj(rng, "start") : NULL;
  if (uri && st)
  {
   snprintf(out_path, out_sz, "%s", uri_to_path(uri));
   if (out_line) *out_line = obj_int(st, "line", 0);
   if (out_char) *out_char = obj_int(st, "character", 0);
   rc = 0;
  }
 }
 json_object_put(resp);
 return rc;
}

int e_lsp_definition(e_lsp_session *s, const char *path, int line, int character,
                     char *out_path, size_t out_sz, int *out_line, int *out_char)
{
 return lsp_locate(s, "textDocument/definition", path, line, character,
                   out_path, out_sz, out_line, out_char);
}

int e_lsp_implementation(e_lsp_session *s, const char *path, int line, int character,
                         char *out_path, size_t out_sz, int *out_line, int *out_char)
{
 return lsp_locate(s, "textDocument/implementation", path, line, character,
                   out_path, out_sz, out_line, out_char);
}

int e_lsp_type_definition(e_lsp_session *s, const char *path, int line, int character,
                          char *out_path, size_t out_sz, int *out_line, int *out_char)
{
 return lsp_locate(s, "textDocument/typeDefinition", path, line, character,
                   out_path, out_sz, out_line, out_char);
}

static void lsp_free_items(e_lsp_session *s)
{
 int i;
 for (i = 0; i < s->nitems; i++)
 {
  free(s->items[i].label);
  free(s->items[i].insert);
 }
 s->nitems = 0;
}

int e_lsp_completion(e_lsp_session *s, const char *path, int line, int character,
                     e_lsp_completion_item *items, int max)
{
 char abspath[PATH_MAX];
 struct json_object *resp, *result, *list = NULL;
 int id, i, n, out = 0;

 if (!s)
  return -1;
 lsp_free_items(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 id = ++s->id;
 lsp_send(s, id, "textDocument/completion", lsp_text_pos(s, abspath, line, character));
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array))
  list = result;                                  /* CompletionItem[] */
 else if (result && json_object_is_type(result, json_type_object))
  list = obj_obj(result, "items");                /* CompletionList   */
 n = (list && json_object_is_type(list, json_type_array))
     ? json_object_array_length(list) : 0;
 for (i = 0; i < n && out < max && out < (int)(sizeof(s->items)/sizeof(s->items[0])); i++)
 {
  struct json_object *c = json_object_array_get_idx(list, i);
  const char *label = obj_str(c, "label");
  const char *insert = obj_str(c, "insertText");
  const char *detail = obj_str(c, "detail");
  char *lbl;
  if (!label)
   continue;
  /* Prefer "label: detail" when the server splits the signature out. */
  if (detail && *detail && !strchr(label, ':'))
  {
   size_t l = strlen(label) + strlen(detail) + 3;
   lbl = malloc(l);
   if (lbl) snprintf(lbl, l, "%s%s", label, detail);
  }
  else
   lbl = strdup(label);
  s->items[s->nitems].label = lbl;
  s->items[s->nitems].insert = insert ? strdup(insert) : NULL;
  s->items[s->nitems].kind = obj_int(c, "kind", 0);
  items[out] = s->items[s->nitems];
  s->nitems++;
  out++;
 }
 json_object_put(resp);
 return out;
}

static void lsp_free_locs(e_lsp_session *s)
{
 int i;
 for (i = 0; i < s->nlocs; i++)
  free(s->locs[i].path);
 s->nlocs = 0;
}

static void lsp_free_syms(e_lsp_session *s)
{
 int i;
 for (i = 0; i < s->nsyms; i++)
 {
  free(s->syms[i].name);
  free(s->syms[i].path);
 }
 s->nsyms = 0;
}

int e_lsp_references(e_lsp_session *s, const char *path, int line, int character,
                     e_lsp_location *locs, int max)
{
 char abspath[PATH_MAX];
 struct json_object *args, *ctx, *resp, *result;
 int id, i, n, out = 0;

 if (!s)
  return -1;
 lsp_free_locs(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 args = lsp_text_pos(s, abspath, line, character);
 ctx = json_object_new_object();
 json_object_object_add(ctx, "includeDeclaration", json_object_new_boolean(1));
 json_object_object_add(args, "context", ctx);
 id = ++s->id;
 lsp_send(s, id, "textDocument/references", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 n = (result && json_object_is_type(result, json_type_array))
     ? json_object_array_length(result) : 0;
 for (i = 0; i < n && out < max &&
             out < (int)(sizeof(s->locs)/sizeof(s->locs[0])); i++)
 {
  struct json_object *loc = json_object_array_get_idx(result, i);
  const char *uri = obj_str(loc, "uri");
  struct json_object *rng = obj_obj(loc, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  if (!uri || !st)
   continue;
  s->locs[s->nlocs].path = strdup(uri_to_path(uri));
  s->locs[s->nlocs].line = obj_int(st, "line", 0);
  s->locs[s->nlocs].character = obj_int(st, "character", 0);
  locs[out] = s->locs[s->nlocs];
  s->nlocs++;
  out++;
 }
 json_object_put(resp);
 return out;
}

int e_lsp_document_highlight(e_lsp_session *s, const char *path, int line,
                             int character, e_lsp_location *locs, int max)
{
 char abspath[PATH_MAX];
 struct json_object *resp, *result;
 int id, i, n, out = 0;

 if (!s)
  return -1;
 lsp_free_locs(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 id = ++s->id;
 lsp_send(s, id, "textDocument/documentHighlight",
          lsp_text_pos(s, abspath, line, character));
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 /* DocumentHighlight[]: each is { range, kind } in THIS document -- no uri, so
    every hit shares abspath. */
 result = obj_obj(resp, "result");
 n = (result && json_object_is_type(result, json_type_array))
     ? json_object_array_length(result) : 0;
 for (i = 0; i < n && out < max &&
             out < (int)(sizeof(s->locs)/sizeof(s->locs[0])); i++)
 {
  struct json_object *h = json_object_array_get_idx(result, i);
  struct json_object *rng = obj_obj(h, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  if (!st)
   continue;
  s->locs[s->nlocs].path = strdup(abspath);
  s->locs[s->nlocs].line = obj_int(st, "line", 0);
  s->locs[s->nlocs].character = obj_int(st, "character", 0);
  locs[out] = s->locs[s->nlocs];
  s->nlocs++;
  out++;
 }
 json_object_put(resp);
 return out;
}

static void lsp_free_lenses(e_lsp_session *s)
{
 int i;
 for (i = 0; i < s->nlenses; i++)
  free(s->lenses[i].title);
 s->nlenses = 0;
}

static void lsp_free_inlays(e_lsp_session *s)
{
 int i;
 for (i = 0; i < s->ninlays; i++)
  free(s->inlays[i].label);
 s->ninlays = 0;
}

int e_lsp_code_lenses(e_lsp_session *s, const char *path,
                      e_lsp_code_lens *lenses, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *args, *doc, *resp, *result;
 int id, i, n, out = 0;

 if (!s)
  return -1;
 lsp_free_lenses(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 id = ++s->id;
 lsp_send(s, id, "textDocument/codeLens", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 n = (result && json_object_is_type(result, json_type_array))
     ? json_object_array_length(result) : 0;
 for (i = 0; i < n && out < max &&
             out < (int)(sizeof(s->lenses)/sizeof(s->lenses[0])); i++)
 {
  struct json_object *lens = json_object_array_get_idx(result, i);
  struct json_object *rng = obj_obj(lens, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  struct json_object *cmd = obj_obj(lens, "command");
  struct json_object *rresp = NULL;
  const char *title;
  if (!st)
   continue;
  if (!cmd)                            /* unresolved: ask the server to fill it */
  {
   int rid = ++s->id;
   lsp_send(s, rid, "codeLens/resolve", json_object_get(lens));
   rresp = lsp_pump(s, rid, NULL, LSP_TMO_REQ);
   if (rresp)
    cmd = obj_obj(obj_obj(rresp, "result"), "command");
  }
  title = cmd ? obj_str(cmd, "title") : NULL;
  if (title && *title)
  {
   s->lenses[s->nlenses].title = strdup(title);
   s->lenses[s->nlenses].line = obj_int(st, "line", 0);
   lenses[out] = s->lenses[s->nlenses];
   s->nlenses++;
   out++;
  }
  if (rresp)
   json_object_put(rresp);
 }
 json_object_put(resp);
 return out;
}

/* InlayHint.label is `string | InlayHintLabelPart[]`; build one display string,
   folding paddingLeft/paddingRight into single leading/trailing spaces.  Returns
   a malloc'd string (caller frees), or NULL. */
static char *lsp_inlay_label(struct json_object *hint)
{
 struct json_object *lab = obj_obj(hint, "label");
 int pad_l = json_object_get_boolean(obj_obj(hint, "paddingLeft"));
 int pad_r = json_object_get_boolean(obj_obj(hint, "paddingRight"));
 size_t cap = 3, len = 0;       /* 2 padding spaces + NUL */
 char *out;
 int is_str = lab && json_object_is_type(lab, json_type_string);
 int is_arr = lab && json_object_is_type(lab, json_type_array);
 int i, n = is_arr ? json_object_array_length(lab) : 0;

 if (is_str)
  cap += strlen(json_object_get_string(lab));
 else if (is_arr)
  for (i = 0; i < n; i++)
  {
   const char *v = obj_str(json_object_array_get_idx(lab, i), "value");
   if (v)
    cap += strlen(v);
  }
 out = malloc(cap);
 if (!out)
  return(NULL);
 if (pad_l)
  out[len++] = ' ';
 if (is_str)
 {
  const char *v = json_object_get_string(lab);
  size_t l = strlen(v);
  memcpy(out + len, v, l);
  len += l;
 }
 else if (is_arr)
  for (i = 0; i < n; i++)
  {
   const char *v = obj_str(json_object_array_get_idx(lab, i), "value");
   size_t l;
   if (!v)
    continue;
   l = strlen(v);
   memcpy(out + len, v, l);
   len += l;
  }
 if (pad_r)
  out[len++] = ' ';
 out[len] = '\0';
 return(out);
}

int e_lsp_inlay_hints(e_lsp_session *s, const char *path,
                      int start_line, int end_line,
                      e_lsp_inlay_hint *out, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *args, *doc, *rng, *st, *en, *resp, *result;
 int id, i, n, count = 0;

 if (!s)
  return -1;
 lsp_free_inlays(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 if (start_line < 0)
  start_line = 0;
 if (end_line < start_line)
  end_line = start_line;
 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 rng = json_object_new_object();
 st = json_object_new_object();
 json_object_object_add(st, "line", json_object_new_int(start_line));
 json_object_object_add(st, "character", json_object_new_int(0));
 en = json_object_new_object();
 json_object_object_add(en, "line", json_object_new_int(end_line));
 json_object_object_add(en, "character", json_object_new_int(1000000));
 json_object_object_add(rng, "start", st);
 json_object_object_add(rng, "end", en);
 json_object_object_add(args, "range", rng);
 id = ++s->id;
 lsp_send(s, id, "textDocument/inlayHint", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 n = (result && json_object_is_type(result, json_type_array))
     ? json_object_array_length(result) : 0;
 for (i = 0; i < n && count < max &&
             s->ninlays < (int)(sizeof(s->inlays)/sizeof(s->inlays[0])); i++)
 {
  struct json_object *hint = json_object_array_get_idx(result, i);
  struct json_object *pos = obj_obj(hint, "position");
  char *label;
  if (!pos)
   continue;
  label = lsp_inlay_label(hint);
  if (!label || !*label)              /* skip empty / whitespace-only hints */
  {
   free(label);
   continue;
  }
  s->inlays[s->ninlays].label = label;
  s->inlays[s->ninlays].line = obj_int(pos, "line", 0);
  s->inlays[s->ninlays].character = obj_int(pos, "character", 0);
  s->inlays[s->ninlays].kind = obj_int(hint, "kind", 0);
  out[count] = s->inlays[s->ninlays];
  s->ninlays++;
  count++;
 }
 json_object_put(resp);
 return count;
}

/* Flatten a DocumentSymbol[]/SymbolInformation[] array depth-first into syms. */
static void lsp_collect_symbols(e_lsp_session *s, struct json_object *arr,
                                e_lsp_symbol *syms, int max, int *out)
{
 int i, n = json_object_array_length(arr);

 for (i = 0; i < n && *out < max &&
             *out < (int)(sizeof(s->syms)/sizeof(s->syms[0])); i++)
 {
  struct json_object *sym = json_object_array_get_idx(arr, i);
  const char *name = obj_str(sym, "name");
  struct json_object *loc = obj_obj(sym, "location");   /* SymbolInformation */
  struct json_object *rng, *st, *children;
  const char *uri = NULL;
  if (loc)
  {
   rng = obj_obj(loc, "range");
   uri = obj_str(loc, "uri");                          /* cross-file symbol */
  }
  else                                                  /* DocumentSymbol */
  {
   rng = obj_obj(sym, "selectionRange");
   if (!rng)
    rng = obj_obj(sym, "range");
  }
  st = rng ? obj_obj(rng, "start") : NULL;
  if (name && st)
  {
   s->syms[s->nsyms].name = strdup(name);
   s->syms[s->nsyms].path = uri ? strdup(uri_to_path(uri)) : NULL;
   s->syms[s->nsyms].line = obj_int(st, "line", 0);
   s->syms[s->nsyms].character = obj_int(st, "character", 0);
   s->syms[s->nsyms].kind = obj_int(sym, "kind", 0);
   syms[*out] = s->syms[s->nsyms];
   s->nsyms++;
   (*out)++;
  }
  children = obj_obj(sym, "children");
  if (children && json_object_is_type(children, json_type_array))
   lsp_collect_symbols(s, children, syms, max, out);
 }
}

int e_lsp_document_symbols(e_lsp_session *s, const char *path,
                           e_lsp_symbol *syms, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *args, *doc, *resp, *result;
 int id, out = 0;

 if (!s)
  return -1;
 lsp_free_syms(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 id = ++s->id;
 lsp_send(s, id, "textDocument/documentSymbol", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array))
  lsp_collect_symbols(s, result, syms, max, &out);
 json_object_put(resp);
 return out;
}

int e_lsp_workspace_symbols(e_lsp_session *s, const char *query,
                            e_lsp_symbol *syms, int max)
{
 struct json_object *args, *resp, *result;
 int id, out = 0;

 if (!s)
  return -1;
 lsp_free_syms(s);
 args = json_object_new_object();
 json_object_object_add(args, "query",
                        json_object_new_string(query ? query : ""));
 id = ++s->id;
 lsp_send(s, id, "workspace/symbol", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 /* SymbolInformation[]: each carries a name + location {uri,range}, which
    lsp_collect_symbols already understands (it fills path from the uri). */
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array))
  lsp_collect_symbols(s, result, syms, max, &out);
 json_object_put(resp);
 return out;
}

/* Store one Call/TypeHierarchyItem's name + location into the session cache and
   the caller's array (both items share the same shape).  Uses
   selectionRange.start (the identifier position, not the whole-definition
   range) so a future jump lands on the name.  Returns 1 if stored, 0 if the
   array or cache is full. */
static int lsp_store_hier_item(e_lsp_session *s, struct json_object *item,
                             e_lsp_symbol *out, int max, int *n)
{
 struct json_object *sr, *st;
 const char *name, *uri;

 if (!item || *n >= max ||
     s->nsyms >= (int)(sizeof(s->syms) / sizeof(s->syms[0])))
  return 0;
 name = obj_str(item, "name");
 uri = obj_str(item, "uri");
 sr = obj_obj(item, "selectionRange");
 if (!sr)
  sr = obj_obj(item, "range");
 st = sr ? obj_obj(sr, "start") : NULL;
 s->syms[s->nsyms].name = strdup(name ? name : "?");
 s->syms[s->nsyms].path = uri ? strdup(uri_to_path(uri)) : NULL;
 s->syms[s->nsyms].line = st ? obj_int(st, "line", 0) : 0;
 s->syms[s->nsyms].character = st ? obj_int(st, "character", 0) : 0;
 s->syms[s->nsyms].kind = obj_int(item, "kind", 0);
 out[*n] = s->syms[s->nsyms];
 s->nsyms++;
 (*n)++;
 return 1;
}

/* prepareCallHierarchy: pin the symbol at (line,character) in @uri to a
   CallHierarchyItem.  Returns the item -- incref'd, so the caller owns it and
   must either json_object_put it or hand it to a request that takes ownership
   -- or NULL when the cursor is not on a callable, or the request failed. */
static struct json_object *lsp_call_hierarchy_prepare(e_lsp_session *s,
                                  const char *uri, int line, int character)
{
 struct json_object *args, *doc, *pos, *resp, *result, *item;
 int id;

 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 pos = json_object_new_object();
 json_object_object_add(pos, "line", json_object_new_int(line));
 json_object_object_add(pos, "character", json_object_new_int(character));
 json_object_object_add(args, "position", pos);
 id = ++s->id;
 lsp_send(s, id, "textDocument/prepareCallHierarchy", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 item = (result && json_object_is_type(result, json_type_array) &&
         json_object_array_length(result) > 0)
        ? json_object_get(json_object_array_get_idx(result, 0)) : NULL;
 json_object_put(resp);
 return item;
}

/* callHierarchy/incomingCalls (outgoing == 0: who calls @item) or outgoingCalls
   (outgoing != 0: what @item calls).  Ownership of @item is taken.  Fills out[]
   from each call's caller ("from") or callee ("to").  Returns the count (>=0),
   or -1 on a transport error. */
static int lsp_call_hierarchy_collect(e_lsp_session *s, struct json_object *item,
                                      int outgoing, e_lsp_symbol *out, int max)
{
 struct json_object *args, *resp, *result;
 int id, n = 0, i, len;

 args = json_object_new_object();
 json_object_object_add(args, "item", item);   /* takes ownership of our ref */
 id = ++s->id;
 lsp_send(s, id, outgoing ? "callHierarchy/outgoingCalls"
                          : "callHierarchy/incomingCalls", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array))
 {
  len = json_object_array_length(result);
  for (i = 0; i < len && n < max; i++)
  {
   struct json_object *call = json_object_array_get_idx(result, i);
   lsp_store_hier_item(s, obj_obj(call, outgoing ? "to" : "from"), out, max, &n);
  }
 }
 json_object_put(resp);
 return n;
}

int e_lsp_call_hierarchy(e_lsp_session *s, const char *path, int line,
                         int character, int outgoing,
                         e_lsp_symbol *out, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *item;

 if (!s)
  return -1;
 lsp_free_syms(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 item = lsp_call_hierarchy_prepare(s, uri, line, character);
 if (!item)
  return 0;                              /* not on a callable */
 return lsp_call_hierarchy_collect(s, item, outgoing, out, max);
}

/* prepareTypeHierarchy: pin the type at (line,character) in @uri to a
   TypeHierarchyItem.  Returns the item (incref'd; caller owns) or NULL when the
   cursor is not on a type, or the request failed. */
static struct json_object *lsp_type_hierarchy_prepare(e_lsp_session *s,
                                  const char *uri, int line, int character)
{
 struct json_object *args, *doc, *pos, *resp, *result, *item;
 int id;

 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 pos = json_object_new_object();
 json_object_object_add(pos, "line", json_object_new_int(line));
 json_object_object_add(pos, "character", json_object_new_int(character));
 json_object_object_add(args, "position", pos);
 id = ++s->id;
 lsp_send(s, id, "textDocument/prepareTypeHierarchy", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 item = (result && json_object_is_type(result, json_type_array) &&
         json_object_array_length(result) > 0)
        ? json_object_get(json_object_array_get_idx(result, 0)) : NULL;
 json_object_put(resp);
 return item;
}

/* typeHierarchy/supertypes (subtypes == 0: what @item extends) or subtypes
   (subtypes != 0: what extends @item).  Ownership of @item is taken.  The
   result is a flat TypeHierarchyItem[] (no from/to wrapper), each stored
   directly.  Returns the count (>=0), or -1 on a transport error. */
static int lsp_type_hierarchy_collect(e_lsp_session *s, struct json_object *item,
                                      int subtypes, e_lsp_symbol *out, int max)
{
 struct json_object *args, *resp, *result;
 int id, n = 0, i, len;

 args = json_object_new_object();
 json_object_object_add(args, "item", item);   /* takes ownership of our ref */
 id = ++s->id;
 lsp_send(s, id, subtypes ? "typeHierarchy/subtypes"
                          : "typeHierarchy/supertypes", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array))
 {
  len = json_object_array_length(result);
  for (i = 0; i < len && n < max; i++)
   lsp_store_hier_item(s, json_object_array_get_idx(result, i), out, max, &n);
 }
 json_object_put(resp);
 return n;
}

int e_lsp_type_hierarchy(e_lsp_session *s, const char *path, int line,
                         int character, int subtypes,
                         e_lsp_symbol *out, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *item;

 if (!s)
  return -1;
 lsp_free_syms(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 item = lsp_type_hierarchy_prepare(s, uri, line, character);
 if (!item)
  return 0;                              /* not on a type */
 return lsp_type_hierarchy_collect(s, item, subtypes, out, max);
}

/* Byte offset of LSP (line,character) within `text` (lines separated by '\n'). */
static size_t lsp_offset(const char *text, int line, int character)
{
 size_t off = 0;
 int ln = 0, c = 0;

 while (ln < line && text[off])
 {
  if (text[off] == '\n')
   ln++;
  off++;
 }
 while (c < character && text[off] && text[off] != '\n')
 {  off++;  c++;  }
 return off;
}

/* Apply a non-overlapping LSP TextEdit[] to `text`; return a new malloc'd
   string.  Edits are sorted by start offset and spliced left-to-right, so the
   result is independent of the order the server sent them. */
static char *lsp_apply_edits(const char *text, struct json_object *edits)
{
 int n = json_object_array_length(edits), i, j;
 struct { size_t start, end; const char *nt; } *ed, tmp;
 char *out;
 size_t tlen = strlen(text), cap, w = 0, pos = 0;

 if (n <= 0)
  return strdup(text);
 ed = calloc((size_t)n, sizeof(*ed));
 if (!ed)
  return NULL;
 cap = tlen + 1;
 for (i = 0; i < n; i++)
 {
  struct json_object *e = json_object_array_get_idx(edits, i);
  struct json_object *rng = obj_obj(e, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  struct json_object *en = rng ? obj_obj(rng, "end") : NULL;
  ed[i].start = st ? lsp_offset(text, obj_int(st, "line", 0),
                               obj_int(st, "character", 0)) : 0;
  ed[i].end = en ? lsp_offset(text, obj_int(en, "line", 0),
                             obj_int(en, "character", 0)) : ed[i].start;
  ed[i].nt = obj_str(e, "newText");
  if (!ed[i].nt)
   ed[i].nt = "";
  cap += strlen(ed[i].nt);
 }
 for (i = 1; i < n; i++)            /* insertion sort by start offset */
  for (j = i; j > 0 && ed[j].start < ed[j - 1].start; j--)
  {  tmp = ed[j];  ed[j] = ed[j - 1];  ed[j - 1] = tmp;  }

 out = malloc(cap);
 if (!out)
 {  free(ed);  return NULL;  }
 for (i = 0; i < n; i++)
 {
  size_t ntl;
  if (ed[i].start < pos || ed[i].end > tlen)
   continue;                       /* overlap/out-of-range: skip safely */
  memcpy(out + w, text + pos, ed[i].start - pos);
  w += ed[i].start - pos;
  ntl = strlen(ed[i].nt);
  memcpy(out + w, ed[i].nt, ntl);
  w += ntl;
  pos = ed[i].end;
 }
 memcpy(out + w, text + pos, tlen - pos);
 w += tlen - pos;
 out[w] = '\0';
 free(ed);
 return out;
}

/* Apply a WorkspaceEdit's edits for `myuri` only to `current_text`, counting the
   OTHER files it would touch into *other_files.  Handles both encodings the spec
   allows: the flat "changes" map and the ordered "documentChanges" array.
   Returns new text (malloc'd), or a copy of current_text if this file is
   untouched, or NULL on allocation failure. */
static char *lsp_apply_workspace_edit(struct json_object *wedit, const char *myuri,
                                      const char *current_text, int *other_files)
{
 struct json_object *changes, *myedits = NULL;
 int others = 0;

 if (other_files)
  *other_files = 0;
 if (wedit && (changes = obj_obj(wedit, "changes")))
 {
  json_object_object_foreach(changes, key, val)
  {
   if (!strcmp(key, myuri))
    myedits = val;
   else
    others++;
  }
 }
 else if (wedit)
 {
  struct json_object *dc = obj_obj(wedit, "documentChanges");
  int i, dn = (dc && json_object_is_type(dc, json_type_array))
              ? json_object_array_length(dc) : 0;
  for (i = 0; i < dn; i++)
  {
   struct json_object *el = json_object_array_get_idx(dc, i);
   const char *uri = obj_str(obj_obj(el, "textDocument"), "uri");
   if (uri && !strcmp(uri, myuri))
    myedits = obj_obj(el, "edits");
   else
    others++;
  }
 }
 if (other_files)
  *other_files = others;
 if (myedits && json_object_is_type(myedits, json_type_array))
  return lsp_apply_edits(current_text, myedits);
 return strdup(current_text);          /* nothing in this file */
}

char *e_lsp_format(e_lsp_session *s, const char *path, const char *current_text)
{
 char abspath[PATH_MAX];
 struct json_object *args, *doc, *opts, *resp, *result;
 char *out = NULL;
 int id;

 if (!s)
  return NULL;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 args = json_object_new_object();
 doc = json_object_new_object();
 { char uri[PATH_MAX + 8];
   snprintf(uri, sizeof(uri), "file://%s", abspath);
   json_object_object_add(doc, "uri", json_object_new_string(uri)); }
 json_object_object_add(args, "textDocument", doc);
 opts = json_object_new_object();
 json_object_object_add(opts, "tabSize", json_object_new_int(2));
 json_object_object_add(opts, "insertSpaces", json_object_new_boolean(1));
 json_object_object_add(args, "options", opts);
 id = ++s->id;
 lsp_send(s, id, "textDocument/formatting", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_array) &&
     json_object_array_length(result) > 0)
  out = lsp_apply_edits(current_text, result);
 json_object_put(resp);
 return out;
}

char *e_lsp_rename(e_lsp_session *s, const char *path, int line, int character,
                   const char *new_name, const char *current_text,
                   int *other_files)
{
 char abspath[PATH_MAX], myuri[PATH_MAX + 8];
 struct json_object *args, *resp, *result;
 char *out = NULL;
 int id, others = 0;

 if (other_files)
  *other_files = 0;
 if (!s)
  return NULL;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(myuri, sizeof(myuri), "file://%s", abspath);
 args = lsp_text_pos(s, abspath, line, character);
 json_object_object_add(args, "newName", json_object_new_string(new_name));
 id = ++s->id;
 lsp_send(s, id, "textDocument/rename", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 out = lsp_apply_workspace_edit(result, myuri, current_text, &others);
 if (other_files)
  *other_files = others;
 json_object_put(resp);
 return out;
}

static void lsp_free_acts(e_lsp_session *s)
{
 int i;
 for (i = 0; i < s->nacts; i++)
  free(s->acts[i].title);
 s->nacts = 0;
 if (s->acts_raw)
 {
  json_object_put(s->acts_raw);
  s->acts_raw = NULL;
 }
}

/* Build {start,end} both at (line,character) -- a zero-width range at the cursor,
   which is what "what can I do here?" code-action queries use. */
static struct json_object *lsp_point_range(int line, int character)
{
 struct json_object *rng = json_object_new_object();
 struct json_object *st = json_object_new_object();
 struct json_object *en = json_object_new_object();
 json_object_object_add(st, "line", json_object_new_int(line));
 json_object_object_add(st, "character", json_object_new_int(character));
 json_object_object_add(en, "line", json_object_new_int(line));
 json_object_object_add(en, "character", json_object_new_int(character));
 json_object_object_add(rng, "start", st);
 json_object_object_add(rng, "end", en);
 return rng;
}

int e_lsp_code_actions(e_lsp_session *s, const char *path, int line, int character,
                       e_lsp_code_action *acts, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *args, *doc, *ctx, *resp, *result;
 int id, i, n, out = 0;

 if (!s)
  return -1;
 lsp_free_acts(s);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 json_object_object_add(args, "range", lsp_point_range(line, character));
 ctx = json_object_new_object();
 json_object_object_add(ctx, "diagnostics", json_object_new_array());
 json_object_object_add(args, "context", ctx);
 id = ++s->id;
 lsp_send(s, id, "textDocument/codeAction", args);
 resp = lsp_pump(s, id, NULL, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 n = (result && json_object_is_type(result, json_type_array))
     ? json_object_array_length(result) : 0;
 /* Keep the whole array alive so e_lsp_apply_code_action can re-read the
    chosen action's WorkspaceEdit without a second round-trip. */
 if (result && n > 0)
  s->acts_raw = json_object_get(result);
 for (i = 0; i < n && out < max &&
             out < (int)(sizeof(s->acts)/sizeof(s->acts[0])); i++)
 {
  struct json_object *a = json_object_array_get_idx(result, i);
  const char *title = obj_str(a, "title");        /* CodeAction or Command */
  if (!title)
   continue;
  s->acts[s->nacts].title = strdup(title);
  s->acts[s->nacts].has_edit = obj_obj(a, "edit") ? 1 : 0;
  acts[out] = s->acts[s->nacts];
  s->nacts++;
  out++;
 }
 json_object_put(resp);
 return out;
}

char *e_lsp_apply_code_action(e_lsp_session *s, int index, const char *path,
                              const char *current_text, int *other_files)
{
 char abspath[PATH_MAX], myuri[PATH_MAX + 8];
 struct json_object *a, *edit;

 if (other_files)
  *other_files = 0;
 if (!s || !s->acts_raw || index < 0 ||
     index >= json_object_array_length(s->acts_raw))
  return NULL;
 a = json_object_array_get_idx(s->acts_raw, index);
 edit = obj_obj(a, "edit");
 if (!edit)
  return NULL;                  /* command-based action: not run yet */
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(myuri, sizeof(myuri), "file://%s", abspath);
 return lsp_apply_workspace_edit(edit, myuri, current_text, other_files);
}

void e_lsp_close(e_lsp_session *s)
{
 if (!s)
  return;
 if (s->in_fd >= 0)
 {
  int id = ++s->id;
  struct json_object *r;
  lsp_send(s, id, "shutdown", NULL);
  r = lsp_pump(s, id, NULL, 3000);   /* best-effort */
  if (r)
   json_object_put(r);
  lsp_notify(s, "exit", NULL);
 }
 if (s->in_fd >= 0)
  close(s->in_fd);
 if (s->out_fd >= 0)
  close(s->out_fd);
 if (s->pid > 0)
 {
  int st;
  kill(s->pid, SIGTERM);
  if (waitpid(s->pid, &st, WNOHANG) == 0)
  {
   struct timespec ts = { 0, 300 * 1000 * 1000 };  /* JVM may be slow */
   nanosleep(&ts, NULL);
   if (waitpid(s->pid, &st, WNOHANG) == 0)
   {
    kill(s->pid, SIGKILL);
    waitpid(s->pid, &st, 0);
   }
  }
 }
 lsp_free_items(s);
 lsp_free_locs(s);
 lsp_free_syms(s);
 lsp_free_acts(s);
 lsp_free_lenses(s);
 lsp_free_inlays(s);
 e_dap_reader_free(&s->rd);
 free(s);
}

/* e_lsp_join_lines - serialize editor lines into the document body sent to the
 * language server (didOpen / didChange).  Each lines[i] is a NUL-terminated
 * string whose logical content ends at the first newline -- the editor stores
 * WPE_WR (== '\n', 0x0A) as the in-buffer line terminator, so a line may still
 * carry it -- or at NUL; a NULL element counts as an empty line.  EXACTLY ONE
 * '\n' is emitted per line.  This single-delimiter invariant is what the server
 * relies on to map a (line, character) position to a token: doubling the
 * delimiter shifts every subsequent line and makes hover/definition land on the
 * wrong place (the 1.6.x buffer-newline regression this guards against).
 *
 * Returns a malloc'd, NUL-terminated buffer of @n lines, each '\n'-terminated
 * (so the empty buffer yields ""), or NULL on allocation failure.  Caller frees.
 */
char *e_lsp_join_lines(char *const *lines, int n)
{
 size_t total = 1, off = 0;
 char *buf;
 int i;

 if (n < 0)
  n = 0;
 for (i = 0; i < n; i++)
 {
  const char *s = (lines && lines[i]) ? lines[i] : "";
  total += strcspn(s, "\n") + 1;          /* content up to first '\n', plus one */
 }
 buf = malloc(total);
 if (!buf)
  return(NULL);
 for (i = 0; i < n; i++)
 {
  const char *s = (lines && lines[i]) ? lines[i] : "";
  size_t len = strcspn(s, "\n");
  if (len)
   memcpy(buf + off, s, len);
  off += len;
  buf[off++] = '\n';                       /* exactly one delimiter per line */
 }
 buf[off] = '\0';
 return(buf);
}

/* e_lsp_doc_is_new - dedup gate for a pushed display document (the Metals
 * Doctor).  Metals re-PUSHES the same Doctor on every build/index event, so
 * without this the editor would re-render an identical report and re-emit the
 * "Doctor updated" note in Messages on each push -- noise the user sees as the
 * report flickering and the note repeating.  @last points at the caller's
 * stored copy of the last body shown (a malloc'd string the caller owns via
 * this function; pass the address of a static char* initialized to NULL).
 *
 * Returns 1 when @body differs from the last one shown -- and ADOPTS it, so the
 * caller should render -- or 0 when it is identical (or @body is NULL), meaning
 * the caller should ignore this push.  On adoption the previous *last is freed
 * and replaced with strdup(@body); if that strdup fails *last is left NULL and
 * 1 is still returned (render once more rather than silently drop a new
 * report).  Pure string logic, no editor state -- unit-tested. */
int e_lsp_doc_is_new(char **last, const char *body)
{
 if (!body)
  return(0);
 if (*last && !strcmp(*last, body))
  return(0);                                /* identical to the last one shown */
 free(*last);
 *last = strdup(body);
 return(1);
}
