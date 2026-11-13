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
#include "we_startplan.h"        /* the server starts through its start plan */
#include "we_dap_proto.h"        /* e_dap_reader: shared Content-Length framing */
#include "we_fdloop.h"           /* wpe_fd_readable_now */

#define LSP_SERVER_WHAT "the language server"   /* in the owner's lines */

/* Requests answered on the loop. */
#define LSP_PENDING_MAX   16     /* requests out at once, the start included */
#define LSP_METHOD_MAX    64     /* "textDocument/semanticTokens/full" fits   */
#define LSP_NO_ID         (-1)
#define LSP_PEND_FREE     0      /* the slot is free                          */
#define LSP_PEND_CALL     1      /* a request: waiting line and deadline      */
#define LSP_PEND_START    2      /* `initialize`: waiting line, no deadline   */
#define LSP_PEND_QUIET    3      /* `shutdown`: nobody waits, no line         */
#define LSP_PEND_BACKGROUND 4    /* an overlay fetch: told, but nobody waits  */
/* What the deadline leaves on the status row. */
#define LSP_TIMEOUT_FMT   "%s did not answer `%s` within %d s: Alt-Q asks again"
#define LSP_DEFAULT_NAME  "the language server"

/* The file a command-based code action edits: the server runs the command
   and sends the change back as workspace/applyEdit, which is applied here,
   to the text the request was sent with. */
typedef struct {
 char *text;                      /* the working copy, edited in place         */
 char  uri[PATH_MAX + 8];         /* file:// of the file it is a copy of       */
 int   others;                    /* edits aimed at other files                */
 int   applied;                   /* at least one applyEdit came               */
} lsp_work;

/* One request out: its id, what it asked, since when, and who is told. */
typedef struct {
 int               kind;          /* LSP_PEND_*                               */
 int               id;            /* the JSON-RPC id its answer carries        */
 char              method[LSP_METHOD_MAX];
 long              sent_ms;       /* the host's clock when it went out         */
 e_lsp_answered_fn fn;            /* told exactly once, or NULL                */
 void             *ctx;
 lsp_work         *work;          /* an executeCommand's file, or NULL         */
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
 int          encoding;             /* E_LSP_ENC_*: the units of every column */
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
                                      const char *current_text, int enc,
                                      int *other_files);

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
  e_lsp_units st_char, en_char;
  e_lsp_units_read(st, &st_char);
  if (!e_lsp_units_read(en, &en_char))
   en_char = st_char;
  if (sev == 1) errors++;
  else if (sev == 2) warnings++;
  if (s->host.on_diagnostic)
   s->host.on_diagnostic(uri_to_path(uri),
                         obj_int(st, "line", 0), st_char,
                         obj_int(en, "line", obj_int(st, "line", 0)),
                         en_char, sev, obj_str(d, "message"), s->host.ud);
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

/**
 * lsp_work_out - The working copy of the oldest command-based code action
 * out, or NULL when none is: the only file a server's applyEdit may change.
 * @s: the session.
 */
static lsp_work *lsp_work_out(e_lsp_session *s)
{
 lsp_pending *oldest = NULL;
 int i;

 for (i = 0; i < LSP_PENDING_MAX; i++)
 {
  lsp_pending *p = &s->pending[i];

  if (p->kind == LSP_PEND_FREE || !p->work)
   continue;
  if (!oldest || p->id < oldest->id)
   oldest = p;
 }
 if (!oldest)
  return NULL;
 return oldest->work;
}

/**
 * lsp_work_apply - Apply a server's WorkspaceEdit to a code action's
 * working copy, so the command's change reaches the buffer it was run on.
 * @s: the session, whose encoding the edit's columns are in;
 * @w: the working copy, or NULL; @edit: the WorkspaceEdit, or NULL.
 * Return: 1 when applied, 0 when declined.
 */
static int lsp_work_apply(e_lsp_session *s, lsp_work *w, struct json_object *edit)
{
 char *nt;
 int others = 0;

 if (!w || !w->text || !edit)
  return 0;
 nt = lsp_apply_workspace_edit(edit, w->uri, w->text, s->encoding, &others);
 if (!nt)
  return 0;
 free(w->text);
 w->text = nt;
 w->others += others;
 w->applied = 1;
 return 1;
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
  /* The server, running a command-based code action, asks for a
     WorkspaceEdit: it is applied to that request's working copy and
     answered {applied:true}.  With no such request out it is declined
     {applied:false}: a file the user is not acting on is never rewritten. */
  struct json_object *edit = obj_obj(obj_obj(m, "params"), "edit");
  struct json_object *res = json_object_new_object();
  int applied = lsp_work_apply(s, lsp_work_out(s), edit);

  json_object_object_add(res, "applied", json_object_new_boolean(applied));
  lsp_reply(s, id, res);
 }
 else
  /* registerCapability, workDoneProgress/create, other *_refresh, unknown -> null */
  lsp_reply(s, id, NULL);
}

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

/* Log one message read (its id/method) so a stuck request can be diagnosed
   without attaching a debugger. */
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
 * handed over when the loop reads it (e_lsp_poll), while diagnostics,
 * status and the server's own requests read meanwhile go where they always
 * go.  The host's clock and one-shot timers count the waiting line and the
 * deadline of the oldest request the user waits for; a request given up on
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

/* Which requests an end reaches. */
#define LSP_END_ALL     0        /* every slot: the session is going          */
#define LSP_END_WAITED  1        /* what the user waits for: Esc, a deadline  */

/**
 * lsp_end_reaches - Does an end of @scope reach the request in slot @p?
 */
static int lsp_end_reaches(const lsp_pending *p, int scope)
{
 if (p->kind == LSP_PEND_FREE)
  return 0;
 if (scope == LSP_END_ALL)
  return 1;
 return lsp_is_waited(p);
}

/**
 * lsp_requests_end - Give up the requests @scope reaches and tell their
 * owners @why, oldest first, once each.
 * @s: the session; @why: E_D_REQ_*; @scope: LSP_END_*.
 *
 * Every slot is freed before anyone is told, so an owner may ask again and
 * an answer that still comes is dropped by its id.
 */
