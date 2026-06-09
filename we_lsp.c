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
 e_lsp_symbol  syms[256];           /* engine-owned outline result         */
 int          nsyms;
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
 int i, n;

 if (!uri || !diags || !json_object_is_type(diags, json_type_array))
  return;
 if (!s->host.on_diagnostic)
  return;
 n = json_object_array_length(diags);
 for (i = 0; i < n; i++)
 {
  struct json_object *d = json_object_array_get_idx(diags, i);
  struct json_object *rng = obj_obj(d, "range");
  struct json_object *st = rng ? obj_obj(rng, "start") : NULL;
  s->host.on_diagnostic(uri_to_path(uri), obj_int(st, "line", 0),
                        obj_int(st, "character", 0),
                        obj_int(d, "severity", 1), obj_str(d, "message"),
                        s->host.ud);
 }
}

/* Answer a server->client request so the server never blocks.  We advertise no
   UI providers, so the safe reply is null / an array of nulls. */
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
   json_object_array_add(arr, NULL);
  lsp_reply(s, id, arr);
 }
 else
  /* registerCapability, workDoneProgress/create, *_refresh, unknown -> null */
  lsp_reply(s, id, NULL);
}

/* Pump messages: answer server requests, deliver diagnostics, and return the
   response whose id == want_id (caller owns it).  If want_diag_path != NULL,
   return a sentinel (non-NULL, caller must json_object_put) as soon as that
   file's diagnostics arrive.  NULL on timeout/EOF. */
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
  if (id && method)                       /* server -> client REQUEST */
  {
   lsp_answer_request(s, m);
   json_object_put(m);
   continue;
  }
  if (method)                             /* notification */
  {
   const char *meth = json_object_get_string(method);
   if (meth && !strcmp(meth, "textDocument/publishDiagnostics"))
   {
    struct json_object *params = obj_obj(m, "params");
    lsp_dispatch_diagnostics(s, params);
    if (want_diag_path)
    {
     const char *u = obj_str(params, "uri");
     if (u && !strcmp(uri_to_path(u), want_diag_path))
     {  json_object_put(m);  return json_object_new_boolean(1);  }
    }
   }
   json_object_put(m);
   continue;
  }
  if (id)                                 /* response to our request */
  {
   if (want_id >= 0 && json_object_get_int(id) == want_id)
    return m;
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
 json_object_object_add(o, "statusBarProvider", json_object_new_string("log-message"));
 json_object_object_add(o, "inputBoxProvider", json_object_new_boolean(0));
 json_object_object_add(o, "quickPickProvider", json_object_new_boolean(0));
 json_object_object_add(o, "executeClientCommandProvider", json_object_new_boolean(0));
 json_object_object_add(o, "didFocusProvider", json_object_new_boolean(0));
 json_object_object_add(o, "isHttpEnabled", json_object_new_boolean(0));
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
 json_object_object_add(caps, "textDocument", td);
 {
  struct json_object *ws = json_object_new_object();
  json_object_object_add(ws, "configuration", json_object_new_boolean(1));
  json_object_object_add(caps, "workspace", ws);
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

int e_lsp_definition(e_lsp_session *s, const char *path, int line, int character,
                     char *out_path, size_t out_sz, int *out_line, int *out_char)
{
 char abspath[PATH_MAX];
 struct json_object *resp, *result, *loc = NULL, *rng, *st;
 int id, rc = -1;

 if (!s)
  return -1;
 if (!realpath(path, abspath))
  snprintf(abspath, sizeof(abspath), "%s", path);
 id = ++s->id;
 lsp_send(s, id, "textDocument/definition", lsp_text_pos(s, abspath, line, character));
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
  free(s->syms[i].name);
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
  if (loc)
   rng = obj_obj(loc, "range");
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
 e_dap_reader_free(&s->rd);
 free(s);
}
