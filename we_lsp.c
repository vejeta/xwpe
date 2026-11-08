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

#include "we_lsp.h"
#include "we_child.h"
#include "we_dap_proto.h"        /* e_dap_reader: shared Content-Length framing */
#include "we_fdloop.h"           /* wpe_fd_readable_now */

#define LSP_TMO_REQ    30000     /* a request still waited for in lsp_pump */
#define LSP_SERVER_WHAT "the language server"   /* in the owner's lines */

/* Requests answered on the loop. */
#define LSP_PENDING_MAX   16     /* requests out at once, the start included */
#define LSP_METHOD_MAX    64     /* "textDocument/semanticTokens/full" fits   */
#define LSP_NO_ID         (-1)
#define LSP_PEND_FREE     0      /* the slot is free                          */
#define LSP_PEND_CALL     1      /* a request: waiting line and deadline      */
#define LSP_PEND_START    2      /* `initialize`: waiting line, no deadline   */
#define LSP_PEND_QUIET    3      /* `shutdown`: nobody waits, no line         */
/* What the deadline leaves on the status row. */
#define LSP_TIMEOUT_FMT   "%s did not answer `%s` within %d s: Alt-Q asks again"
#define LSP_DEFAULT_NAME  "the language server"

/* One request out: its id, what it asked, since when, and who is told. */
typedef struct {
 int               kind;          /* LSP_PEND_*                               */
 int               id;            /* the JSON-RPC id its answer carries        */
 char              method[LSP_METHOD_MAX];
 long              sent_ms;       /* the host's clock when it went out         */
 e_lsp_answered_fn fn;            /* told exactly once, or NULL                */
 void             *ctx;
} lsp_pending;

struct e_lsp_session {
 WpeChild     server;            /* the server's process, the owner's to end */
 int          in_fd;             /* write requests to the server's stdin  */
 int          out_fd;            /* read messages from the server's stdout */
 e_dap_reader rd;
 char         lang_id[16];       /* LSP languageId for didOpen ("scala", "c", "cpp") */
 int          id;                /* JSON-RPC id counter                   */
 int          started;           /* 0 = awaiting initialize response,      */
                                  /* 1 = handshake done (server is ready)   */
 int          dead;              /* 1 = the server closed its stdout (EOF),
                                    or its input was lost (input_lost)    */
 int          input_lost;        /* a message could not be handed to it:
                                    it stopped reading, or closed its input */
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
 /* A command-based code action is run with workspace/executeCommand; the server
    then applies the change by sending workspace/applyEdit BACK to us.  While that
    one synchronous command is in flight these hold a working copy of the current
    file's text that the applyEdit handler edits, so the result can be returned to
    the host afterwards.  pend_text is NULL outside that window (an unsolicited
    applyEdit is then declined with applied=false). */
 char        *pend_text;            /* working buffer the applyEdit handler edits */
 char         pend_uri[PATH_MAX + 8]; /* file:// uri of the buffer being edited   */
 int          pend_others;          /* edits applyEdit aimed at OTHER files       */
 int          pend_applied;         /* server sent at least one applyEdit          */
 /* The most recent publishDiagnostics array (and the file it belongs to), kept
    so a codeAction request can pass the diagnostics covering the cursor in
    context.diagnostics -- a server only offers the matching quick-fixes when it
    sees them there.  Replaced on every publish; NULL until the first one. */
 struct json_object *diag_raw;
 char         diag_uri[PATH_MAX + 8];
 e_lsp_code_lens lenses[64];        /* engine-owned code-lens result        */
 int          nlenses;
 e_lsp_inlay_hint inlays[512];      /* engine-owned inlay-hint result        */
 int          ninlays;
 char        *sem_legend[128];      /* semantic-token type names (from init)  */
 int          sem_nlegend;
 /* Requests answered on the loop, by id. */
 lsp_pending  pending[LSP_PENDING_MAX];
 const E_D_REQ_IO *rio;             /* the host's clock, timers and lines, or NULL */
 int          deadline_s;           /* a request is given up after this      */
 int          head_id;              /* the request the timers count for      */
 int          waiting_shown;        /* its waiting line is on the status row */
 int          ending;               /* e_lsp_close runs: tell nobody twice    */
 /* The C tests' "has this file compiled?": batches published for one path. */
 char         diag_watch[PATH_MAX + 8];
 int          diag_watch_hits;
};

/* ---- transport --------------------------------------------------------- */

/**
 * lsp_write_all - Hand one whole message (header and body) to the server.
 * @s: the session; @buf, @n: the framed message.
 *
 * The message is written now as far as the server reads, and the rest is
 * written by the loop, behind any message still waiting, so the editor never
 * waits for a server that is busy indexing and the user keeps typing; the
 * status row names the server once it has read nothing for a while.  A
 * message the server can no longer be given (its queue is full, or it
 * closed its input) would leave it with a view of the files the editor does
 * not share, so the session is given up as a server that ended: the user is
 * told, and Alt-Q starts a fresh one.
 * Return: 0 when the message is on its way, -1 when the session was lost.
 */
static int lsp_write_all(e_lsp_session *s, const char *buf, size_t n)
{
 int r;

 if (s->input_lost)
  return -1;
 r = wpe_fd_write(s->in_fd, buf, n, LSP_SERVER_WHAT, NULL);
 if (r == WPE_FD_WRITE_SENT || r == WPE_FD_WRITE_QUEUED)
  return 0;
 s->input_lost = 1;
 s->dead = 1;
 return -1;
}

/* Apply a WorkspaceEdit to one file's text (defined far below, with the rest of
   the edit math); forward-declared so the workspace/applyEdit request handler can
   reach it. */
static char *lsp_apply_workspace_edit(struct json_object *wedit, const char *myuri,
                                      const char *current_text, int *other_files);

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
 rc = lsp_write_all(s, frame, (size_t)rc);
 free(frame);
 json_object_put(msg);
 return rc;
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
  lsp_write_all(s, frame, (size_t)rc);
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

/* The two descriptors a message wait watches: the server's output, and its
   input while messages for it are still queued. */
#define LSP_WAIT_OUT 0
#define LSP_WAIT_IN  1

/**
 * lsp_wait_set - Fill the poll() set of a message wait.
 * @s: the session; @p: room for two entries.
 * Return: the entries used.
 *
 * The request being waited on may still be queued behind a server that
 * read slowly; the wait writes it on as the server takes it, so its answer
 * can come.
 */
static int lsp_wait_set(e_lsp_session *s, struct pollfd *p)
{
 p[LSP_WAIT_OUT].fd = s->out_fd;
 p[LSP_WAIT_OUT].events = POLLIN;
 p[LSP_WAIT_OUT].revents = 0;
 if (wpe_fd_write_pending(s->in_fd) <= 0)
  return 1;
 p[LSP_WAIT_IN].fd = s->in_fd;
 p[LSP_WAIT_IN].events = POLLOUT;
 p[LSP_WAIT_IN].revents = 0;
 return 2;
}

/**
 * lsp_wait_flush - Write on what the server took of its queue; a server
 * that closed its input loses the session.
 */
static void lsp_wait_flush(e_lsp_session *s)
{
 int r = wpe_fd_write_flush(s->in_fd);

 if (r == WPE_FD_WRITE_CLOSED || r == WPE_FD_WRITE_REFUSED)
 {
  s->input_lost = 1;
  s->dead = 1;
 }
}

static struct json_object *lsp_read_message(e_lsp_session *s, int timeout_ms)
{
 long deadline = lsp_now_ms() + timeout_ms;
 for (;;)
 {
  struct pollfd p[2];
  char buf[8192];
  ssize_t n;
  int err = 0, left, np;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);