static void lsp_requests_end(e_lsp_session *s, E_D_REQ_END why, int scope)
{
 lsp_told told[LSP_PENDING_MAX];
 int i, n = 0;

 if (!s)
  return;
 for (i = 0; i < LSP_PENDING_MAX; i++)
 {
  lsp_pending *p = &s->pending[i];

  if (!lsp_end_reaches(p, scope))
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

void e_lsp_requests_cancel(e_lsp_session *s, E_D_REQ_END why)
{
 lsp_requests_end(s, why, LSP_END_ALL);
}

void e_lsp_requests_give_up(e_lsp_session *s, E_D_REQ_END why)
{
 lsp_requests_end(s, why, LSP_END_WAITED);
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
  e_lsp_requests_give_up(s, E_D_REQ_TIMED_OUT);
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

int e_lsp_requests_owed(const e_lsp_session *s)
{
 int i;

 if (!s)
  return 0;
 for (i = 0; i < LSP_PENDING_MAX; i++)
  if (s->pending[i].kind != LSP_PEND_FREE && s->pending[i].fn)
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
 * lsp_request_work - Send @method with @params as a request of @kind whose
 * answer goes to @fn, with the working copy a server's applyEdit may
 * change while it is out.
 * @s: the session; @kind: LSP_PEND_*; @method, @params: the request
 * (@params taken over, may be NULL); @fn, @ctx: the continuation; @work:
 * the working copy (borrowed), or NULL.
 * Return: E_D_SEND_OK, or why it was not sent (@fn is then never called).
 */
static int lsp_request_work(e_lsp_session *s, int kind, const char *method,
                            struct json_object *params, e_lsp_answered_fn fn,
                            void *ctx, lsp_work *work)
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
 p->work = work;
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

/**
 * lsp_request_kind - lsp_request_work with no working copy.
 */
static int lsp_request_kind(e_lsp_session *s, int kind, const char *method,
                            struct json_object *params, e_lsp_answered_fn fn,
                            void *ctx)
{
 return lsp_request_work(s, kind, method, params, fn, ctx, NULL);
}

int e_lsp_request(e_lsp_session *s, const char *method,
                  struct json_object *params, e_lsp_answered_fn fn, void *ctx)
{
 return lsp_request_kind(s, LSP_PEND_CALL, method, params, fn, ctx);
}

/**
 * lsp_background_replace - A newer overlay fetch of @method replaces the
 * one still out: the older is told E_D_REQ_CANCELLED and its slot freed,
 * so its answer, already stale, is dropped by its id.
 * @s: the session; @method: the fetch's method.
 */
static void lsp_background_replace(e_lsp_session *s, const char *method)
{
 int i;

 for (i = 0; i < LSP_PENDING_MAX; i++)
 {
  lsp_pending *p = &s->pending[i];
  e_lsp_answered_fn fn;
  void *ctx;

  if (p->kind != LSP_PEND_BACKGROUND || strcmp(p->method, method))
   continue;
  fn = p->fn;
  ctx = p->ctx;
  lsp_release(p);
  if (fn)
   fn(E_D_REQ_CANCELLED, NULL, ctx);
 }
}

/**
 * lsp_request_background - Send an overlay fetch: no waiting line, no
 * deadline, and at most one of each method out, the newest.
 * Return: what lsp_request_kind returns.
 */
static int lsp_request_background(e_lsp_session *s, const char *method,
                                  struct json_object *params,
                                  e_lsp_answered_fn fn, void *ctx)
{
 if (s && method)
  lsp_background_replace(s, method);
 return lsp_request_kind(s, LSP_PEND_BACKGROUND, method, params, fn, ctx);
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

/* ---- spawn + handshake ------------------------------------------------- */

/* Asked before a server process starts (e_lsp_set_planner). */
static e_lsp_plan_fn lsp_planner;

/**
 * e_lsp_set_planner - Who plans, shows and asks about a server's start.
 * @fn: the editor's planner, or NULL (the C tests).
 */
void e_lsp_set_planner(e_lsp_plan_fn fn)
{
 lsp_planner = fn;
}

/**
 * lsp_plan_of - The answered start plan of a server's command line.
 * @argv: the command; @cwd: the project.
 *
 * The editor's planner says whether the server runs the folder's build
 * code and asks; with no planner (a C test) the test's command runs as
 * given.
 * Return: the plan (freed by the caller), or NULL when nothing may start.
 */
static WpeStartPlan *lsp_plan_of(char *const argv[], const char *cwd)
{
 WpeStartPlan *plan;

 if (lsp_planner)
  return lsp_planner(argv, cwd);
 plan = wpe_startplan_fixed(WPE_START_LANGUAGE_SERVER, argv[0], argv);
 if (plan && cwd)
  wpe_startplan_set_cwd(plan, cwd);
 return plan;
}

/**
 * lsp_spawn - Start the language server for a session.
 * @s: the session; @argv: the server's command line; @cwd: the project.
 *
 * Through its start plan, in the project.  In a session of its own, so a
 * Ctrl-C typed for a script run from the editor spares it; its stderr goes
 * nowhere, since servers are chatty there and the editor's screen would
 * show it; a server that writes to a closed pipe ends, as from a shell.
 * The editor follows its end, so closing the session later never signals a
 * number another process has by then.  Its end reaches its whole group (an
 * indexer it started goes with it), except for a server whose group may
 * hold a shared build server (e_lsp_host.server_shares_daemon), which is
 * signalled alone.
 * Return: 0, or -1 when it could not be started.
 */
static int lsp_spawn(e_lsp_session *s, char *const argv[], const char *cwd)
{
 WpeStartPlan *plan = lsp_plan_of(argv, cwd);
 WpeStartIo io;
 int rc = -1;

 memset(&io, 0, sizeof io);
 io.io = WPE_CHILD_IO_PIPES;
 io.group = WPE_CHILD_OWN_SESSION;
 io.stderr_to_null = 1;
 io.signal_leader_only = s->host.server_shares_daemon;
 io.what = LSP_SERVER_WHAT;
 if (plan && wpe_startplan_running(plan))
  rc = wpe_startplan_start_words(plan, &io, &s->server, NULL, NULL);
 wpe_startplan_free(plan);
 if (rc != 0)
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
  /* Offer the encodings the position owner can count in.  A server that
     honours the offer (clangd answers utf-8) sends byte columns; one that
     ignores it (pyright, gopls) stays utf-16, which the session reads from
     the answer in lsp_capture_encoding. */
  struct json_object *general = json_object_new_object();
  json_object_object_add(general, "positionEncodings",
                         e_lsp_encodings_offered());
  json_object_object_add(caps, "general", general);
 }
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
 * lsp_capture_encoding - Keep the encoding the server answered with.
 * @m: the `initialize` response.
 *
 * Every column the session sends or reads is in these units.  An absent
 * answer is utf-16, which every server supports; a value the editor did not
 * offer is utf-16 too, said once in the wire trace.
 */
static void lsp_capture_encoding(e_lsp_session *s, struct json_object *m)
{
 struct json_object *caps = obj_obj(obj_obj(m, "result"), "capabilities");
 const char *answer = obj_str(caps, "positionEncoding");
 FILE *fp = lsp_trace_file();

 s->encoding = e_lsp_encoding_parse(answer);
 if (!fp)
  return;
 fprintf(fp, "[%ld ENC] positionEncoding=%s -> %s%s\n", lsp_now_ms(),
         answer ? answer : "absent", e_lsp_encoding_name(s->encoding),
         e_lsp_encoding_answer_is_known(answer) ? "" : " (not offered)");
 fflush(fp);
}

int e_lsp_position_encoding(const e_lsp_session *s)
{
 return s ? s->encoding : E_LSP_ENC_UTF16;
}

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
 lsp_capture_encoding(s, m);
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
 s->encoding = E_LSP_ENC_UTF16;        /* until the server says otherwise */
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

/**
 * lsp_add_document_text - Put the document's text in a didOpen or didChange.
 *
 * The server gets the text with each CR written as a space (the protocol
 * counts a CR as a line end, and one server line is one editor row); the
 * editor's own copy, which edits are spliced into, keeps its CRs.  Same
 * bytes and units, so no position moves.  If the copy fails the text goes
 * as it is.
 */
static void lsp_add_document_text(struct json_object *obj, const char *text)
{
 char *wire = e_lsp_wire_text(text);

 json_object_object_add(obj, "text",
                        json_object_new_string(wire ? wire : (text ? text : "")));
 free(wire);
}

static struct json_object *lsp_text_pos(e_lsp_session *s, const char *path,
                                        int line, e_lsp_units character)
{
 struct json_object *args = json_object_new_object();
 struct json_object *doc = json_object_new_object();
 char uri[PATH_MAX + 8];

 (void)s;
 snprintf(uri, sizeof(uri), "file://%s", path);
 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 json_object_object_add(args, "position", e_lsp_position_json(line, character));
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
 lsp_add_document_text(doc, text);
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
 lsp_add_document_text(change, text);
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

/* The notification that says the server's view of a file has changed. */
#define LSP_NEWS_METHOD "textDocument/publishDiagnostics"

/**
 * lsp_is_news - Does message @m say the server's view of a file may have
 * changed, so an overlay shown for it may be out of date?
 * @m: a message read from the server.
 *
 * Only a diagnostics batch does: the server publishes it once it has
 * compiled or indexed.  The log lines, progress and status a server sends
 * as it works come with the editor's own requests too, so counting them
 * would have an overlay fetched again for every answer it gets, for as
 * long as the server answers; the server's explicit refresh requests reach
 * the host through their own hooks.
 * Return: 1 when it is news.
 */
static int lsp_is_news(struct json_object *m)
{
 const char *meth = obj_str(m, "method");

 if (!meth || obj_obj(m, "id"))
  return 0;
 return !strcmp(meth, LSP_NEWS_METHOD);
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
  if (lsp_is_news(m))
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
 e_lsp_text_fn      text_fn;
 e_lsp_location_fn  loc_fn;
 e_lsp_items_fn     items_fn;
 e_lsp_edit_fn      edit_fn;
 e_lsp_locations_fn locs_fn;
 e_lsp_symbols_fn   syms_fn;
 e_lsp_ranges_fn    ranges_fn;
 e_lsp_lenses_fn    lenses_fn;
 e_lsp_inlays_fn    inlays_fn;
 e_lsp_tokens_fn    tokens_fn;
 e_lsp_actions_fn   acts_fn;
 e_lsp_applied_fn   applied_fn;
 int            max;           /* the most items wanted                     */
 int            flag;          /* hierarchies: outgoing calls, subtypes     */
 char          *base;          /* format, rename, code action: the text sent */
 char           path[PATH_MAX];       /* the file as the server knows it   */
 char           uri[PATH_MAX + 8];    /* ... and its file:// form          */
 const char    *next_method;   /* hierarchies: the step after the prepare   */
 const char    *wrap;          /* ... the field each answer entry wraps the
                                  item in ("from", "to"), or NULL           */
 struct json_object *list;     /* code lenses: the list being resolved      */
 int            next;          /* ... the lens to look at next              */
 struct json_object *action;   /* a code action being run                   */
 lsp_work      *work;          /* ... the file its command may change       */
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
 * lsp_work_free - Forget a code action's working copy.
 */
static void lsp_work_free(lsp_work *w)
{
 if (!w)
  return;
 free(w->text);
 free(w);
}

/**
 * lsp_typed_free - Forget a typed request once its caller was told.
 */
static void lsp_typed_free(lsp_typed *t)
{
 free(t->base);
 if (t->list)
  json_object_put(t->list);
 if (t->action)
  json_object_put(t->action);
 lsp_work_free(t->work);
 free(t);
}

/**
 * lsp_typed_at - Note the file a typed request is about, as the server
 * knows it (the real path) and as a file:// URI.
 */
static void lsp_typed_at(lsp_typed *t, const char *path)
{
 if (!realpath(path, t->path))
  snprintf(t->path, sizeof t->path, "%s", path);
 snprintf(t->uri, sizeof t->uri, "file://%s", t->path);
}

/**
 * lsp_typed_step - Send the next step of a typed request from inside its
 * continuation; the context stays with the request.
 * Return: what e_lsp_request returns (the caller tells its owner when the
 * step could not go out).
 */
static int lsp_typed_step(lsp_typed *t, const char *method,
                          struct json_object *params, e_lsp_answered_fn fn)
{
 return e_lsp_request(t->s, method, params, fn, t);
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
                      e_lsp_units character, e_lsp_text_fn fn, void *ctx)
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

/**
 * lsp_signature_text - The active signature's label of a signatureHelp
 * answer (malloc'd), or NULL when there is none.
 */
static char *lsp_signature_text(struct json_object *resp)
{
 struct json_object *result = obj_obj(resp, "result");
 struct json_object *sigs = result ? obj_obj(result, "signatures") : NULL;
 struct json_object *sig;
 int idx, n;

 n = (sigs && json_object_is_type(sigs, json_type_array))
     ? json_object_array_length(sigs) : 0;
 if (n <= 0)
  return NULL;
 idx = obj_int(result, "activeSignature", 0);
 if (idx < 0 || idx >= n)
  idx = 0;
 sig = json_object_array_get_idx(sigs, idx);
 if (!obj_str(sig, "label"))
  return NULL;
 return strdup(obj_str(sig, "label"));
}

/**
 * lsp_signature_answered - Hand the caller the signature's label, or NULL.
 */
static void lsp_signature_answered(E_D_REQ_END how, struct json_object *resp,
                                   void *ctx)
{
 lsp_typed *t = ctx;
 char *text = NULL;

 if (how == E_D_REQ_DONE)
  text = lsp_signature_text(resp);
 t->text_fn(how, text, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_signature_help_async(e_lsp_session *s, const char *path, int line,
                               e_lsp_units character, e_lsp_text_fn fn, void *ctx)
{
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->text_fn = fn;
 lsp_typed_at(t, path);
 return lsp_typed_send(s, "textDocument/signatureHelp",
                       lsp_text_pos(s, t->path, line, character),
                       lsp_signature_answered, t);
}

/* The answer of the three "jump to a location" requests, which differ only in
 * the method name and all answer with Location | Location[] | LocationLink[]:
 * fills out_* with the first location; returns 0 on success, -1 otherwise. */
static int lsp_location_of(struct json_object *resp, char *out_path,
                           size_t out_sz, int *out_line, e_lsp_units *out_char)
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
   if (out_char) e_lsp_units_read(st, out_char);
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
 int line = 0, found = 0;
 e_lsp_units character = e_lsp_units_zero();

 where[0] = '\0';
 if (how == E_D_REQ_DONE)
  found = (lsp_location_of(resp, where, sizeof where, &line, &character) == 0);
 t->loc_fn(how, found, where, line, character, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_locate_async(e_lsp_session *s, int which, const char *path,
                       int line, e_lsp_units character, e_lsp_location_fn fn,
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
                           e_lsp_units character, int max, e_lsp_items_fn fn,
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

/* How many entries an engine-kept array holds. */
#define LSP_KEPT(arr) ((int)(sizeof(arr) / sizeof((arr)[0])))

/**
 * lsp_result_array - The answer's result when it is an array, else NULL.
 */
static struct json_object *lsp_result_array(struct json_object *resp)
{
 struct json_object *result = obj_obj(resp, "result");

 if (result && json_object_is_type(result, json_type_array))
  return result;
 return NULL;
}

/**
 * lsp_locations_fill - Keep the locations of a references or highlight
 * answer in the session (the previous ones are freed).
 * @s: the session; @resp: the answer; @max: the most wanted; @here: the
 * file a highlight answer is about (its entries carry no uri), or NULL for
 * references, whose entries name their file.
 * Return: how many were kept.
 */
static int lsp_locations_fill(e_lsp_session *s, struct json_object *resp,
                              int max, const char *here)
{
 struct json_object *result = lsp_result_array(resp);
 int i, n = result ? (int)json_object_array_length(result) : 0;

 lsp_free_locs(s);
 for (i = 0; i < n && s->nlocs < max && s->nlocs < LSP_KEPT(s->locs); i++)
 {
  struct json_object *loc = json_object_array_get_idx(result, i);
  const char *uri = obj_str(loc, "uri");
  struct json_object *rng = obj_obj(loc, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;

  if (!st || (!here && !uri))
   continue;
  if (here)
   s->locs[s->nlocs].path = strdup(here);
  else
   s->locs[s->nlocs].path = strdup(uri_to_path(uri));
  s->locs[s->nlocs].line = obj_int(st, "line", 0);
  e_lsp_units_read(st, &s->locs[s->nlocs].character);
  s->nlocs++;
 }
 return s->nlocs;
}

/**
 * lsp_references_answered - Hand the caller every use of the symbol.
 */
static void lsp_references_answered(E_D_REQ_END how, struct json_object *resp,
                                    void *ctx)
{
 lsp_typed *t = ctx;
 int n = -1;

 if (how == E_D_REQ_DONE)
  n = lsp_locations_fill(t->s, resp, t->max, NULL);
 t->locs_fn(how, t->s->locs, n, t->ctx);
 lsp_typed_free(t);
}

/**
 * lsp_highlight_answered - Hand the caller every occurrence in the file.
 */
static void lsp_highlight_answered(E_D_REQ_END how, struct json_object *resp,
                                   void *ctx)
{
 lsp_typed *t = ctx;
 int n = -1;

 if (how == E_D_REQ_DONE)
  n = lsp_locations_fill(t->s, resp, t->max, t->path);
 t->locs_fn(how, t->s->locs, n, t->ctx);
 lsp_typed_free(t);
}

/**
 * lsp_typed_locations - A typed request whose answer is locations.
 * Return: the typed context, or NULL without memory.
 */
static lsp_typed *lsp_typed_locations(e_lsp_session *s, const char *path,
                                      int max, e_lsp_locations_fn fn,
                                      void *ctx)
{
 lsp_typed *t = lsp_typed_new(s, ctx);

 if (!t)
  return NULL;
 t->locs_fn = fn;
 t->max = max;
 lsp_typed_at(t, path);
 return t;
}

int e_lsp_references_async(e_lsp_session *s, const char *path, int line,
                           e_lsp_units character, int max, e_lsp_locations_fn fn,
                           void *ctx)
{
 struct json_object *args, *refctx;
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_locations(s, path, max, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 args = lsp_text_pos(s, t->path, line, character);
 refctx = json_object_new_object();
 json_object_object_add(refctx, "includeDeclaration", json_object_new_boolean(1));
 json_object_object_add(args, "context", refctx);
 return lsp_typed_send(s, "textDocument/references", args,
                       lsp_references_answered, t);
}

int e_lsp_document_highlight_async(e_lsp_session *s, const char *path, int line,
                                   e_lsp_units character, int max,
                                   e_lsp_locations_fn fn, void *ctx)
{
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_locations(s, path, max, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 return lsp_typed_send(s, "textDocument/documentHighlight",
                       lsp_text_pos(s, t->path, line, character),
                       lsp_highlight_answered, t);
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

/**
 * lsp_doc_params - {textDocument: {uri}}, the arguments of a request about
 * a whole file.
 */
static struct json_object *lsp_doc_params(const char *uri)
{
 struct json_object *args = json_object_new_object();
 struct json_object *doc = json_object_new_object();

 json_object_object_add(doc, "uri", json_object_new_string(uri));
 json_object_object_add(args, "textDocument", doc);
 return args;
}

/**
 * lsp_lens_needs_resolve - Is @lens one the server sent without its
 * command (its title), to be filled by codeLens/resolve?
 */
static int lsp_lens_needs_resolve(struct json_object *lens)
{
 struct json_object *rng = obj_obj(lens, "range");

 return rng && obj_obj(rng, "start") && !obj_obj(lens, "command");
}

/**
 * lsp_lenses_fill - Keep the titled lenses of a resolved list in the
 * session (the previous ones are freed).
 * @s: the session; @list: the lenses, resolved where they needed it;
 * @max: the most wanted.
 * Return: how many were kept.
 */
static int lsp_lenses_fill(e_lsp_session *s, struct json_object *list, int max)
{
 int i, n = list ? (int)json_object_array_length(list) : 0;

 lsp_free_lenses(s);
 for (i = 0; i < n && s->nlenses < max && s->nlenses < LSP_KEPT(s->lenses); i++)
 {
  struct json_object *lens = json_object_array_get_idx(list, i);
  struct json_object *rng = obj_obj(lens, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  struct json_object *cmd = obj_obj(lens, "command");
  const char *title = cmd ? obj_str(cmd, "title") : NULL;

  if (!st || !title || !*title)
   continue;
  s->lenses[s->nlenses].title = strdup(title);
  s->lenses[s->nlenses].line = obj_int(st, "line", 0);
  s->nlenses++;
 }
 return s->nlenses;
}

/**
 * lsp_lenses_tell - The list is complete: hand the caller its lenses.
 */
static void lsp_lenses_tell(lsp_typed *t)
{
 int n = lsp_lenses_fill(t->s, t->list, t->max);

 t->lenses_fn(E_D_REQ_DONE, t->s->lenses, n, t->ctx);
 lsp_typed_free(t);
}

static void lsp_lens_resolved(E_D_REQ_END how, struct json_object *resp,
                              void *ctx);

/**
 * lsp_lenses_walk - Resolve the next lens that needs it, or, when none is
 * left, hand the caller the list.
 * @t: the request, its list and the lens to look at next.
 */
static void lsp_lenses_walk(lsp_typed *t)
{
 int n = (int)json_object_array_length(t->list);

 while (t->next < n &&
        !lsp_lens_needs_resolve(json_object_array_get_idx(t->list, t->next)))
  t->next++;
 if (t->next >= n)
 {
  lsp_lenses_tell(t);
  return;
 }
 if (lsp_typed_step(t, "codeLens/resolve",
                    json_object_get(json_object_array_get_idx(t->list, t->next)),
                    lsp_lens_resolved) != E_D_SEND_OK)
 {
  t->lenses_fn(E_D_REQ_GONE, t->s->lenses, -1, t->ctx);
  lsp_typed_free(t);
 }
}

/**
 * lsp_lens_resolved - One lens came back resolved: it replaces the one in
 * the list (a lens the server could not resolve stays untitled and is
 * left out), and the walk goes on.
 */
static void lsp_lens_resolved(E_D_REQ_END how, struct json_object *resp,
                              void *ctx)
{
 lsp_typed *t = ctx;
 struct json_object *result;

 if (how != E_D_REQ_DONE)
 {
  t->lenses_fn(how, t->s->lenses, -1, t->ctx);
  lsp_typed_free(t);
  return;
 }
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_object))
  json_object_array_put_idx(t->list, (size_t)t->next, json_object_get(result));
 t->next++;
 lsp_lenses_walk(t);
}

/**
 * lsp_lenses_answered - The file's lenses came: keep the list and resolve
 * the ones that need it, one after another.
 */
static void lsp_lenses_answered(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 struct json_object *result;

 if (how != E_D_REQ_DONE)
 {
  t->lenses_fn(how, t->s->lenses, -1, t->ctx);
  lsp_typed_free(t);
  return;
 }
 result = lsp_result_array(resp);
 if (result)
  t->list = json_object_get(result);
 else
  t->list = json_object_new_array();
 t->next = 0;
 lsp_lenses_walk(t);
}

int e_lsp_code_lenses_async(e_lsp_session *s, const char *path, int max,
                            e_lsp_lenses_fn fn, void *ctx)
{
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->lenses_fn = fn;
 t->max = max;
 lsp_typed_at(t, path);
 return lsp_typed_send(s, "textDocument/codeLens", lsp_doc_params(t->uri),
                       lsp_lenses_answered, t);
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

/**
 * lsp_inlays_fill - Keep the hints of an inlayHint answer in the session
 * (the previous ones are freed); empty labels are left out.
 * Return: how many were kept.
 */
static int lsp_inlays_fill(e_lsp_session *s, struct json_object *resp, int max)
{
 struct json_object *result = lsp_result_array(resp);
 int i, n = result ? (int)json_object_array_length(result) : 0;

 lsp_free_inlays(s);
 for (i = 0; i < n && s->ninlays < max && s->ninlays < LSP_KEPT(s->inlays); i++)
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
  e_lsp_units_read(pos, &s->inlays[s->ninlays].character);
  s->inlays[s->ninlays].kind = obj_int(hint, "kind", 0);
  s->ninlays++;
 }
 return s->ninlays;
}

/**
 * lsp_inlays_answered - Hand the caller the hints, or n = -1 when the
 * fetch ended unanswered (a newer one replaced it, or the server went).
 */
static void lsp_inlays_answered(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 int n = -1;

 if (how == E_D_REQ_DONE)
  n = lsp_inlays_fill(t->s, resp, t->max);
 t->inlays_fn(how, t->s->inlays, n, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_inlay_hints_async(e_lsp_session *s, const char *path,
                            int start_line, int end_line, int max,
                            e_lsp_inlays_fn fn, void *ctx)
{
 struct json_object *args;
 lsp_typed *t;
 int rc;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->inlays_fn = fn;
 t->max = max;
 lsp_typed_at(t, path);
 if (start_line < 0)
  start_line = 0;
 if (end_line < start_line)
  end_line = start_line;
 args = lsp_doc_params(t->uri);
 json_object_object_add(args, "range", e_lsp_line_range(start_line, end_line));
 rc = lsp_request_background(s, "textDocument/inlayHint", args,
                             lsp_inlays_answered, t);
 if (rc != E_D_SEND_OK)
  lsp_typed_free(t);
 return rc;
}

/* Flatten a DocumentSymbol[]/SymbolInformation[] array depth-first into the
   session's symbols, at most @max. */
static void lsp_collect_symbols(e_lsp_session *s, struct json_object *arr,
                                int max)
{
 int i, n = json_object_array_length(arr);

 for (i = 0; i < n && s->nsyms < max && s->nsyms < LSP_KEPT(s->syms); i++)
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
   e_lsp_units_read(st, &s->syms[s->nsyms].character);
   s->syms[s->nsyms].kind = obj_int(sym, "kind", 0);
   s->nsyms++;
  }
  children = obj_obj(sym, "children");
  if (children && json_object_is_type(children, json_type_array))
   lsp_collect_symbols(s, children, max);
 }
}

/**
 * lsp_symbols_answered - Hand the caller the outline or the workspace
 * symbols (SymbolInformation carries a location with its uri, which
 * lsp_collect_symbols reads as the symbol's file).
 */
static void lsp_symbols_answered(E_D_REQ_END how, struct json_object *resp,
                                 void *ctx)
{
 lsp_typed *t = ctx;
 struct json_object *result;
 int n = -1;

 if (how == E_D_REQ_DONE)
 {
  lsp_free_syms(t->s);
  result = lsp_result_array(resp);
  if (result)
   lsp_collect_symbols(t->s, result, t->max);
  n = t->s->nsyms;
 }
 t->syms_fn(how, t->s->syms, n, t->ctx);
 lsp_typed_free(t);
}

/**
 * lsp_typed_symbols - A typed request whose answer is symbols.
 */
static lsp_typed *lsp_typed_symbols(e_lsp_session *s, int max,
                                    e_lsp_symbols_fn fn, void *ctx)
{
 lsp_typed *t = lsp_typed_new(s, ctx);

 if (!t)
  return NULL;
 t->syms_fn = fn;
 t->max = max;
 return t;
}

int e_lsp_document_symbols_async(e_lsp_session *s, const char *path, int max,
                                 e_lsp_symbols_fn fn, void *ctx)
{
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_symbols(s, max, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 lsp_typed_at(t, path);
 return lsp_typed_send(s, "textDocument/documentSymbol",
                       lsp_doc_params(t->uri), lsp_symbols_answered, t);
}

int e_lsp_workspace_symbols_async(e_lsp_session *s, const char *query, int max,
                                  e_lsp_symbols_fn fn, void *ctx)
{
 struct json_object *args;
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_symbols(s, max, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 args = json_object_new_object();
 json_object_object_add(args, "query",
                        json_object_new_string(query ? query : ""));
 return lsp_typed_send(s, "workspace/symbol", args, lsp_symbols_answered, t);
}

/* Store one Call/TypeHierarchyItem's name + location into the session cache
   (both items share the same shape).  Uses selectionRange.start (the
   identifier position, not the whole-definition range) so a future jump
   lands on the name.  Returns 1 if stored, 0 if the cache is full. */
static int lsp_store_hier_item(e_lsp_session *s, struct json_object *item,
                               int max)
{
 struct json_object *sr, *st;
 const char *name, *uri;

 if (!item || s->nsyms >= max || s->nsyms >= LSP_KEPT(s->syms))
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
 e_lsp_units_read(st, &s->syms[s->nsyms].character);
 s->syms[s->nsyms].kind = obj_int(item, "kind", 0);
 s->nsyms++;
 return 1;
}

/**
 * lsp_hier_collected - The hierarchy's second step answered: hand the
 * caller each caller, callee, supertype or subtype (a call wraps its item
 * in "from" or "to"; a type answer is the items themselves).
 */
static void lsp_hier_collected(E_D_REQ_END how, struct json_object *resp,
                               void *ctx)
{
 lsp_typed *t = ctx;
 struct json_object *result;
 int i, len, n = -1;

 if (how == E_D_REQ_DONE)
 {
  lsp_free_syms(t->s);
  result = lsp_result_array(resp);
  len = result ? (int)json_object_array_length(result) : 0;
  for (i = 0; i < len; i++)
  {
   struct json_object *entry = json_object_array_get_idx(result, i);

   if (t->wrap)
    entry = obj_obj(entry, t->wrap);
   lsp_store_hier_item(t->s, entry, t->max);
  }
  n = t->s->nsyms;
 }
 t->syms_fn(how, t->s->syms, n, t->ctx);
 lsp_typed_free(t);
}

/**
 * lsp_hier_prepared - The prepare answered: the symbol under the cursor is
 * pinned to an item, whose callers, callees or types are asked for next;
 * with no item (the cursor is not on a callable or a type) the caller is
 * told there are none.
 */
static void lsp_hier_prepared(E_D_REQ_END how, struct json_object *resp,
                              void *ctx)
{
 lsp_typed *t = ctx;
 struct json_object *result, *args;
 int rc;

 if (how != E_D_REQ_DONE)
 {
  t->syms_fn(how, t->s->syms, -1, t->ctx);
  lsp_typed_free(t);
  return;
 }
 result = lsp_result_array(resp);
 if (!result || json_object_array_length(result) == 0)
 {
  lsp_free_syms(t->s);
  t->syms_fn(how, t->s->syms, 0, t->ctx);
  lsp_typed_free(t);
  return;
 }
 args = json_object_new_object();
 json_object_object_add(args, "item",
                        json_object_get(json_object_array_get_idx(result, 0)));
 rc = lsp_typed_step(t, t->next_method, args, lsp_hier_collected);
 if (rc == E_D_SEND_OK)
  return;
 t->syms_fn(E_D_REQ_GONE, t->s->syms, -1, t->ctx);
 lsp_typed_free(t);
}

/* The two steps of each hierarchy: the prepare, then one of two
   directions, whose answer entries wrap the item in a field or not. */
typedef struct {
 const char *prepare;
 const char *method[2];
 const char *wrap[2];
} lsp_hier_kind;

static const lsp_hier_kind lsp_hier_calls = {
 "textDocument/prepareCallHierarchy",
 { "callHierarchy/incomingCalls", "callHierarchy/outgoingCalls" },
 { "from", "to" }
};
static const lsp_hier_kind lsp_hier_types = {
 "textDocument/prepareTypeHierarchy",
 { "typeHierarchy/supertypes", "typeHierarchy/subtypes" },
 { NULL, NULL }
};

/**
 * lsp_hier_async - Start a hierarchy: the prepare goes out, and its
 * continuation asks for direction @second (0 or 1) of @kind.
 */
static int lsp_hier_async(e_lsp_session *s, const lsp_hier_kind *kind,
                          const char *path, int line, e_lsp_units character,
                          int second, int max, e_lsp_symbols_fn fn, void *ctx)
{
 lsp_typed *t;
 int dir = second ? 1 : 0;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_symbols(s, max, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 lsp_typed_at(t, path);
 t->next_method = kind->method[dir];
 t->wrap = kind->wrap[dir];
 return lsp_typed_send(s, kind->prepare,
                       lsp_text_pos(s, t->path, line, character),
                       lsp_hier_prepared, t);
}

int e_lsp_call_hierarchy_async(e_lsp_session *s, const char *path, int line,
                               e_lsp_units character, int outgoing, int max,
                               e_lsp_symbols_fn fn, void *ctx)
{
 return lsp_hier_async(s, &lsp_hier_calls, path, line, character, outgoing,
                       max, fn, ctx);
}

int e_lsp_type_hierarchy_async(e_lsp_session *s, const char *path, int line,
                               e_lsp_units character, int subtypes, int max,
                               e_lsp_symbols_fn fn, void *ctx)
{
 return lsp_hier_async(s, &lsp_hier_types, path, line, character, subtypes,
                       max, fn, ctx);
}

/* Read a {start:{line,character}, end:{...}} range object into `r`. */
static void lsp_read_range(struct json_object *range, e_lsp_range *r)
{
 struct json_object *st = range ? obj_obj(range, "start") : NULL;
 struct json_object *en = range ? obj_obj(range, "end") : NULL;

 r->start_line = st ? obj_int(st, "line", 0) : 0;
 e_lsp_units_read(st, &r->start_char);
 r->end_line = en ? obj_int(en, "line", 0) : 0;
 e_lsp_units_read(en, &r->end_char);
}

/* The most ranges a selection range answer is followed for. */
#define LSP_RANGES_MAX 64

/**
 * lsp_selection_answered - Hand the caller the nested ranges, innermost
 * first: the answer is SelectionRange[] (one per position asked), and the
 * first one's .parent chain is flattened.
 */
static void lsp_selection_answered(E_D_REQ_END how, struct json_object *resp,
                                   void *ctx)
{
 lsp_typed *t = ctx;
 e_lsp_range ranges[LSP_RANGES_MAX];
 struct json_object *result, *node = NULL;
 int n = -1;

 if (how == E_D_REQ_DONE)
 {
  n = 0;
  result = lsp_result_array(resp);
  if (result && json_object_array_length(result) > 0)
   node = json_object_array_get_idx(result, 0);
  while (node && n < t->max && n < LSP_RANGES_MAX)
  {
   lsp_read_range(obj_obj(node, "range"), &ranges[n]);
   n++;
   node = obj_obj(node, "parent");
  }
 }
 t->ranges_fn(how, ranges, n, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_selection_range_async(e_lsp_session *s, const char *path, int line,
                                e_lsp_units character, int max, e_lsp_ranges_fn fn,
                                void *ctx)
{
 struct json_object *args, *positions;
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->ranges_fn = fn;
 t->max = max;
 lsp_typed_at(t, path);
 args = lsp_doc_params(t->uri);
 positions = json_object_new_array();
 json_object_array_add(positions, e_lsp_position_json(line, character));
 json_object_object_add(args, "positions", positions);
 return lsp_typed_send(s, "textDocument/selectionRange", args,
                       lsp_selection_answered, t);
}

/* ---- semantic tokens --------------------------------------------------- */

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

/**
 * lsp_tokens_decode - Decode a semanticTokens answer into @out (at most
 * @max tokens).
 * Return: how many were decoded.
 */
static int lsp_tokens_decode(struct json_object *resp, e_lsp_sem_token *out,
                             int max)
{
 struct json_object *result = obj_obj(resp, "result");
 struct json_object *data = result ? obj_obj(result, "data") : NULL;
 int *ints;
 int i, len, got;

 if (!data || !json_object_is_type(data, json_type_array))
  return 0;
 len = json_object_array_length(data);
 ints = malloc((size_t)(len > 0 ? len : 1) * sizeof(int));
 if (!ints)
  return 0;
 for (i = 0; i < len; i++)
  ints[i] = json_object_get_int(json_object_array_get_idx(data, i));
 got = e_lsp_semantic_decode(ints, len, out, max);
 free(ints);
 return got;
}

/**
 * lsp_tokens_answered - Hand the caller the decoded tokens, or n = -1 when
 * the fetch ended unanswered (a newer one replaced it, or the server went).
 */
static void lsp_tokens_answered(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 e_lsp_sem_token *tokens = NULL;
 int n = -1;

 if (how == E_D_REQ_DONE)
 {
  tokens = malloc((size_t)(t->max > 0 ? t->max : 1) * sizeof(*tokens));
  n = tokens ? lsp_tokens_decode(resp, tokens, t->max) : 0;
 }
 t->tokens_fn(how, tokens, n, t->ctx);
 free(tokens);
 lsp_typed_free(t);
}

int e_lsp_semantic_tokens_async(e_lsp_session *s, const char *path, int max,
                                e_lsp_tokens_fn fn, void *ctx)
{
 lsp_typed *t;
 int rc;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->tokens_fn = fn;
 t->max = max;
 lsp_typed_at(t, path);
 rc = lsp_request_background(s, "textDocument/semanticTokens/full",
                             lsp_doc_params(t->uri), lsp_tokens_answered, t);
 if (rc != E_D_SEND_OK)
  lsp_typed_free(t);
 return rc;
}

/**
 * lsp_edit_offset - The byte offset in the text of a TextEdit's position.
 * @ix: the text's rows; @pos: the {line, character} object, or NULL.
 *
 * The server's character is in the session's units: the owner turns it
 * into the bytes the edit must land on.
 */
static size_t lsp_edit_offset(const e_lsp_text_index *ix, struct json_object *pos,
                              int enc)
{
 e_lsp_units u;

 e_lsp_units_read(pos, &u);
 return e_lsp_text_index_offset(ix, obj_int(pos, "line", 0), u, enc);
}

/* Apply a non-overlapping LSP TextEdit[] to `text`; return a new malloc'd
   string.  Edits are sorted by start offset and spliced left-to-right, so the
   result is independent of the order the server sent them.  The edits' columns
   are in the encoding @enc. */
static char *lsp_apply_edits(const char *text, struct json_object *edits, int enc)
{
 int n = json_object_array_length(edits), i, j;
 struct { size_t start, end; const char *nt; } *ed, tmp;
 char *out;
 size_t tlen = strlen(text), cap, w = 0, pos = 0;
 e_lsp_text_index *ix;

 if (n <= 0)
  return strdup(text);
 ed = calloc((size_t)n, sizeof(*ed));
 ix = e_lsp_text_index_new(text);
 if (!ed || !ix)
 {
  free(ed);
  e_lsp_text_index_free(ix);
  return NULL;
 }
 cap = tlen + 1;
 for (i = 0; i < n; i++)
 {
  struct json_object *e = json_object_array_get_idx(edits, i);
  struct json_object *rng = obj_obj(e, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  struct json_object *en = rng ? obj_obj(rng, "end") : NULL;
  ed[i].start = st ? lsp_edit_offset(ix, st, enc) : 0;
  ed[i].end = en ? lsp_edit_offset(ix, en, enc) : ed[i].start;
  ed[i].nt = obj_str(e, "newText");
  if (!ed[i].nt)
   ed[i].nt = "";
  cap += strlen(ed[i].nt);
 }
 for (i = 1; i < n; i++)            /* insertion sort by start offset */
  for (j = i; j > 0 && ed[j].start < ed[j - 1].start; j--)
  {  tmp = ed[j];  ed[j] = ed[j - 1];  ed[j - 1] = tmp;  }

 out = malloc(cap);
 e_lsp_text_index_free(ix);
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
                                      const char *current_text, int enc,
                                      int *other_files)
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
  return lsp_apply_edits(current_text, myedits, enc);
 return strdup(current_text);          /* nothing in this file */
}

/* The layout every format request asks the server for. */
#define LSP_FORMAT_TAB_SIZE 2

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
                         e_lsp_position_json(range->start_line, range->start_char));
  json_object_object_add(r, "end",
                         e_lsp_position_json(range->end_line, range->end_char));
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
static char *lsp_format_text(struct json_object *resp, const char *base, int enc)
{
 struct json_object *result = obj_obj(resp, "result");

 if (!result || !json_object_is_type(result, json_type_array) ||
     json_object_array_length(result) == 0)
  return NULL;
 return lsp_apply_edits(base, result, enc);
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
  out = lsp_format_text(resp, t->base, t->s->encoding);
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

/**
 * lsp_rename_answered - Hand the caller the text sent with the rename's
 * edits for this file applied, and how many other files it changes.
 */
static void lsp_rename_answered(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 char *out = NULL;
 int others = 0;

 if (how == E_D_REQ_DONE)
  out = lsp_apply_workspace_edit(obj_obj(resp, "result"), t->uri, t->base,
                                 t->s->encoding, &others);
 t->applied_fn(how, out, others, t->ctx);
 lsp_typed_free(t);
}

/**
 * lsp_typed_on_text - A typed request whose answer edits @current_text.
 * Return: the typed context, or NULL without memory.
 */
static lsp_typed *lsp_typed_on_text(e_lsp_session *s, const char *path,
                                    const char *current_text,
                                    e_lsp_applied_fn fn, void *ctx)
{
 lsp_typed *t = lsp_typed_new(s, ctx);

 if (!t)
  return NULL;
 t->applied_fn = fn;
 t->base = strdup(current_text);
 if (!t->base)
 {
  lsp_typed_free(t);
  return NULL;
 }
 lsp_typed_at(t, path);
 return t;
}

int e_lsp_rename_async(e_lsp_session *s, const char *path, int line,
                       e_lsp_units character, const char *new_name,
                       const char *current_text, e_lsp_applied_fn fn,
                       void *ctx)
{
 struct json_object *args;
 lsp_typed *t;

 if (!s || !fn || !current_text || !new_name)
  return E_D_SEND_GONE;
 t = lsp_typed_on_text(s, path, current_text, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 args = lsp_text_pos(s, t->path, line, character);
 json_object_object_add(args, "newName", json_object_new_string(new_name));
 return lsp_typed_send(s, "textDocument/rename", args, lsp_rename_answered, t);
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
static struct json_object *lsp_point_range(int line, e_lsp_units character)
{
 struct json_object *rng = json_object_new_object();

 json_object_object_add(rng, "start", e_lsp_position_json(line, character));
 json_object_object_add(rng, "end", e_lsp_position_json(line, character));
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

/**
 * lsp_actions_fill - Keep the actions of a codeAction answer in the session
 * (the previous ones are freed), the whole array too, so the chosen one is
 * run without asking again.
 * Return: how many were kept.
 */
static int lsp_actions_fill(e_lsp_session *s, struct json_object *resp, int max)
{
 struct json_object *result = lsp_result_array(resp);
 int i, n = result ? (int)json_object_array_length(result) : 0;

 lsp_free_acts(s);
 if (n > 0)
  s->acts_raw = json_object_get(result);
 for (i = 0; i < n && s->nacts < max && s->nacts < LSP_KEPT(s->acts); i++)
 {
  struct json_object *a = json_object_array_get_idx(result, i);
  const char *title = obj_str(a, "title");        /* CodeAction or Command */

  if (!title)
   continue;
  s->acts[s->nacts].title = strdup(title);
  s->acts[s->nacts].has_edit = obj_obj(a, "edit") ? 1 : 0;
  s->nacts++;
 }
 return s->nacts;
}

/**
 * lsp_actions_answered - Hand the caller the actions offered.
 */
static void lsp_actions_answered(E_D_REQ_END how, struct json_object *resp,
                                 void *ctx)
{
 lsp_typed *t = ctx;
 int n = -1;

 if (how == E_D_REQ_DONE)
  n = lsp_actions_fill(t->s, resp, t->max);
 t->acts_fn(how, t->s->acts, n, t->ctx);
 lsp_typed_free(t);
}

int e_lsp_code_actions_async(e_lsp_session *s, const char *path, int line,
                             e_lsp_units character, int max, e_lsp_actions_fn fn,
                             void *ctx)
{
 struct json_object *args, *actx;
 lsp_typed *t;

 if (!s || !fn)
  return E_D_SEND_GONE;
 t = lsp_typed_new(s, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->acts_fn = fn;
 t->max = max;
 lsp_typed_at(t, path);
 args = lsp_doc_params(t->uri);
 json_object_object_add(args, "range", lsp_point_range(line, character));
 actx = json_object_new_object();
 json_object_object_add(actx, "diagnostics",
                        lsp_diags_covering(s, t->uri, line));
 json_object_object_add(args, "context", actx);
 return lsp_typed_send(s, "textDocument/codeAction", args,
                       lsp_actions_answered, t);
}

/**
 * lsp_action_tell - The chosen action has run: hand the caller the text,
 * or NULL when the file is untouched, and forget the request.
 */
static void lsp_action_tell(lsp_typed *t, E_D_REQ_END how, char *newtext,
                            int others)
{
 t->applied_fn(how, newtext, others, t->ctx);
 lsp_typed_free(t);
}

/**
 * lsp_action_executed - The command answered: whatever the server sent
 * back with workspace/applyEdit meanwhile is the action's change; a
 * command that changed no file (a "run main") hands back NULL.
 */
static void lsp_action_executed(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 lsp_work *w = t->work;
 char *out = NULL;

 (void)resp;
 if (how == E_D_REQ_DONE && w->applied)
 {
  out = w->text;
  w->text = NULL;
 }
 lsp_action_tell(t, how, out, w->others);
}

/**
 * lsp_action_command - The command of an action: a CodeAction whose
 * "command" is a Command object, or an entry that is itself a Command.
 * @act: the action; @cmdobj: set to the object holding the arguments.
 * Return: the command's id, or NULL when it has none.
 */
static const char *lsp_action_command(struct json_object *act,
                                      struct json_object **cmdobj)
{
 struct json_object *c = obj_obj(act, "command");

 if (c && json_object_is_type(c, json_type_object))
 {
  *cmdobj = c;
  return obj_str(c, "command");
 }
 *cmdobj = act;
 return obj_str(act, "command");
}

/**
 * lsp_action_execute - Run the action's command with
 * workspace/executeCommand; the server's applyEdit while it is out edits
 * a working copy of the text sent, held by this request alone.
 */
static void lsp_action_execute(lsp_typed *t, struct json_object *act)
{
 struct json_object *cmdobj, *args, *arguments;
 const char *command = lsp_action_command(act, &cmdobj);
 int rc;

 if (!command)
 {
  lsp_action_tell(t, E_D_REQ_DONE, NULL, 0);
  return;
 }
 t->work = calloc(1, sizeof(*t->work));
 if (t->work)
  t->work->text = strdup(t->base);
 if (!t->work || !t->work->text)
 {
  lsp_action_tell(t, E_D_REQ_GONE, NULL, 0);
  return;
 }
 snprintf(t->work->uri, sizeof t->work->uri, "%s", t->uri);
 args = json_object_new_object();
 json_object_object_add(args, "command", json_object_new_string(command));
 arguments = obj_obj(cmdobj, "arguments");
 if (arguments && json_object_is_type(arguments, json_type_array))
  json_object_object_add(args, "arguments", json_object_get(arguments));
 rc = lsp_request_work(t->s, LSP_PEND_CALL, "workspace/executeCommand", args,
                       lsp_action_executed, t, t->work);
 if (rc != E_D_SEND_OK)
  lsp_action_tell(t, E_D_REQ_GONE, NULL, 0);
}

/**
 * lsp_action_apply - Apply an action that is complete: its edit to the
 * text sent, or else its command.
 */
static void lsp_action_apply(lsp_typed *t, struct json_object *act)
{
 struct json_object *edit = obj_obj(act, "edit");
 int others = 0;
 char *out;

 if (!edit)
 {
  lsp_action_execute(t, act);
  return;
 }
 out = lsp_apply_workspace_edit(edit, t->uri, t->base, t->s->encoding,
                                &others);
 lsp_action_tell(t, E_D_REQ_DONE, out, others);
}

/**
 * lsp_action_resolved - The server filled in an action that carried only
 * data (how Metals delivers its refactors); one it could not resolve is
 * run as it was.
 */
static void lsp_action_resolved(E_D_REQ_END how, struct json_object *resp,
                                void *ctx)
{
 lsp_typed *t = ctx;
 struct json_object *result;

 if (how != E_D_REQ_DONE)
 {
  lsp_action_tell(t, how, NULL, 0);
  return;
 }
 result = obj_obj(resp, "result");
 if (result && json_object_is_type(result, json_type_object))
 {
  json_object_put(t->action);
  t->action = json_object_get(result);
 }
 lsp_action_apply(t, t->action);
}

/**
 * lsp_action_needs_resolve - Does the action carry only data, its edit
 * and command still to be asked for?
 */
static int lsp_action_needs_resolve(struct json_object *act)
{
 return !obj_obj(act, "edit") && !obj_obj(act, "command") &&
        obj_obj(act, "data") != NULL;
}

int e_lsp_code_action_run_async(e_lsp_session *s, int index, const char *path,
                                const char *current_text, e_lsp_applied_fn fn,
                                void *ctx)
{
 lsp_typed *t;
 int rc;

 if (!s || !fn || !current_text)
  return E_D_SEND_GONE;
 if (!s->acts_raw || index < 0 ||
     index >= (int)json_object_array_length(s->acts_raw))
  return E_D_SEND_FAILED;
 t = lsp_typed_on_text(s, path, current_text, fn, ctx);
 if (!t)
  return E_D_SEND_FAILED;
 t->action = json_object_get(json_object_array_get_idx(s->acts_raw, index));
 if (!lsp_action_needs_resolve(t->action))
 {
  lsp_action_apply(t, t->action);
  return E_D_SEND_OK;
 }
 rc = lsp_typed_step(t, "codeAction/resolve", json_object_get(t->action),
                     lsp_action_resolved);
 if (rc != E_D_SEND_OK)
  lsp_typed_free(t);
 return rc;
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