  if (m)
   return m;
  if (s->dead)
   return NULL;
  left = (int)(deadline - lsp_now_ms());
  if (left <= 0)
   return NULL;
  np = lsp_wait_set(s, p);
  if (poll(p, np, left) <= 0)
   return NULL;
  if (np > LSP_WAIT_IN && p[LSP_WAIT_IN].revents)
   lsp_wait_flush(s);
  if (!p[LSP_WAIT_OUT].revents)
   continue;
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
 if (s->diag_raw)
  json_object_put(s->diag_raw);             /* keep only the latest set */
 s->diag_raw = json_object_get(diags);
 snprintf(s->diag_uri, sizeof(s->diag_uri), "%s", uri);
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
 if (s->diag_watch[0] && !strcmp(uri_to_path(uri), s->diag_watch))
  s->diag_watch_hits++;
}

void e_lsp_diagnostics_watch(e_lsp_session *s, const char *path)
{
 char abspath[PATH_MAX];

 if (!s)
  return;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(s->diag_watch, sizeof(s->diag_watch), "%s", abspath);
 s->diag_watch_hits = 0;
}

int e_lsp_diagnostics_seen(const e_lsp_session *s)
{
 return s ? s->diag_watch_hits : 0;
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
 else if (meth && !strcmp(meth, "workspace/inlayHint/refresh"))
 {
  /* "re-query your inlay hints": ack, then let the editor refetch (the server
     sends this once indexing fills in cross-file inferred types). */
  lsp_reply(s, id, NULL);
  if (s->host.on_inlay_refresh)
   s->host.on_inlay_refresh(s->host.ud);
 }
 else if (meth && !strcmp(meth, "workspace/semanticTokens/refresh"))
 {
  /* "re-query your semantic tokens": ack, then let the editor refetch. */
  lsp_reply(s, id, NULL);
  if (s->host.on_semantic_refresh)
   s->host.on_semantic_refresh(s->host.ud);
 }
 else if (meth && !strcmp(meth, "workspace/applyEdit"))
 {
  /* The server (mid workspace/executeCommand for a command-based code action)
     asks us to apply a WorkspaceEdit.  Apply it to the working buffer set up by
     lsp_execute_command_collect and answer {applied:true}; if no command is in
     flight (unsolicited edit) decline with {applied:false} -- we never rewrite a
     file the user is not actively acting on. */
  struct json_object *edit = obj_obj(obj_obj(m, "params"), "edit");
  struct json_object *res = json_object_new_object();
  int applied = 0;

  if (s->pend_text && edit)
  {
   int others = 0;
   char *nt = lsp_apply_workspace_edit(edit, s->pend_uri, s->pend_text, &others);
   if (nt)
   {
    free(s->pend_text);
    s->pend_text = nt;
    s->pend_others += others;
    s->pend_applied = 1;
    applied = 1;
   }
  }
  json_object_object_add(res, "applied", json_object_new_boolean(applied));
  lsp_reply(s, id, res);
 }
 else
  /* registerCapability, workDoneProgress/create, other *_refresh, unknown -> null */
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

/* Handle one server notification message: diagnostics, status, the Doctor. */
static void lsp_dispatch_notification(e_lsp_session *s, struct json_object *m)
{
 const char *meth = json_object_get_string(obj_obj(m, "method"));

 if (!meth)
  return;
 if (!strcmp(meth, "textDocument/publishDiagnostics"))
  lsp_dispatch_diagnostics(s, obj_obj(m, "params"));
 else if (!strcmp(meth, "metals/executeClientCommand"))
  lsp_dispatch_client_command(s, m);
 else if (!strcmp(meth, "metals/status"))
  lsp_dispatch_status(s, m);
}

/* ---- requests answered on the loop ------------------------------------- *
 *
 * A request is registered with its id and a continuation, and its answer is
 * handed over whenever it is read -- by the loop's read (e_lsp_poll) or by
 * the synchronous pump the requests not moved yet still use -- while
 * diagnostics, status and the server's own requests read meanwhile go where
 * they always go.  The host's clock and one-shot timers count the waiting
 * line and the deadline of the oldest request out; a request given up on
 * frees its slot, so an answer that comes after it is dropped.
 */

/**
 * lsp_now - The host's clock, or 0 when the host gave none.
 * @s: the session.
 */
static long lsp_now(const e_lsp_session *s)
{
 if (!s->rio || !s->rio->now_ms)
  return 0;
 return s->rio->now_ms(s->rio->io);
}

/**
 * lsp_arm - Ask the host for timer @which in @ms, or for none.
 * @s: the session; @which: E_D_TIMER_*; @ms: the delay, or E_D_TIMER_OFF.
 */
static void lsp_arm(const e_lsp_session *s, int which, long ms)
{
 if (!s->rio || !s->rio->arm_timer)
  return;
 s->rio->arm_timer(which, ms, s->rio->io);
}

/**
 * lsp_disarm_all - Take back every timer the requests asked for.
 * @s: the session.
 */
static void lsp_disarm_all(e_lsp_session *s)
{
 int which;

 for (which = 0; which < E_D_TIMERS; which++)
  lsp_arm(s, which, E_D_TIMER_OFF);
}

/**
 * lsp_is_waited - Does the user wait for the request in slot @p?  The
 * start and every request do; a `shutdown` nobody reads does not.
 */
static int lsp_is_waited(const lsp_pending *p)
{
 return p->kind == LSP_PEND_CALL || p->kind == LSP_PEND_START;
}

/**
 * lsp_oldest - The request out that went first, or NULL when none is out.
 * @s: the session.
 */
static lsp_pending *lsp_oldest(e_lsp_session *s)
{
 lsp_pending *oldest = NULL;
 int i;

 for (i = 0; i < LSP_PENDING_MAX; i++)
 {
  lsp_pending *p = &s->pending[i];

  if (!lsp_is_waited(p))
   continue;
  if (!oldest || p->id < oldest->id)
   oldest = p;
 }
 return oldest;
}

/**
 * lsp_find - The slot of the request whose answer carries @id, or NULL.
 * @s: the session; @id: the response's id.
 */
static lsp_pending *lsp_find(e_lsp_session *s, int id)
{
 int i;

 for (i = 0; i < LSP_PENDING_MAX; i++)
  if (s->pending[i].kind != LSP_PEND_FREE && s->pending[i].id == id)
   return &s->pending[i];
 return NULL;
}

/**
 * lsp_free_slot - A slot no request uses, or NULL when all are taken.
 * @s: the session.
 */
static lsp_pending *lsp_free_slot(e_lsp_session *s)
{
 int i;

 for (i = 0; i < LSP_PENDING_MAX; i++)
  if (s->pending[i].kind == LSP_PEND_FREE)
   return &s->pending[i];
 return NULL;
}

/**
 * lsp_release - Free slot @p; an answer that still comes for it is dropped.
 */
static void lsp_release(lsp_pending *p)
{
 memset(p, 0, sizeof(*p));
 p->kind = LSP_PEND_FREE;
 p->id = LSP_NO_ID;
}

/**
 * lsp_ms_after - What is left of @ms counted from @since, never below 0.
 */
static long lsp_ms_after(const e_lsp_session *s, long since, long ms)
{
 long left = since + ms - lsp_now(s);

 if (left < 0)
  return 0;
 return left;
}

/**
 * lsp_calls_time - Count the waiting line and the deadline for the oldest
 * request out, once it changed.
 * @s: the session.
 *
 * The user is told what the editor waits for after a quiet second, and a
 * request is given up at its deadline, counted from when it went out, as a
 * debugger's answer is.  The start has no deadline: a server may import a
 * build for minutes, and the user ends it with Esc.  With nothing out every
 * timer is taken back.
 */
static void lsp_calls_time(e_lsp_session *s)
{
 lsp_pending *p = lsp_oldest(s);

 if (!p)
 {
  s->head_id = LSP_NO_ID;
  s->waiting_shown = 0;
  lsp_disarm_all(s);
  return;
 }
 if (p->id == s->head_id)
  return;
 s->head_id = p->id;
 s->waiting_shown = 0;
 lsp_arm(s, E_D_TIMER_SECOND, E_D_TIMER_OFF);
 lsp_arm(s, E_D_TIMER_QUIET, lsp_ms_after(s, p->sent_ms, E_D_REQ_QUIET_MS));
 if (p->kind == LSP_PEND_CALL)
  lsp_arm(s, E_D_TIMER_DEADLINE,
          lsp_ms_after(s, p->sent_ms, (long)s->deadline_s * E_D_REQ_SECOND_MS));
 else
  lsp_arm(s, E_D_TIMER_DEADLINE, E_D_TIMER_OFF);
}

/**
 * lsp_answered - The response @m to the request in slot @p has come.
 * @s: the session; @p: its slot; @m: the response.
 *
 * The slot is free before the owner is told, so the owner may send its
 * next request at once.
 */
static void lsp_answered(e_lsp_session *s, lsp_pending *p, struct json_object *m)
{
 e_lsp_answered_fn fn = p->fn;
 void *ctx = p->ctx;

 lsp_trace_response(p->id, m);
 lsp_release(p);
 lsp_calls_time(s);
 if (fn)
  fn(E_D_REQ_DONE, m, ctx);
}

/* The owners an end tells, oldest first. */
typedef struct {
 int id;
 e_lsp_answered_fn fn;
 void *ctx;
} lsp_told;

/**
 * lsp_told_sort - Order the owners an end tells by their requests' ids.
 */
static void lsp_told_sort(lsp_told *t, int n)
{
 int i, j;

 for (i = 1; i < n; i++)
  for (j = i; j > 0 && t[j - 1].id > t[j].id; j--)
  {
   lsp_told x = t[j];

   t[j] = t[j - 1];
   t[j - 1] = x;
  }
}

void e_lsp_requests_cancel(e_lsp_session *s, E_D_REQ_END why)
{
 lsp_told told[LSP_PENDING_MAX];
 int i, n = 0;

 if (!s)
  return;
 for (i = 0; i < LSP_PENDING_MAX; i++)
 {
  lsp_pending *p = &s->pending[i];

  if (p->kind == LSP_PEND_FREE)
   continue;
  told[n].id = p->id;
  told[n].fn = p->fn;
  told[n].ctx = p->ctx;
  n++;
  lsp_release(p);
 }
 lsp_calls_time(s);
 lsp_told_sort(told, n);
 for (i = 0; i < n; i++)
  if (told[i].fn)
   told[i].fn(why, NULL, told[i].ctx);
}

void e_lsp_requests_init(e_lsp_session *s, const E_D_REQ_IO *io,
                         const char *timeout_text)
{
 int i;

 if (!s)
  return;
 s->rio = io;
 s->deadline_s = e_d_req_timeout_from_text(timeout_text);
 s->head_id = LSP_NO_ID;
 /* What went out before there was a clock (`initialize`, sent as the server
    was spawned) is counted from now. */
 for (i = 0; i < LSP_PENDING_MAX; i++)
  if (s->pending[i].kind != LSP_PEND_FREE)
   s->pending[i].sent_ms = lsp_now(s);
 lsp_calls_time(s);
}

/**
 * lsp_say_given_up - The sentence the deadline leaves on the status row:
 * which request was not answered, within how long, and what to do.
 */
static void lsp_say_given_up(const e_lsp_session *s, const lsp_pending *p)
{
 char msg[E_D_REQ_MSG_MAX];
 const char *name = s->rio->name ? s->rio->name : LSP_DEFAULT_NAME;

 if (!s->rio->notice)
  return;
 snprintf(msg, sizeof msg, LSP_TIMEOUT_FMT, name, p->method, s->deadline_s);
 s->rio->notice(msg, s->rio->io);
}

/**
 * lsp_say_waiting - Show the waiting line for the oldest request out.
 */
static void lsp_say_waiting(e_lsp_session *s, const lsp_pending *p)
{
 long secs = (lsp_now(s) - p->sent_ms) / E_D_REQ_SECOND_MS;

 s->waiting_shown = 1;
 if (s->rio->waiting)
  s->rio->waiting(p->method, secs, s->rio->io);
 lsp_arm(s, E_D_TIMER_SECOND, E_D_REQ_SECOND_MS);
}

void e_lsp_request_timer(e_lsp_session *s, int which)
{
 lsp_pending *p;

 if (!s || !s->rio)
  return;
 p = lsp_find(s, s->head_id);
 if (!p || !lsp_is_waited(p))
  return;                       /* a timer of a request that has ended */
 if (which == E_D_TIMER_QUIET)
  lsp_say_waiting(s, p);
 else if (which == E_D_TIMER_SECOND && s->waiting_shown)
  lsp_say_waiting(s, p);
 else if (which == E_D_TIMER_DEADLINE && p->kind == LSP_PEND_CALL)
 {
  lsp_say_given_up(s, p);
  e_lsp_requests_cancel(s, E_D_REQ_TIMED_OUT);
 }
}

int e_lsp_requests_pending(const e_lsp_session *s)
{
 int i;

 if (!s)
  return 0;
 for (i = 0; i < LSP_PENDING_MAX; i++)
  if (lsp_is_waited(&s->pending[i]))
   return 1;
 return 0;
}

const char *e_lsp_request_waited(const e_lsp_session *s, long *secs)
{
 lsp_pending *p;

 if (!s)
  return NULL;
 p = lsp_oldest((e_lsp_session *)s);
 if (!p)
  return NULL;
 if (secs)
  *secs = (lsp_now(s) - p->sent_ms) / E_D_REQ_SECOND_MS;
 return p->method;
}

/**
 * lsp_request_kind - Send @method with @params as a request of @kind whose
 * answer goes to @fn.
 * @s: the session; @kind: LSP_PEND_*; @method, @params: the request
 * (@params taken over, may be NULL); @fn, @ctx: the continuation.
 * Return: E_D_SEND_OK, or why it was not sent (@fn is then never called).
 */
static int lsp_request_kind(e_lsp_session *s, int kind, const char *method,
                            struct json_object *params, e_lsp_answered_fn fn,
                            void *ctx)
{
 lsp_pending *p;

 if (!s || s->dead || s->input_lost || s->ending)
 {
  if (params)
   json_object_put(params);
  return E_D_SEND_GONE;
 }
 if (!method || strlen(method) >= LSP_METHOD_MAX)
 {
  if (params)
   json_object_put(params);
  return E_D_SEND_TOO_LONG;
 }
 p = lsp_free_slot(s);
 if (!p)
 {
  if (params)
   json_object_put(params);
  return E_D_SEND_FULL;
 }
 p->kind = kind;
 p->id = ++s->id;
 p->fn = fn;
 p->ctx = ctx;
 p->sent_ms = lsp_now(s);
 snprintf(p->method, sizeof p->method, "%s", method);
 if (lsp_send(s, p->id, method, params) != 0)
 {
  lsp_release(p);
  return E_D_SEND_GONE;
 }
 lsp_calls_time(s);
 return E_D_SEND_OK;
}

int e_lsp_request(e_lsp_session *s, const char *method,
                  struct json_object *params, e_lsp_answered_fn fn, void *ctx)
{
 return lsp_request_kind(s, LSP_PEND_CALL, method, params, fn, ctx);
}

/**
 * lsp_dispatch_response - A response came: hand it to the request that
 * asked, or drop it when that request was given up on.
 */
static void lsp_dispatch_response(e_lsp_session *s, struct json_object *m,
                                  struct json_object *id)
{
 lsp_pending *p = lsp_find(s, json_object_get_int(id));

 if (p)
  lsp_answered(s, p, m);
}

/**
 * lsp_dispatch - Take one message from the server, whatever is out.
 * @s: the session; @m: the message (the caller still owns it).
 *
 * A server request is answered at once, so the server never waits on the
 * editor; a notification reaches the host; a response reaches the request
 * that asked.
 */
static void lsp_dispatch(e_lsp_session *s, struct json_object *m)
{
 struct json_object *id = obj_obj(m, "id");
 struct json_object *method = obj_obj(m, "method");

 lsp_trace_message(LSP_NO_ID, id, method);
 if (id && method)
  lsp_answer_request(s, m);
 else if (method)
  lsp_dispatch_notification(s, m);
 else if (id)
  lsp_dispatch_response(s, m, id);
}

/**
 * lsp_dispatch_buffered - Hand on every whole message already read.
 * @s: the session.
 *
 * A waited request returns as soon as its own answer is found, while the
 * same read may also hold a loop request's answer.  The server then has
 * nothing more to write, so the loop would not read again and that answer
 * would sit unhandled until its deadline; it is handed to its owner now.
 */
static void lsp_dispatch_buffered(e_lsp_session *s)
{
 for (;;)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);

  if (!m)
   return;
  lsp_dispatch(s, m);
  json_object_put(m);
 }
}

/**
 * lsp_pump - Wait for the response to @want_id, serving everything else.
 * @s: the session; @want_id: the request waited on; @timeout_ms: how long.
 *
 * The requests not yet moved onto the loop wait here; a loop request's
 * answer read meanwhile still reaches its continuation, also one read
 * together with the response waited for.
 * Return: the response (the caller frees it), or NULL on a timeout or EOF.
 */
static struct json_object *lsp_pump(e_lsp_session *s, int want_id,
                                    int timeout_ms)
{
 long deadline = lsp_now_ms() + timeout_ms;
 for (;;)
 {
  int left = (int)(deadline - lsp_now_ms());
  struct json_object *m, *id;
  if (left <= 0)
   return NULL;
  m = lsp_read_message(s, left);
  if (!m)
   return NULL;
  id = obj_obj(m, "id");
  if (id && !obj_obj(m, "method") && json_object_get_int(id) == want_id)
  {
   lsp_trace_response(want_id, m);
   lsp_dispatch_buffered(s);
   return m;
  }
  lsp_dispatch(s, m);
  json_object_put(m);
 }
}

/* ---- spawn + handshake ------------------------------------------------- */

/**
 * lsp_spawn - Start the language server for a session.
 * @s: the session; @argv: the server's command line; @cwd: the project.
 *
 * In a session of its own, so a Ctrl-C typed for a script run from the
 * editor spares it; its stderr goes nowhere, since servers are chatty there
 * and the editor's screen would show it; a server that writes to a closed
 * pipe ends, as from a shell.  The editor follows its end, so closing the
 * session later never signals a number another process has by then.  Its
 * end reaches its whole group (an indexer it started goes with it), except
 * for a server whose group may hold a shared build server
 * (e_lsp_host.server_shares_daemon), which is signalled alone.
 * Return: 0, or -1 when it could not be started.
 */
static int lsp_spawn(e_lsp_session *s, char *const argv[], const char *cwd)
{
 WpeChildSpec spec;

 memset(&spec, 0, sizeof spec);
 spec.argv = argv;
 spec.cwd = cwd;
 spec.io = WPE_CHILD_IO_PIPES;
 spec.group = WPE_CHILD_OWN_SESSION;
 spec.stderr_to_null = 1;
 spec.signal_leader_only = s->host.server_shares_daemon;
 spec.what = LSP_SERVER_WHAT;
 if (wpe_child_start(&spec, &s->server, NULL, NULL) != 0)
  return -1;
 s->in_fd = s->server.in_fd;
 s->out_fd = s->server.out_fd;
 return 0;
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
 {
  /* Code actions: tell the server we can RESOLVE an action that arrives with
     only a `data` field (no edit yet) -- modern Metals returns its refactors
     that way, expecting a codeAction/resolve round-trip to fetch the edit. */
  struct json_object *ca = json_object_new_object();
  struct json_object *rs = json_object_new_object();
  struct json_object *props = json_object_new_array();
  json_object_array_add(props, json_object_new_string("edit"));
  json_object_object_add(rs, "properties", props);
  json_object_object_add(ca, "resolveSupport", rs);
  json_object_object_add(ca, "dataSupport", json_object_new_boolean(1));
  json_object_object_add(td, "codeAction", ca);
 }
 json_object_object_add(td, "inlayHint", json_object_new_object());
 json_object_object_add(td, "callHierarchy", json_object_new_object());
 json_object_object_add(td, "typeHierarchy", json_object_new_object());
 json_object_object_add(td, "selectionRange", json_object_new_object());
 {
  /* semantic tokens: advertise the standard LSP token types + the "full"
     request + the relative-delta format, so the server (Metals) sends them. */
  static const char *SEM_TYPES[] = {
   "namespace", "type", "class", "enum", "interface", "struct", "typeParameter",
   "parameter", "variable", "property", "enumMember", "event", "function",
   "method", "macro", "keyword", "modifier", "comment", "string", "number",
   "regexp", "operator", "decorator"
  };
  struct json_object *st = json_object_new_object();
  struct json_object *req = json_object_new_object();
  struct json_object *types = json_object_new_array();
  struct json_object *fmts = json_object_new_array();
  size_t i;
  json_object_object_add(req, "full", json_object_new_boolean(1));
  json_object_object_add(st, "requests", req);
  for (i = 0; i < sizeof(SEM_TYPES) / sizeof(SEM_TYPES[0]); i++)
   json_object_array_add(types, json_object_new_string(SEM_TYPES[i]));
  json_object_object_add(st, "tokenTypes", types);
  json_object_object_add(st, "tokenModifiers", json_object_new_array());
  json_object_array_add(fmts, json_object_new_string("relative"));
  json_object_object_add(st, "formats", fmts);
  json_object_object_add(td, "semanticTokens", st);
 }
 json_object_object_add(caps, "textDocument", td);
 {
  struct json_object *ws = json_object_new_object();
  struct json_object *ih = json_object_new_object();
  json_object_object_add(ws, "configuration", json_object_new_boolean(1));
  /* We honour workspace/applyEdit (the server's way of writing back a
     command-based code action's result) and can invoke workspace/executeCommand,
     so a Metals action delivered as a Command -- not a precomputed edit -- can
     actually run. */
  json_object_object_add(ws, "applyEdit", json_object_new_boolean(1));
  {
   struct json_object *ec = json_object_new_object();
   json_object_object_add(ec, "dynamicRegistration", json_object_new_boolean(0));
   json_object_object_add(ws, "executeCommand", ec);
  }
  /* ask the server to tell us when inlay hints need re-querying (it does so
     once indexing fills in cross-file inferred types). */
  json_object_object_add(ih, "refreshSupport", json_object_new_boolean(1));
  json_object_object_add(ws, "inlayHint", ih);
  {
   struct json_object *sh = json_object_new_object();
   json_object_object_add(sh, "refreshSupport", json_object_new_boolean(1));
   json_object_object_add(ws, "semanticTokens", sh);
  }
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

static void lsp_capture_semantic_legend(e_lsp_session *s, struct json_object *m);

/**
 * lsp_initialize_answered - The server answered `initialize`: the handshake
 * ends here, on the loop, and the host sees e_lsp_started() turn true.
 * @how: how the request ended; @m: the response; @ctx: the session.
 *
 * A start given up on (Esc, the server gone) never reaches started, and the
 * session is closed by whoever gave it up.
 */
static void lsp_initialize_answered(E_D_REQ_END how, struct json_object *m,
                                    void *ctx)
{
 e_lsp_session *s = ctx;

 if (how != E_D_REQ_DONE)
  return;
 lsp_capture_semantic_legend(s, m);
 lsp_notify(s, "initialized", json_object_new_object());
 s->started = 1;
}

/**
 * lsp_session_new - A session with nothing out and no server yet.
 * @rootdir: the project; @lang: the languageId; @host: the callbacks.
 */
static e_lsp_session *lsp_session_new(const char *rootdir, const char *lang,
                                      const e_lsp_host *host)
{
 e_lsp_session *s = calloc(1, sizeof(*s));
 int i;

 if (!s)
  return NULL;
 e_dap_reader_init(&s->rd);
 snprintf(s->lang_id, sizeof(s->lang_id), "%s", lang ? lang : "scala");
 if (host)
  s->host = *host;
 s->in_fd = s->out_fd = -1;
 s->started = 0;
 s->head_id = LSP_NO_ID;
 s->deadline_s = E_D_REQ_TIMEOUT_S;
 for (i = 0; i < LSP_PENDING_MAX; i++)
  lsp_release(&s->pending[i]);
 snprintf(s->root_uri, sizeof(s->root_uri), "file://%s", rootdir);
 return s;
}

/* Spawn the server and SEND `initialize`, but do NOT wait for the response:
   return immediately with the session in the "starting" state (s->started == 0).
   The answer is a request's like any other: e_lsp_poll() reads it on the loop,
   sends `initialized` and flips s->started to 1.  This
   keeps the slow JVM cold-start OFF the editor's input loop.  Returns NULL only
   on spawn failure (the handshake itself is async). */
e_lsp_session *e_lsp_open_async(char *const argv[], const char *root_dir,
                                const char *lang, const e_lsp_host *host)
{
 e_lsp_session *s;
 char rootdir[PATH_MAX];
 struct json_object *args;

 if (!realpath(root_dir, rootdir))
  snprintf(rootdir, sizeof(rootdir), "%s", root_dir);

 s = lsp_session_new(rootdir, lang, host);
 if (!s)
  return NULL;
 if (lsp_spawn(s, argv, rootdir) != 0)
 {  e_lsp_close(s);  return NULL;  }

 args = json_object_new_object();
 json_object_object_add(args, "processId", json_object_new_int((int)getpid()));
 json_object_object_add(args, "rootUri", json_object_new_string(s->root_uri));
 json_object_object_add(args, "capabilities", lsp_client_caps());
 json_object_object_add(args, "initializationOptions", lsp_init_options());
 lsp_request_kind(s, LSP_PEND_START, "initialize", args,
                  lsp_initialize_answered, s);
 return s;
}

int e_lsp_fd(e_lsp_session *s)      {  return s ? s->out_fd : -1;  }
int e_lsp_started(e_lsp_session *s) {  return s ? s->started : 0;  }
int e_lsp_dead(e_lsp_session *s) {  return s ? s->dead : 1;  }

/**
 * e_lsp_input_lost - Did the session end because the server could no
 * longer be sent a message (it stopped reading, or closed its input), and
 * not because it exited?  The line the user reads says which.
 */
int e_lsp_input_lost(e_lsp_session *s)
{
 return s && s->input_lost;
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
 char abspath[PATH_MAX];
 char uri[PATH_MAX + 8];

 if (!s)
  return -1;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(doc, "languageId",
                        json_object_new_string(s->lang_id[0] ? s->lang_id : "scala"));
 json_object_object_add(doc, "version", json_object_new_int(1));
 json_object_object_add(doc, "text", json_object_new_string(text ? text : ""));
 json_object_object_add(args, "textDocument", doc);
 lsp_notify(s, "textDocument/didOpen", args);
 s->doc_version = 1;
 return 0;
}

int e_lsp_did_focus(e_lsp_session *s, const char *path)
{
 char abspath[PATH_MAX];
 char uri[PATH_MAX + 8];

 if (!s)
  return -1;
 /* metals/didFocusTextDocument is a Metals extension; only Metals understands
    it (clangd and other servers would log it as unknown).  Skip for non-Scala. */
 if (strcmp(s->lang_id, "scala") != 0)
  return 0;
 /* same URI form as didOpen so Metals ties the focus to the open document */
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 lsp_notify(s, "metals/didFocusTextDocument", json_object_new_string(uri));
 return 0;
}

int e_lsp_did_change(e_lsp_session *s, const char *path, const char *text)
{
 struct json_object *args, *doc, *changes, *change;
 char abspath[PATH_MAX];
 char uri[PATH_MAX + 8];

 if (!s)
  return -1;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
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

/**
 * lsp_ended - The server's output closed or its input was lost: nothing
 * more will be answered, so every request out is told the server is gone
 * and no owner waits for it.
 */
static void lsp_ended(e_lsp_session *s)
{
 if (s->ending)
  return;
 e_lsp_requests_cancel(s, E_D_REQ_GONE);
}

int e_lsp_poll(e_lsp_session *s)
{
 int handled = 0, guard = 0;

 if (!s || s->out_fd < 0)
  return -1;
 if (s->dead)
 {
  lsp_ended(s);
  return -1;
 }
 /* read whatever is immediately available, without blocking */
 while (guard++ < 64)
 {
  char buf[8192];
  ssize_t n;
  if (wpe_fd_readable_now(s->out_fd) != WPE_FD_READABLE)
   break;
  n = read(s->out_fd, buf, sizeof(buf));
  if (n == 0)                  /* EOF: the server closed its stdout -- it died */
  {  s->dead = 1;  break;  }
  if (n < 0)                   /* EAGAIN/EINTR: nothing more right now */
   break;
  e_dap_reader_push(&s->rd, buf, (size_t)n);
 }
 /* dispatch every complete message now buffered (diagnostics, server requests) */
 for (;;)
 {
  int err = 0;
  struct json_object *m = e_dap_reader_pop(&s->rd, &err);
  if (!m)
   break;
  handled++;
  lsp_dispatch(s, m);   /* answers, server requests, diagnostics, the handshake */
  json_object_put(m);
 }
 if (s->dead)
  lsp_ended(s);
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

/**
 * lsp_hover_text - The plain text of a hover answer (markdown fences
 * stripped), malloc'd, or NULL when it says nothing.
 * @resp: the response.
 */
static char *lsp_hover_text(struct json_object *resp)
{
 struct json_object *result = obj_obj(resp, "result");
 struct json_object *contents = result ? obj_obj(result, "contents") : NULL;
 const char *v = NULL;

 if (!contents)
  return NULL;
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
 if (!v)
  return NULL;
 return lsp_markdown_to_text(v);
}

/**
 * lsp_abspath - The request's file as the server knows it: the real path,
 * or @path as given when it cannot be resolved.
 */
static void lsp_abspath(const char *path, char *out)
{
 if (!realpath(path, out))
  snprintf(out, PATH_MAX, "%s", path);
}

/* What a typed request keeps until its answer: the caller's continuation, and
   what the parser needs besides the response. */
typedef struct {
 e_lsp_session *s;
 void          *ctx;           /* the caller's                              */
 e_lsp_text_fn     text_fn;
 e_lsp_location_fn loc_fn;
 e_lsp_items_fn    items_fn;
 e_lsp_edit_fn     edit_fn;
 int            max;           /* completion: the most items wanted         */
 char          *base;          /* format: the text sent, edits apply to it  */
} lsp_typed;

/**
 * lsp_typed_new - Room for a typed request's continuation, or NULL.
 * @s: the session; @ctx: the caller's context.
 */
static lsp_typed *lsp_typed_new(e_lsp_session *s, void *ctx)
{
 lsp_typed *t = calloc(1, sizeof(*t));

 if (!t)
  return NULL;
 t->s = s;
 t->ctx = ctx;
 return t;
}

/**
 * lsp_typed_free - Forget a typed request once its caller was told.
 */
static void lsp_typed_free(lsp_typed *t)
{
 free(t->base);
 free(t);
}

/**
 * lsp_typed_send - Send a typed request; when it cannot go out, nobody is
 * told and its context is freed.
 * Return: what e_lsp_request returns.
 */
static int lsp_typed_send(e_lsp_session *s, const char *method,
                          struct json_object *params, e_lsp_answered_fn fn,
                          lsp_typed *t)
{
 int rc = e_lsp_request(s, method, params, fn, t);

 if (rc != E_D_SEND_OK)
  lsp_typed_free(t);
 return rc;
}

/**
 * lsp_hover_answered - Hand the caller the hover's text, or NULL.
 */
static void lsp_hover_answered(E_D_REQ_END how, struct json_object *resp,
                               void *ctx)
{
 lsp_typed *t = ctx;
 char *text = NULL;

 if (how == E_D_REQ_DONE)
  text = lsp_hover_text(resp);
 t->text_fn(how, text, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_hover_async(e_lsp_session *s, const char *path, int line,
                      int character, e_lsp_text_fn fn, void *ctx)
{
 char abspath[PATH_MAX];
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->text_fn = fn;
 lsp_abspath(path, abspath);
 return lsp_typed_send(s, "textDocument/hover",
                       lsp_text_pos(s, abspath, line, character),
                       lsp_hover_answered, t);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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

/* The answer of the three "jump to a location" requests, which differ only in
 * the method name and all answer with Location | Location[] | LocationLink[]:
 * fills out_* with the first location; returns 0 on success, -1 otherwise. */
static int lsp_location_of(struct json_object *resp, char *out_path,
                           size_t out_sz, int *out_line, int *out_char)
{
 struct json_object *result, *loc = NULL, *rng, *st;
 int rc = -1;

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
 return rc;
}

/* The method of each E_LSP_LOCATE_* request. */
static const char *const lsp_locate_methods[] = {
 "textDocument/definition",
 "textDocument/implementation",
 "textDocument/typeDefinition"
};
#define LSP_LOCATE_KINDS \
 ((int)(sizeof(lsp_locate_methods) / sizeof(lsp_locate_methods[0])))

/**
 * lsp_locate_answered - Hand the caller the first location of the answer.
 */
static void lsp_locate_answered(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 char where[PATH_MAX];
 int line = 0, character = 0, found = 0;

 where[0] = '\0';
 if (how == E_D_REQ_DONE)
  found = (lsp_location_of(resp, where, sizeof where, &line, &character) == 0);
 t->loc_fn(how, found, where, line, character, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_locate_async(e_lsp_session *s, int which, const char *path,
                       int line, int character, e_lsp_location_fn fn,
                       void *ctx)
{
 char abspath[PATH_MAX];
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 if (which < 0 || which >= LSP_LOCATE_KINDS)
  return E_D_SEND_FAILED;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->loc_fn = fn;
 lsp_abspath(path, abspath);
 return lsp_typed_send(s, lsp_locate_methods[which],
                       lsp_text_pos(s, abspath, line, character),
                       lsp_locate_answered, t);
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

/**
 * lsp_completion_fill - Keep the candidates of a completion answer in the
 * session (the previous ones are freed).
 * @s: the session; @resp: the response; @max: the most wanted.
 * Return: how many were kept.
 */
static int lsp_completion_fill(e_lsp_session *s, struct json_object *resp,
                               int max)
{
 struct json_object *result, *list = NULL;
 int i, n, out = 0;

 lsp_free_items(s);
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
  s->nitems++;
  out++;
 }
 return out;
}

/**
 * lsp_completion_answered - Hand the caller the candidates, or n = -1 when
 * the request ended unanswered.
 */
static void lsp_completion_answered(E_D_REQ_END how, struct json_object *resp,
                                    void *ctx)
{
 lsp_typed *t = ctx;
 int n = -1;

 if (how == E_D_REQ_DONE)
  n = lsp_completion_fill(t->s, resp, t->max);
 t->items_fn(how, t->s->items, n, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_completion_async(e_lsp_session *s, const char *path, int line,
                           int character, int max, e_lsp_items_fn fn,
                           void *ctx)
{
 char abspath[PATH_MAX];
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->items_fn = fn;
 t->max = max;
 lsp_abspath(path, abspath);
 return lsp_typed_send(s, "textDocument/completion",
                       lsp_text_pos(s, abspath, line, character),
                       lsp_completion_answered, t);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
   rresp = lsp_pump(s, rid, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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

/* Read a {start:{line,character}, end:{...}} range object into `r`. */
static void lsp_read_range(struct json_object *range, e_lsp_range *r)
{
 struct json_object *st = range ? obj_obj(range, "start") : NULL;
 struct json_object *en = range ? obj_obj(range, "end") : NULL;

 r->start_line = st ? obj_int(st, "line", 0) : 0;
 r->start_char = st ? obj_int(st, "character", 0) : 0;
 r->end_line = en ? obj_int(en, "line", 0) : 0;
 r->end_char = en ? obj_int(en, "character", 0) : 0;
}

int e_lsp_selection_range(e_lsp_session *s, const char *path, int line,
                          int character, e_lsp_range *out, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *args, *doc, *positions, *pos, *resp, *result, *node;
 int id, n = 0;

 if (!s)
  return -1;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);

 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 pos = json_object_new_object();
 json_object_object_add(pos, "line", json_object_new_int(line));
 json_object_object_add(pos, "character", json_object_new_int(character));
 positions = json_object_new_array();
 json_object_array_add(positions, pos);
 json_object_object_add(args, "positions", positions);
 id = ++s->id;
 lsp_send(s, id, "textDocument/selectionRange", args);
 resp = lsp_pump(s, id, LSP_TMO_REQ);
 if (!resp)
  return -1;
 /* result is SelectionRange[] (one per requested position); follow the .parent
    chain of the first, flattening innermost -> outermost into out[]. */
 result = obj_obj(resp, "result");
 node = (result && json_object_is_type(result, json_type_array) &&
         json_object_array_length(result) > 0)
        ? json_object_array_get_idx(result, 0) : NULL;
 while (node && n < max)
 {
  lsp_read_range(obj_obj(node, "range"), &out[n]);
  n++;
  node = obj_obj(node, "parent");
 }
 json_object_put(resp);
 return n;
}

/* ---- semantic tokens --------------------------------------------------- */

int e_lsp_semantic_decode(const int *data, int n, e_lsp_sem_token *out, int max)
{
 int i, count = 0, line = 0, start = 0;

 if (!data || n < 0)
  return 0;
 for (i = 0; i + 4 < n; i += 5)         /* groups of 5; ignore a short tail */
 {
  int dline = data[i], dchar = data[i + 1];
  if (dline == 0)                       /* same line: char delta is relative */
   start += dchar;
  else
  {  line += dline;  start = dchar;  }  /* new line: char delta is absolute  */
  if (count >= max)
   continue;                            /* keep advancing line/start, just skip */
  out[count].line = line;
  out[count].start = start;
  out[count].length = data[i + 2];
  out[count].type = data[i + 3];
  out[count].modifiers = data[i + 4];
  count++;
 }
 return count;
}

const char *e_lsp_semantic_legend(e_lsp_session *s, int type)
{
 if (!s || type < 0 || type >= s->sem_nlegend)
  return NULL;
 return s->sem_legend[type];
}

/* Semantic-token truecolor palette (1.6.6): slot -> 24-bit RGB.  Kept tiny on
   purpose -- only the categories the 16-colour palette cannot express well.
   Slot 0 is the headline: method/function/macro want a warm orange that reads
   on the Borland-blue editor background, where the nearest 16-colour choice was
   bright red.  Add slots here (and a branch in e_lsp_sem_truecolor) to give
   more categories a dedicated colour; renderers pick them up automatically. */
static const struct { int r, g, b; } e_lsp_sem_tc_palette[] = {
 { 0xFF, 0x8C, 0x00 },   /* slot 0: method / function / macro -- orange */
 { 0x4E, 0xC9, 0xB0 }    /* slot 1: type / class / ... -- teal           */
};
#define LSP_SEM_TC_COUNT ((int)(sizeof(e_lsp_sem_tc_palette) / \
                                sizeof(e_lsp_sem_tc_palette[0])))

int e_lsp_sem_truecolor(const char *type)
{
 if (!type)
  return LSP_SEM_TC_NONE;
 if (!strcmp(type, "function") || !strcmp(type, "method") ||
     !strcmp(type, "macro"))
  return 0;
 /* type-like categories: a teal the 16-colour palette cannot use on the
    Borland-blue background (its nearest teal washes out), so it falls back to
    yellow (e_lsp_sem_fg) where 24-bit colour is unavailable. */
 if (!strcmp(type, "type") || !strcmp(type, "class") ||
     !strcmp(type, "interface") || !strcmp(type, "enum") ||
     !strcmp(type, "struct") || !strcmp(type, "typeParameter") ||
     !strcmp(type, "namespace"))
  return 1;
 return LSP_SEM_TC_NONE;
}

int e_lsp_sem_slot_rgb(int slot, int *r, int *g, int *b)
{
 if (slot < 0 || slot >= LSP_SEM_TC_COUNT)
  return 0;
 if (r) *r = e_lsp_sem_tc_palette[slot].r;
 if (g) *g = e_lsp_sem_tc_palette[slot].g;
 if (b) *b = e_lsp_sem_tc_palette[slot].b;
 return 1;
}

/* Capture the server's semantic-token legend from the initialize response
   (capabilities.semanticTokensProvider.legend.tokenTypes) so token indices can
   be resolved to names.  Called once, during the handshake. */
static void lsp_capture_semantic_legend(e_lsp_session *s, struct json_object *m)
{
 struct json_object *caps, *prov, *legend, *types;
 int i, n;

 caps = obj_obj(obj_obj(m, "result"), "capabilities");
 prov = caps ? obj_obj(caps, "semanticTokensProvider") : NULL;
 legend = prov ? obj_obj(prov, "legend") : NULL;
 types = legend ? obj_obj(legend, "tokenTypes") : NULL;
 if (!types || !json_object_is_type(types, json_type_array))
  return;
 n = json_object_array_length(types);
 for (i = 0; i < n && s->sem_nlegend < (int)(sizeof(s->sem_legend) /
                                             sizeof(s->sem_legend[0])); i++)
 {
  const char *name = json_object_get_string(json_object_array_get_idx(types, i));
  s->sem_legend[s->sem_nlegend++] = strdup(name ? name : "");
 }
}

int e_lsp_semantic_tokens(e_lsp_session *s, const char *path,
                          e_lsp_sem_token *out, int max)
{
 char abspath[PATH_MAX], uri[PATH_MAX + 8];
 struct json_object *args, *doc, *resp, *result, *data;
 int *ints = NULL;
 int id, n = 0, i, len, got = 0;

 if (!s)
  return -1;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(uri, sizeof(uri), "file://%s", abspath);
 args = json_object_new_object();
 doc = json_object_new_object();
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 id = ++s->id;
 lsp_send(s, id, "textDocument/semanticTokens/full", args);
 resp = lsp_pump(s, id, LSP_TMO_REQ);
 if (!resp)
  return -1;
 result = obj_obj(resp, "result");
 data = result ? obj_obj(result, "data") : NULL;
 if (data && json_object_is_type(data, json_type_array))
 {
  len = json_object_array_length(data);
  ints = malloc((size_t)(len > 0 ? len : 1) * sizeof(int));
  if (ints)
  {
   for (i = 0; i < len; i++)
    ints[i] = json_object_get_int(json_object_array_get_idx(data, i));
   got = e_lsp_semantic_decode(ints, len, out, max);
   free(ints);
  }
 }
 (void)n;
 json_object_put(resp);
 return got;
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

/* The layout every format request asks the server for. */
#define LSP_FORMAT_TAB_SIZE 2

/**
 * lsp_position - A new {line, character} object.
 */
static struct json_object *lsp_position(int line, int character)
{
 struct json_object *pos = json_object_new_object();

 json_object_object_add(pos, "line", json_object_new_int(line));
 json_object_object_add(pos, "character", json_object_new_int(character));
 return pos;
}

/**
 * lsp_format_params - The arguments of a whole-file or a range format.
 * @abspath: the file; @range: the range, or NULL for the whole file.
 */
static struct json_object *lsp_format_params(const char *abspath,
                                             const e_lsp_range *range)
{
 struct json_object *args = json_object_new_object();
 struct json_object *doc = json_object_new_object();
 struct json_object *opts = json_object_new_object();
 char uri[PATH_MAX + 8];

 snprintf(uri, sizeof(uri), "file://%s", abspath);
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 if (range)
 {
  struct json_object *r = json_object_new_object();

  json_object_object_add(r, "start",
                         lsp_position(range->start_line, range->start_char));
  json_object_object_add(r, "end",
                         lsp_position(range->end_line, range->end_char));
  json_object_object_add(args, "range", r);
 }
 json_object_object_add(opts, "tabSize", json_object_new_int(LSP_FORMAT_TAB_SIZE));
 json_object_object_add(opts, "insertSpaces", json_object_new_boolean(1));
 json_object_object_add(args, "options", opts);
 return args;
}

/**
 * lsp_format_text - The text sent with a format request, with the edits of
 * its answer applied (malloc'd), or NULL when the server changes nothing.
 */
static char *lsp_format_text(struct json_object *resp, const char *base)
{
 struct json_object *result = obj_obj(resp, "result");

 if (!result || !json_object_is_type(result, json_type_array) ||
     json_object_array_length(result) == 0)
  return NULL;
 return lsp_apply_edits(base, result);
}

/**
 * lsp_format_answered - Hand the caller the formatted text, or NULL.
 */
static void lsp_format_answered(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 char *out = NULL;

 if (how == E_D_REQ_DONE)
  out = lsp_format_text(resp, t->base);
 t->edit_fn(how, out, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_format_async(e_lsp_session *s, const char *path,
                       const char *current_text, const e_lsp_range *range,
                       e_lsp_edit_fn fn, void *ctx)
{
 char abspath[PATH_MAX];
 const char *method = "textDocument/formatting";
 lsp_typed *t;

 if (!s || !fn || !current_text)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->edit_fn = fn;
 t->base = strdup(current_text);
 if (!t->base)
 {
  lsp_typed_free(t);
  return E_D_SEND_FAILED;
 }
 if (range)
  method = "textDocument/rangeFormatting";
 lsp_abspath(path, abspath);
 return lsp_typed_send(s, method, lsp_format_params(abspath, range),
                       lsp_format_answered, t);
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
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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

/* The subset of the last-published diagnostics for @uri that cover line @line,
   as a NEW json array for codeAction's context.diagnostics.  A server (Metals,
   clangd, ...) only emits the matching quick-fix actions when it sees the
   diagnostic here -- without it a "fix this error" action never appears.  Empty
   array when nothing overlaps or the stored set is for another file. */
static struct json_object *lsp_diags_covering(e_lsp_session *s, const char *uri,
                                              int line)
{
 struct json_object *out = json_object_new_array();
 int i, n;

 if (!s->diag_raw || strcmp(s->diag_uri, uri) != 0)
  return out;
 n = json_object_array_length(s->diag_raw);
 for (i = 0; i < n; i++)
 {
  struct json_object *d = json_object_array_get_idx(s->diag_raw, i);
  struct json_object *rng = obj_obj(d, "range");
  int ds = rng ? obj_int(obj_obj(rng, "start"), "line", -1) : -1;
  int de = rng ? obj_int(obj_obj(rng, "end"), "line", ds) : ds;
  if (ds < 0 || line < ds || line > de)
   continue;
  json_object_array_add(out, json_object_get(d));
 }
 return out;
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
 json_object_object_add(ctx, "diagnostics", lsp_diags_covering(s, uri, line));
 json_object_object_add(args, "context", ctx);
 id = ++s->id;
 lsp_send(s, id, "textDocument/codeAction", args);
 resp = lsp_pump(s, id, LSP_TMO_REQ);
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

/* Resolve a CodeAction the server returned unresolved (only a `data` field, no
   edit yet -- how Metals delivers its refactors): send codeAction/resolve with
   the action and return the resolved copy (now carrying an `edit` and/or a
   `command`).  Caller owns the result; NULL on timeout. */
static struct json_object *lsp_resolve_code_action(e_lsp_session *s,
                                                   struct json_object *action)
{
 struct json_object *resp, *result;
 int id = ++s->id;

 lsp_send(s, id, "codeAction/resolve", json_object_get(action));
 resp = lsp_pump(s, id, LSP_TMO_REQ);
 if (!resp)
  return NULL;
 result = obj_obj(resp, "result");
 result = result ? json_object_get(result) : NULL;   /* outlive the resp put */
 json_object_put(resp);
 return result;
}

/* Run a command-based code action: send workspace/executeCommand and let the
   server write the result back through workspace/applyEdit (handled inline by
   lsp_answer_request, which edits s->pend_text).  Returns the new text for @myuri
   -- the caller owns it -- with *other_files set to the count of OTHER files the
   command touched; NULL when the command applied no edit to a tracked file (a
   pure side-effect command, e.g. "run main": nothing to write into the buffer). */
static char *lsp_execute_command_collect(e_lsp_session *s, const char *command,
                                         struct json_object *arguments,
                                         const char *myuri, const char *current_text,
                                         int *other_files)
{
 struct json_object *args, *resp;
 char *newtext;
 int id;

 free(s->pend_text);                 /* arm the applyEdit working buffer */
 s->pend_text = strdup(current_text);
 snprintf(s->pend_uri, sizeof(s->pend_uri), "%s", myuri);
 s->pend_others = 0;
 s->pend_applied = 0;

 args = json_object_new_object();
 json_object_object_add(args, "command", json_object_new_string(command));
 if (arguments && json_object_is_type(arguments, json_type_array))
  json_object_object_add(args, "arguments", json_object_get(arguments));
 id = ++s->id;
 lsp_send(s, id, "workspace/executeCommand", args);
 resp = lsp_pump(s, id, LSP_TMO_REQ);   /* applyEdit handled while pumping */
 if (resp)
  json_object_put(resp);

 newtext = s->pend_text;             /* disarm, hand ownership to the caller */
 s->pend_text = NULL;
 s->pend_uri[0] = '\0';
 if (other_files)
  *other_files = s->pend_others;
 if (!s->pend_applied)
 {  free(newtext);  return NULL;  }  /* command ran, no buffer edit to apply */
 return newtext;
}

char *e_lsp_apply_code_action(e_lsp_session *s, int index, const char *path,
                              const char *current_text, int *other_files)
{
 char abspath[PATH_MAX], myuri[PATH_MAX + 8];
 struct json_object *a, *act, *resolved = NULL, *edit, *cmdobj;
 const char *command;
 char *out = NULL;

 if (other_files)
  *other_files = 0;
 if (!s || !s->acts_raw || index < 0 ||
     index >= json_object_array_length(s->acts_raw))
  return NULL;
 a = json_object_array_get_idx(s->acts_raw, index);
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 snprintf(myuri, sizeof(myuri), "file://%s", abspath);
 act = a;
 /* An unresolved action carries only a `data` field; resolve it first so its
    edit/command is filled in (Metals' refactors arrive this way). */
 if (!obj_obj(a, "edit") && !obj_obj(a, "command") && obj_obj(a, "data"))
 {
  resolved = lsp_resolve_code_action(s, a);
  if (resolved)
   act = resolved;
 }
 edit = obj_obj(act, "edit");
 if (edit)                           /* a precomputed (or just-resolved) WorkspaceEdit */
  out = lsp_apply_workspace_edit(edit, myuri, current_text, other_files);
 else
 {
  /* Command-based: either a CodeAction whose "command" field is a Command
     object, or the entry IS a Command (its "command" is the string id). */
  cmdobj = obj_obj(act, "command");
  if (cmdobj && json_object_is_type(cmdobj, json_type_object))
   command = obj_str(cmdobj, "command");
  else
  {  cmdobj = act;  command = obj_str(act, "command");  }
  if (command)
   out = lsp_execute_command_collect(s, command, obj_obj(cmdobj, "arguments"),
                                     myuri, current_text, other_files);
 }
 if (resolved)
  json_object_put(resolved);
 return out;
}

/**
 * lsp_ask_to_end - Ask a server that has answered `initialize` to end, on
 * its own pipe: `shutdown`, whose answer nobody waits for, then `exit`.
 * @s: the session.
 * Return: 1 when it was asked, 0 when it cannot be (it lost its input, or
 * never finished its start and would not take the request).
 */
static int lsp_ask_to_end(e_lsp_session *s)
{
 if (s->in_fd < 0 || s->input_lost || s->dead || !s->started)
  return 0;
 if (lsp_request_kind(s, LSP_PEND_QUIET, "shutdown", NULL, NULL, NULL)
     != E_D_SEND_OK)
  return 0;
 lsp_notify(s, "exit", NULL);
 return !s->input_lost;
}

/* How long a closed session's output is read at most: the owner's whole
   ladder (the grace, SIGKILL, the kill grace) and a second of slack.  By
   then the server has ended; a process it left behind that still holds its
   output is not read for ever. */
#define LSP_DRAIN_MAX_MS \
 (WPE_CHILD_END_GRACE_MS + WPE_CHILD_END_KILL_MS + E_D_REQ_SECOND_MS)

/* A closed session's output, read until its end of file or the bound. */
typedef struct {
 int fd;
 int timer;
} lsp_drain;

/**
 * lsp_drain_stop - Stop reading a closed session's output and close it.
 * @d: the drain (freed here).
 */
static void lsp_drain_stop(lsp_drain *d)
{
 if (d->timer > WPE_TIMER_NONE)
  wpe_timer_del(d->timer);
 wpe_fd_del(d->fd);
 close(d->fd);
 free(d);
}

/**
 * lsp_discard_on_readable - What a server that is being ended still writes
 * (its answer to `shutdown`, a last notification) is read and dropped; at
 * its end of file the descriptor goes.
 * @fd: the server's output; @data: the drain.
 */
static void lsp_discard_on_readable(int fd, void *data)
{
 char buf[8192];
 ssize_t n;

 for (;;)
 {
  n = read(fd, buf, sizeof buf);
  if (n > 0)
   continue;
  if (n < 0 && errno == EINTR)
   continue;
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
   return;
  break;
 }
 lsp_drain_stop(data);
}

/**
 * lsp_drain_bound - The bound has passed: the output is closed even if
 * something still holds it open, so a descriptor of the loop is never held
 * by a server long gone.
 * @data: the drain.
 */
static void lsp_drain_bound(void *data)
{
 lsp_drain *d = data;

 d->timer = WPE_TIMER_NONE;      /* a one-shot timer is gone once it fired */
 lsp_drain_stop(d);
}

/**
 * lsp_output_drain_until_end - Keep reading a closed session's server until
 * it ends.
 * @fd: the server's output.
 *
 * Closing the pipe at once would make the server's answer to `shutdown`
 * fail with SIGPIPE: the server would die there, before the owner reaches
 * its group, and a helper it started (an indexer, a JVM) would be left
 * behind.  Read on, the server ends as asked or on the owner's ladder, with
 * its group, and the pipe closes at its end of file or at LSP_DRAIN_MAX_MS,
 * whichever comes first.  When the loop has no room, the pipe is closed.
 */
static void lsp_output_drain_until_end(int fd)
{
 lsp_drain *d = calloc(1, sizeof(*d));

 if (!d)
 {
  close(fd);
  return;
 }
 d->fd = fd;
 if (wpe_fd_add(fd, POLLIN, lsp_discard_on_readable, d) != 0)
 {
  close(fd);
  free(d);
  return;
 }
 d->timer = wpe_timer_add(LSP_DRAIN_MAX_MS, 0, lsp_drain_bound, d);
}

void e_lsp_close(e_lsp_session *s)
{
 int asked;

 if (!s)
  return;
 /* Whoever waits for an answer is told now, so nothing waits for a server
    that is going away. */
 e_lsp_requests_cancel(s, E_D_REQ_GONE);
 asked = lsp_ask_to_end(s);
 s->ending = 1;
 lsp_disarm_all(s);
 if (s->in_fd >= 0)
  wpe_fd_close(s->in_fd);          /* what still waits for it goes too */
 if (s->out_fd >= 0 && asked)
  lsp_output_drain_until_end(s->out_fd);
 else if (s->out_fd >= 0)
  close(s->out_fd);
 /* Asked on its pipe, the server gets the owner's grace to end by itself,
    then SIGKILL; one that was never asked gets SIGTERM first.  Either way on
    the loop, so the user never waits for a server that does not end. */
 if (asked)
  wpe_child_end_detached(&s->server, WPE_CHILD_NO_SIGNAL);
 else
  wpe_child_end_detached(&s->server, SIGTERM);
 lsp_free_items(s);
 lsp_free_locs(s);
 lsp_free_syms(s);
 lsp_free_acts(s);
 lsp_free_lenses(s);
 lsp_free_inlays(s);
 free(s->pend_text);
 if (s->diag_raw)
  json_object_put(s->diag_raw);
 {  int i;  for (i = 0; i < s->sem_nlegend; i++)  free(s->sem_legend[i]);  }
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
