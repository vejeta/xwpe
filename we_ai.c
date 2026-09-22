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
#include <sys/types.h>
#include <sys/wait.h>
#include <json-c/json.h>

#include "we_ai.h"
#include "we_ai_http.h"

static char *ai_strdup(const char *s);            /* defined below; used early */
static char *ai_read_first_line(const char *path); /* defined below; used early */

/* ----- configuration globals (defaults chosen so an empty config works) --- */
int   e_ai_backend  = WPE_AI_OLLAMA;
char *e_ai_endpoint = NULL;            /* filled by wpe_ai_config_init()       */
char *e_ai_model    = NULL;            /* "" => auto-pick the first model       */
char *e_ai_cafile   = NULL;            /* extra CA/self-signed cert to trust (TLS),
                                          for a local HTTPS bridge; NULL => system
                                          CA store only                          */
char *e_ai_provider = NULL;            /* active provider profile name, or NULL   */
char *e_ai_key      = NULL;            /* OpenAI key typed in the dialog this
                                          session; overrides the key files/env so
                                          Alt-M works before the provider is saved */

/* ---- provider profiles: named OpenAI-compatible endpoints ---------------- */
static struct wpe_ai_provider *g_ai_providers;
static int                     g_ai_provider_n;

int wpe_ai_provider_count(void) { return g_ai_provider_n; }

const struct wpe_ai_provider *wpe_ai_provider_get(int i)
{
 return (i >= 0 && i < g_ai_provider_n) ? &g_ai_providers[i] : NULL;
}

const struct wpe_ai_provider *wpe_ai_provider_find(const char *name)
{
 int i;
 if (!name) return NULL;
 for (i = 0; i < g_ai_provider_n; i++)
  if (!strcmp(g_ai_providers[i].name, name)) return &g_ai_providers[i];
 return NULL;
}

/* Add a named provider, or replace the fields of one that already has that name.
 * Copies every value; a NULL model/cafile is stored as "". */
void wpe_ai_provider_set(const char *name, const char *endpoint,
                         const char *model, const char *cafile)
{
 int i;
 struct wpe_ai_provider *p = NULL;
 if (!name || !*name || !endpoint) return;
 for (i = 0; i < g_ai_provider_n; i++)
  if (!strcmp(g_ai_providers[i].name, name)) { p = &g_ai_providers[i]; break; }
 if (!p) {
  struct wpe_ai_provider *na = realloc(g_ai_providers,
                                       (size_t)(g_ai_provider_n + 1) * sizeof *na);
  if (!na) return;
  g_ai_providers = na;
  p = &g_ai_providers[g_ai_provider_n++];
  memset(p, 0, sizeof *p);
  p->name = ai_strdup(name);
 }
 free(p->endpoint); p->endpoint = ai_strdup(endpoint);
 free(p->model);    p->model    = ai_strdup(model  ? model  : "");
 free(p->cafile);   p->cafile   = ai_strdup(cafile ? cafile : "");
}

int wpe_ai_backend_from_name(const char *name)
{
 if (!name)                          return WPE_AI_OLLAMA;
 if (!strcasecmp(name, "openai"))    return WPE_AI_OPENAI;
 if (!strcasecmp(name, "claude") ||
     !strcasecmp(name, "anthropic")) return WPE_AI_CLAUDE;
 if (!strcasecmp(name, "claudecli") ||
     !strcasecmp(name, "claude-cli") ||
     !strcasecmp(name, "cli"))       return WPE_AI_CLAUDECLI;
 if (!strcasecmp(name, "mock"))      return WPE_AI_MOCK;
 return WPE_AI_OLLAMA;
}

const char *wpe_ai_backend_name(int backend)
{
 switch (backend) {
  case WPE_AI_OPENAI:    return "openai";
  case WPE_AI_CLAUDE:    return "claude";
  case WPE_AI_CLAUDECLI: return "claudecli";
  case WPE_AI_MOCK:      return "mock";
  default:               return "ollama";
 }
}

/* Search PATH for an executable (small, self-contained). */
static int ai_which(const char *prog)
{
 const char *path = getenv("PATH");
 char buf[1024];
 size_t plen = strlen(prog);
 if (!path || !*path) path = "/usr/bin:/bin:/usr/local/bin";
 while (*path) {
  const char *colon = strchr(path, ':');
  size_t l = colon ? (size_t)(colon - path) : strlen(path);
  if (l > 0 && l + plen + 2 < sizeof buf) {
   memcpy(buf, path, l);
   buf[l] = '/';
   memcpy(buf + l + 1, prog, plen + 1);
   if (access(buf, X_OK) == 0) return 1;
  }
  if (!colon) break;
  path = colon + 1;
 }
 return 0;
}

int wpe_ai_claude_cli_available(void) { return ai_which("claude"); }

/* ----- permission dial + claudecli control globals ----------------------- */
int   e_ai_policy          = WPE_AI_POLICY_ASK;
int   e_ai_agent_engine    = WPE_AI_ENGINE_BUILTIN;
int   e_ai_cli_mode        = WPE_AI_CLI_TEXTONLY;
char *e_ai_resume_session  = NULL;
char *e_ai_last_session_id = NULL;

int wpe_ai_policy_from_name(const char *name)
{
 if (!name)                        return WPE_AI_POLICY_ASK;
 if (!strcasecmp(name, "edits"))   return WPE_AI_POLICY_EDITS;
 if (!strcasecmp(name, "auto"))    return WPE_AI_POLICY_AUTO;
 return WPE_AI_POLICY_ASK;
}

const char *wpe_ai_policy_name(int policy)
{
 switch (policy) {
  case WPE_AI_POLICY_EDITS: return "edits";
  case WPE_AI_POLICY_AUTO:  return "auto";
  default:                  return "ask";
 }
}

char *wpe_ai_run_capture(const char *cmd)
{
 FILE *fp;
 char *out, full[2100];
 size_t cap = 4096, len = 0;
 int ch;
 snprintf(full, sizeof full, "%s 2>&1", cmd);
 fp = popen(full, "r");
 if (!fp) return ai_strdup("(could not run command)");
 out = malloc(cap);
 if (!out) { pclose(fp); return ai_strdup("(out of memory)"); }
 while ((ch = fgetc(fp)) != EOF && len < 6000) {
  if (len + 2 > cap) { char *nb; cap *= 2; nb = realloc(out, cap); if (!nb) break; out = nb; }
  out[len++] = (char)ch;
 }
 out[len] = '\0';
 pclose(fp);
 return out;
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
 if ((e = getenv("XWPE_AI_POLICY")))
  e_ai_policy = wpe_ai_policy_from_name(e);
 if ((e = getenv("XWPE_AI_AGENT_ENGINE")))
  e_ai_agent_engine = (!strcmp(e, "claude-code") || !strcmp(e, "host"))
                        ? WPE_AI_ENGINE_CLAUDE_HOST : WPE_AI_ENGINE_BUILTIN;

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

 if ((e = getenv("XWPE_AI_CAFILE"))) {
  free(e_ai_cafile);
  e_ai_cafile = (*e) ? ai_strdup(e) : NULL;
 }
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

/* The path prefix carried by the configured endpoint URL (e.g. "/v1",
 * "/openai/v1", "/api/v1"), trailing slash removed; empty for a bare host.
 * Lets the OpenAI-compatible endpoint include the version segment the way Groq
 * (/openai/v1) and OpenRouter (/api/v1) do, instead of assuming /v1 at the root. */
static void ai_endpoint_prefix(char *out, size_t n)
{
 char host[256], path[512];
 int port = 0, https = 0;
 size_t len;
 out[0] = '\0';
 if (!e_ai_endpoint || !n) return;
 if (wpe_http_parse_url(e_ai_endpoint, host, sizeof host, &port, &https,
                        path, sizeof path) != 0)
  return;
 len = strlen(path);
 while (len && path[len - 1] == '/') path[--len] = '\0';   /* drop trailing '/' */
 if (len) snprintf(out, n, "%s", path);
}

/* Ollama serves its API at the host ROOT (/api/tags, /api/chat); a URL that
 * carries a path segment (/v1, /openai/v1, /api/v1) is an OpenAI-compatible base,
 * not an Ollama one.  When the endpoint is such an OpenAI-style URL (as it is
 * right after switching the dialog's Backend radio from an OpenAI provider to
 * Ollama, since the endpoint field is shared) return the Ollama default so the
 * list/chat go to the local server; otherwise NULL to keep the current endpoint
 * (a bare host -- local or a remote Ollama -- is left as-is). */
const char *wpe_ai_ollama_endpoint_fixup(const char *url)
{
 char host[256], path[512];
 int port = 0, https = 0;
 size_t l;
 if (!url || !*url) return "http://localhost:11434";
 if (wpe_http_parse_url(url, host, sizeof host, &port, &https, path, sizeof path) != 0)
  return "http://localhost:11434";
 l = strlen(path);
 while (l && path[l - 1] == '/') path[--l] = '\0';
 return l ? "http://localhost:11434" : NULL;
}

/* Write the OpenAI API key for a provider (NULL/"" => the generic file) to
 * $XDG_CONFIG_HOME/xwpe/openai-api-key[-<provider>] with 0600 perms, so a key
 * typed in the settings dialog persists.  Returns 0 on success. */
int wpe_ai_write_openai_key(const char *provider, const char *key)
{
 const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
 char dir[1024], path[1200];
 FILE *fp;
 int fd;
 if (!key) return -1;
 if (xdg && *xdg) snprintf(dir, sizeof dir, "%s/xwpe", xdg);
 else if (home)   snprintf(dir, sizeof dir, "%s/.config/xwpe", home);
 else return -1;
 if (provider && *provider)
  snprintf(path, sizeof path, "%s/openai-api-key-%s", dir, provider);
 else
  snprintf(path, sizeof path, "%s/openai-api-key", dir);
 fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
 if (fd < 0) return -1;
 fp = fdopen(fd, "w");
 if (!fp) { close(fd); return -1; }
 fprintf(fp, "%s\n", key);
 fclose(fp);
 return 0;
}

/* Read the stored OpenAI key for a provider from its file (NULL/"" => the generic
 * file), for showing in the settings dialog so the user can see a key is set.
 * Does NOT consult the environment (that is a runtime override, not the stored
 * key).  Returns a malloc'd string or NULL. */
char *wpe_ai_read_openai_key_file(const char *provider)
{
 const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
 char path[1200];
 if (xdg && *xdg) {
  if (provider && *provider) snprintf(path, sizeof path, "%s/xwpe/openai-api-key-%s", xdg, provider);
  else                       snprintf(path, sizeof path, "%s/xwpe/openai-api-key", xdg);
 } else if (home) {
  if (provider && *provider) snprintf(path, sizeof path, "%s/.config/xwpe/openai-api-key-%s", home, provider);
  else                       snprintf(path, sizeof path, "%s/.config/xwpe/openai-api-key", home);
 } else return NULL;
 return ai_read_first_line(path);
}

/* Build the request path for a backend, honouring the endpoint's path prefix.
 * OpenAI-compatible and Claude take their path relative to that prefix (a bare
 * host defaults to /v1); Ollama's paths are absolute from the host root. */
static void ai_make_path(int backend, const char *suffix, char *out, size_t n)
{
 char pfx[512];
 ai_endpoint_prefix(pfx, sizeof pfx);
 if (backend == WPE_AI_OPENAI || backend == WPE_AI_CLAUDE)
  snprintf(out, n, "%s%s", pfx[0] ? pfx : "/v1", suffix);
 else
  snprintf(out, n, "%s%s", pfx, suffix);          /* ollama: prefix normally "" */
}

/* The chat/completion request path for a backend (into out). */
static void ai_chat_path(int backend, char *out, size_t n)
{
 switch (backend) {
  case WPE_AI_OPENAI: ai_make_path(backend, "/chat/completions", out, n); break;
  case WPE_AI_CLAUDE: ai_make_path(backend, "/messages", out, n);         break;
  default:            ai_make_path(backend, "/api/chat", out, n);         break;
 }
}

static char *ai_read_first_line(const char *path)
{
 FILE *fp = fopen(path, "r");
 char line[1024], *r = NULL;
 if (!fp) return NULL;
 if (fgets(line, sizeof line, fp)) {
  size_t l = strlen(line);
  while (l && (line[l-1] == '\n' || line[l-1] == '\r' ||
               line[l-1] == ' '  || line[l-1] == '\t')) line[--l] = '\0';
  if (l) r = ai_strdup(line);
 }
 fclose(fp);
 return r;
}

/* Resolve an API key by the expectable convention: the standard env var first
 * (e.g. ANTHROPIC_API_KEY), then <ENV>_FILE, then the XDG config file
 * $XDG_CONFIG_HOME/xwpe/<file_base> (default ~/.config/xwpe/<file_base>).
 * Returns malloc'd key or NULL. */
static char *ai_get_api_key(const char *env_name, const char *file_base)
{
 const char *e = getenv(env_name), *home, *xdg;
 char fenv[80], path[1024];
 if (e && *e) return ai_strdup(e);
 snprintf(fenv, sizeof fenv, "%s_FILE", env_name);
 e = getenv(fenv);
 if (e && *e) { char *k = ai_read_first_line(e); if (k) return k; }
 xdg = getenv("XDG_CONFIG_HOME");
 home = getenv("HOME");
 if (xdg && *xdg) snprintf(path, sizeof path, "%s/xwpe/%s", xdg, file_base);
 else if (home)   snprintf(path, sizeof path, "%s/.config/xwpe/%s", home, file_base);
 else return NULL;
 return ai_read_first_line(path);
}

/* Fill hdrs[] (NULL-terminated) with malloc'd "Key: Value" strings; count. */
static int ai_headers(int backend, char *hdrs[], int max)
{
 int n = 0;
 char *k;
 char b[600];
 if (n < max) hdrs[n++] = ai_strdup("Content-Type: application/json");
 if (backend == WPE_AI_OPENAI) {
  /* A key typed in the dialog this session wins (so Alt-M works before the
     provider is saved); then a provider profile's own key file
     (openai-api-key-<name>) so Groq/Proton/... can each carry a distinct key;
     then the generic OPENAI_API_KEY / openai-api-key. */
  k = NULL;
  if (e_ai_key && *e_ai_key) k = ai_strdup(e_ai_key);
  if (!k && e_ai_provider && *e_ai_provider) {
   char base[160];
   snprintf(base, sizeof base, "openai-api-key-%s", e_ai_provider);
   k = ai_get_api_key("OPENAI_API_KEY", base);
  }
  if (!k) k = ai_get_api_key("OPENAI_API_KEY", "openai-api-key");
  if (k && n < max) {
   snprintf(b, sizeof b, "Authorization: Bearer %s", k);
   hdrs[n++] = ai_strdup(b);
  }
  free(k);
 } else if (backend == WPE_AI_CLAUDE) {
  k = ai_get_api_key("ANTHROPIC_API_KEY", "anthropic-api-key");
  if (k && n < max) {
   snprintf(b, sizeof b, "x-api-key: %s", k);
   hdrs[n++] = ai_strdup(b);
  }
  free(k);
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
                          char **delta, char **think, int *done)
{
 const char *p = line;
 struct json_object *o, *tmp;
 (void)len;
 *delta = NULL;
 if (think) *think = NULL;
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
   if (think && json_object_object_get_ex(tmp, "thinking", &c))
    *think = ai_strdup(json_object_get_string(c));
  }
  if (json_object_object_get_ex(o, "done", &tmp) && json_object_get_boolean(tmp))
   *done = 1;
 } else if (backend == WPE_AI_OPENAI) {
  struct json_object *ch;
  if (json_object_object_get_ex(o, "choices", &ch) &&
      json_object_array_length(ch) > 0) {
   struct json_object *c0 = json_object_array_get_idx(ch, 0), *d, *cont, *fin, *rsn;
   if (json_object_object_get_ex(c0, "delta", &d)) {
    if (json_object_object_get_ex(d, "content", &cont)) {
     const char *s = json_object_get_string(cont);
     if (s) *delta = ai_strdup(s);
    }
    /* Reasoning models served over the OpenAI API (gpt-oss, deepseek-r1, ...)
       stream their chain of thought in a separate "reasoning" field, often for
       many deltas before any content -- and a reply can be reasoning-only.  Keep
       it as the fallback (same as Ollama's "thinking") so such a turn shows the
       reasoning rather than nothing ("(no answer)"). */
    if (think && json_object_object_get_ex(d, "reasoning", &rsn)) {
     const char *s = json_object_get_string(rsn);
     if (s && *s) *think = ai_strdup(s);
    }
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
 if (backend == WPE_AI_CLAUDECLI) {
  if (wpe_ai_claude_cli_available()) return 0;
  if (errbuf) snprintf(errbuf, errsz,
    "Claude Code CLI not found - install `claude` and log in (claude auth)");
  return -1;
 }
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
 char path[600];
 int nh, i, st = 0, cnt = 0, rc;
 struct json_object *o, *arr, *it, *nm;

 if (backend == WPE_AI_MOCK) {
  if (max > 0) { names[0] = ai_strdup("mock-model"); return 1; }
  return 0;
 }
 if (backend == WPE_AI_CLAUDECLI) {
  /* The Claude CLI takes --model with an alias; "default" means "no --model",
     i.e. whatever the Claude Code login selects. */
  if (cnt < max) names[cnt++] = ai_strdup("default");
  if (cnt < max) names[cnt++] = ai_strdup("sonnet");
  if (cnt < max) names[cnt++] = ai_strdup("opus");
  if (cnt < max) names[cnt++] = ai_strdup("haiku");
  return cnt;
 }
 if (backend == WPE_AI_CLAUDE) {
  if (cnt < max) names[cnt++] = ai_strdup("claude-sonnet-5");
  if (cnt < max) names[cnt++] = ai_strdup("claude-opus-5");
  return cnt;
 }
 ai_make_path(backend, (backend == WPE_AI_OPENAI) ? "/models" : "/api/tags",
              path, sizeof path);
 nh = ai_headers(backend, hdrs, 8);
 rc = ai_fetch("GET", path, hdrs, NULL, 5000, &resp, &st, errbuf, errsz);
 for (i = 0; i < nh; i++) free(hdrs[i]);
 wpe_ai_trace("MODELS path=%s rc=%d status=%d nhdr=%d err=%s resp=%.160s",
              path, rc, st, nh, errbuf && errbuf[0] ? errbuf : "-",
              resp ? resp : "(null)");
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

/* Case-insensitive substring test (strcasestr is not portable). */
static int ai_name_has(const char *hay, const char *needle)
{
 size_t nl = strlen(needle);
 if (!hay) return 0;
 for (; *hay; hay++)
  if (!strncasecmp(hay, needle, nl)) return 1;
 return 0;
}

/* Model-name fragments that mark a code-tuned model.  A code editor should
 * answer coding questions well, so auto-selection favours these over a general
 * chat model (which, if small, tends to ramble). */
static const char *ai_code_model_hints[] = {
 "qwen2.5-coder", "qwen3-coder", "deepseek-coder", "codestral", "codellama",
 "starcoder", "codegemma", "granite-code", "coder", "code", NULL
};

static int ai_is_code_model(const char *name)
{
 int h;
 for (h = 0; ai_code_model_hints[h]; h++)
  if (ai_name_has(name, ai_code_model_hints[h])) return 1;
 return 0;
}

/* A rough "how strong" score from the parameter size in an Ollama tag: the
 * number before a 'b'/'B' ("7b", "32b", "3.8b"), scaled by 10 so a decimal is
 * kept (3.8b -> 38, 32b -> 320).  0 when the tag carries no size.  A model the
 * user chose to install is trusted, so among equally-suitable models the larger
 * (stronger) one wins. */
static int ai_model_size(const char *name)
{
 int best = 0;
 const char *p;
 if (!name) return 0;
 for (p = name; *p; ) {
  if (*p >= '0' && *p <= '9') {
   double v = 0; double frac;
   const char *q = p;
   while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
   if (*q == '.') { q++; for (frac = 0.1; *q >= '0' && *q <= '9'; q++, frac *= 0.1) v += (*q - '0') * frac; }
   if (*q == 'b' || *q == 'B') { int s = (int)(v * 10 + 0.5); if (s > best) best = s; }
   p = (q > p) ? q : p + 1;
  } else p++;
 }
 return best;
}

/* Index of the model to auto-select: prefer a code-tuned model, and within the
 * same class prefer the larger (stronger) one; fall back to the first listed. */
static int ai_pick_model(char **names, int n)
{
 int i, best = -1, best_code = -1, best_size = -1;
 for (i = 0; i < n; i++) {
  int code = ai_is_code_model(names[i]);
  int size = ai_model_size(names[i]);
  if (best < 0 || code > best_code || (code == best_code && size > best_size)) {
   best = i; best_code = code; best_size = size;
  }
 }
 return best < 0 ? 0 : best;
}

int wpe_ai_ensure_model(char *errbuf, size_t errsz)
{
 char *names[32];
 int n, i, pick;
 if (e_ai_backend == WPE_AI_CLAUDECLI) return 0;  /* CLI picks its own model */
 if (e_ai_model && *e_ai_model) return 0;
 if (errbuf && errsz) errbuf[0] = '\0';
 n = wpe_ai_list_models(e_ai_backend, names, 32, errbuf, errsz);
 if (n <= 0) {
  /* Keep a transport error (e.g. "Ollama not reachable ...") if list_models set
     one; only fill in the "nothing installed" case, with a copy-paste next step. */
  if (errbuf && errsz && !errbuf[0]) {
   if (e_ai_backend == WPE_AI_OLLAMA)
    snprintf(errbuf, errsz,
      "No Ollama models found.  Install a code model, e.g.:  ollama pull qwen2.5-coder");
   else
    snprintf(errbuf, errsz, "no models available");
  }
  return -1;
 }
 pick = ai_pick_model(names, n);                  /* prefer a code model, then larger */
 free(e_ai_model);
 e_ai_model = ai_strdup(names[pick]);
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
 int             is_subproc;   /* claudecli: read a child pipe, not a socket   */
 pid_t           pid;
 int             out_fd;       /* child stdout (subprocess transport)          */
 int             had_error;    /* backend reported a failure, not a real reply */
 /* Ollama reasoning models (qwen3, deepseek-r1) stream their chain of thought
    in a separate "thinking" field and can finish a turn with "content" empty.
    We render content only, so accumulate thinking as a fallback and, if the
    turn produced no content at all, hand the thinking over as the reply -- so a
    "thought but did not answer" turn is never shown as blank. */
 char           *think;
 size_t          think_len, think_cap;
 int             got_content;  /* the turn emitted at least one content delta   */
 int             think_used;   /* the thinking fallback was already handed over  */
};

/* Flatten the request into a single prompt for the `claude` CLI (one -p call).
 * claude -p takes one text blob, not a role-structured message array, so LABEL
 * each turn: without "User:"/"Assistant:" markers the model sees the prior turns
 * as one undifferentiated block and, asked about the conversation, claims to have
 * no context even though the history is present.  The labels make it read the
 * blob as the ongoing dialogue it is.  The system turn is emitted unlabelled as
 * the leading instructions. */
static const char *ai_role_label(const char *role)
{
 if (role && !strcmp(role, "assistant")) return "Assistant: ";
 if (role && !strcmp(role, "system"))    return "";
 return "User: ";
}

static char *ai_flatten_prompt(const wpe_ai_req *req)
{
 size_t cap = 1024, len = 0;
 char *p = malloc(cap);
 int i;
 if (!p) return NULL;
 p[0] = '\0';
 for (i = 0; i < req->nmsgs; i++) {
  const char *c = req->msgs[i].content ? req->msgs[i].content : "";
  const char *lbl = ai_role_label(req->msgs[i].role);
  size_t need = len + strlen(lbl) + strlen(c) + 3;
  if (need > cap) { char *np; while (need > cap) cap *= 2; np = realloc(p, cap); if (!np) { free(p); return NULL; } p = np; }
  len += (size_t)snprintf(p + len, cap - len, "%s%s\n\n", lbl, c);
 }
 return p;
}

/* Spawn `claude -p --output-format json`, feeding the prompt on stdin and
 * reading the single JSON result off the child's stdout.  Uses the user's own
 * Claude Code login -- no API key, no extra dependency. */
static int ai_claudecli_open(struct wpe_ai_stream *st, const wpe_ai_req *req)
{
 int in[2], out[2];
 pid_t pid;
 char *prompt;

 if (pipe(in) != 0) return -1;
 if (pipe(out) != 0) { close(in[0]); close(in[1]); return -1; }
 prompt = ai_flatten_prompt(req);

 pid = fork();
 if (pid < 0) {
  close(in[0]); close(in[1]); close(out[0]); close(out[1]);
  free(prompt);
  return -1;
 }
 if (pid == 0) {                      /* child */
  char *argv[24];
  int a = 0, dn;
  dup2(in[0], 0);
  dup2(out[1], 1);
  dn = open("/dev/null", O_WRONLY);
  if (dn >= 0) { dup2(dn, 2); close(dn); }
  close(in[0]); close(in[1]); close(out[0]); close(out[1]);
  argv[a++] = "claude";
  argv[a++] = "-p";
  argv[a++] = "--output-format";
  argv[a++] = "json";
  /* "default" (or empty) = let the Claude Code login pick; otherwise pass the
     chosen alias (sonnet/opus/haiku/...). */
  if (e_ai_model && *e_ai_model && strcmp(e_ai_model, "default")) {
   argv[a++] = "--model"; argv[a++] = e_ai_model;
  }
  if (e_ai_resume_session && *e_ai_resume_session) {
   argv[a++] = "--resume"; argv[a++] = e_ai_resume_session;
  }
  /* The permission dial, pre-granted (claude -p cannot prompt).  Placed last:
     --disallowedTools is variadic and swallows following bare arguments. */
  if (e_ai_cli_mode == WPE_AI_CLI_AUTO) {
   argv[a++] = "--dangerously-skip-permissions";
  } else if (e_ai_cli_mode == WPE_AI_CLI_EDITS) {
   argv[a++] = "--permission-mode"; argv[a++] = "acceptEdits";
  } else {                            /* text-only: xwpe owns every edit */
   argv[a++] = "--disallowedTools";
   argv[a++] = "Write"; argv[a++] = "Edit"; argv[a++] = "MultiEdit";
   argv[a++] = "NotebookEdit"; argv[a++] = "Bash";
  }
  argv[a] = NULL;
  execvp("claude", argv);
  _exit(127);
 }
 /* parent */
 close(in[0]);
 close(out[1]);
 if (prompt) {
  size_t pl = strlen(prompt), off = 0;
  while (off < pl) { ssize_t w = write(in[1], prompt + off, pl - off); if (w <= 0) break; off += (size_t)w; }
  free(prompt);
 }
 close(in[1]);                        /* EOF to the child's stdin */
 { int fl = fcntl(out[0], F_GETFL, 0); if (fl != -1) fcntl(out[0], F_SETFL, fl | O_NONBLOCK); }

 st->is_subproc = 1;
 st->pid = pid;
 st->out_fd = out[0];
 wpe_http_stream_init(&st->hs);
 st->hs.header_done = 1;              /* no HTTP: treat all child output as body */
 st->hs.status = 200;
 return 0;
}

/* Parse the `claude -p --output-format json` object: the reply is in .result. */
static void ai_parse_claudecli(const char *body, char **delta, int *had_error)
{
 struct json_object *o, *r, *ie;
 *delta = NULL;
 if (had_error) *had_error = 0;
 o = json_tokener_parse(body ? body : "");
 /* No JSON at all means the CLI failed before producing a result envelope
    (crash, or a bare error line): a failure, never file content. */
 if (!o) {
  *delta = ai_strdup("[claude-cli produced no output]");
  if (had_error) *had_error = 1;
  return;
 }
 if (json_object_object_get_ex(o, "result", &r))
  *delta = ai_strdup(json_object_get_string(r));
 else if (json_object_object_get_ex(o, "error", &r)) {
  *delta = ai_strdup(json_object_get_string(r));
  if (had_error) *had_error = 1;
 } else {
  *delta = ai_strdup("[claude-cli: no result field]");
  if (had_error) *had_error = 1;
 }
 /* claude -p signals failure with is_error:true and puts the human-readable
    reason (e.g. "Not logged in - Please run /login") in .result.  That text is
    a diagnostic, not a reply -- flag it so callers that ACT on the reply (Edit,
    Plan, Agent) refuse it instead of writing it into the file. */
 if (json_object_object_get_ex(o, "is_error", &ie) && json_object_get_boolean(ie))
  if (had_error) *had_error = 1;
 if (json_object_object_get_ex(o, "session_id", &r)) {   /* for --resume */
  free(e_ai_last_session_id);
  e_ai_last_session_id = ai_strdup(json_object_get_string(r));
 }
 json_object_put(o);
}

/* Mock: stage a canned HTTP response (NDJSON) in a temp file and read it back,
 * so the whole streaming path (framer + parser + fd-loop) is exercised with no
 * network and full determinism.  The reply text comes from $XWPE_AI_MOCK_REPLY
 * (default a short greeting), JSON-escaped via json-c so code payloads are safe. */
/* Successive mock replies: XWPE_AI_MOCK_REPLY may hold several turns separated
 * by the literal "@@TURN@@"; the Nth stream returns the Nth turn (last one
 * repeats).  Lets a test script a multi-step agent conversation deterministically. */
static int g_mock_turn = 0;

static char *ai_mock_segment(void)
{
 const char *full = getenv("XWPE_AI_MOCK_REPLY");
 const char *seg, *nx;
 const char *sep = "@@TURN@@";
 size_t seplen = 8, l;
 int idx = 0;
 if (!full) full = "Hello from the xwpe mock backend.";
 seg = full;
 for (;;) {
  nx = strstr(seg, sep);
  if (idx == g_mock_turn || !nx) {
   l = nx && idx == g_mock_turn ? (size_t)(nx - seg) : strlen(seg);
   break;
  }
  seg = nx + seplen;
  idx++;
 }
 g_mock_turn++;
 { char *r = malloc(l + 1); if (r) { memcpy(r, seg, l); r[l] = '\0'; } return r; }
}

static int ai_mock_open(struct wpe_ai_stream *st)
{
 char *reply = ai_mock_segment();
 struct json_object *o, *m;
 const char *body1;
 char *resp;
 size_t need;
 char tmpl[] = "/tmp/xwpe_ai_mockXXXXXX";
 int fd;

 if (!reply) return -1;
 o = json_object_new_object();
 m = json_object_new_object();
 json_object_object_add(m, "role", json_object_new_string("assistant"));
 json_object_object_add(m, "content", json_object_new_string(reply));
 free(reply);                                 /* json_object copied it */
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

 {
  /* XWPE_AI_MOCK_DELAY_MS>0 makes the mock reply arrive after a delay instead of
     instantly, so headless tests can exercise the ASYNCHRONOUS behaviour (the
     editor staying live, the spinner, Esc-cancel) that an instant reply cannot
     show.  A forked child sleeps then writes the response to a pipe -- only the
     FAKE backend sleeps; xwpe itself never blocks, it polls the pipe on its
     fd-loop exactly as it would a slow real server. */
  const char *dly = getenv("XWPE_AI_MOCK_DELAY_MS");
  int delay_ms = dly ? atoi(dly) : 0;
  if (delay_ms > 0) {
   int p[2];
   pid_t pid;
   if (pipe(p) < 0) { free(resp); return -1; }
   pid = fork();
   if (pid < 0) { free(resp); close(p[0]); close(p[1]); return -1; }
   if (pid == 0) {
    ssize_t w;
    close(p[0]);
    usleep((useconds_t)delay_ms * 1000);
    w = write(p[1], resp, strlen(resp));
    (void)w;
    close(p[1]);
    _exit(0);
   }
   close(p[1]);
   free(resp);
   { int fl = fcntl(p[0], F_GETFL, 0); if (fl != -1) fcntl(p[0], F_SETFL, fl | O_NONBLOCK); }
   memset(&st->conn, 0, sizeof st->conn);
   st->conn.fd = p[0];
   st->conn.is_tls = 0;
   strncpy(st->conn.host, "mock", sizeof st->conn.host - 1);
   st->pid = pid;                    /* reaped in wpe_ai_stream_free */
   wpe_http_stream_init(&st->hs);
   return 0;
  }
 }

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

 if (st->backend == WPE_AI_CLAUDECLI) {
  if (ai_claudecli_open(st, req) != 0) {
   if (errbuf) snprintf(errbuf, errsz, "could not start the claude CLI");
   free(st);
   return NULL;
  }
  wpe_ai_trace("stream start backend=claudecli");
  return st;
 }

 {
  char *hdrs[8];
  char *body;
  char path[600];
  int nh, i, rc;
  nh = ai_headers(st->backend, hdrs, 8);
  body = ai_build_body(st->backend, req);
  if (wpe_http_open(e_ai_endpoint, &st->conn, errbuf, errsz) != 0) {
   for (i = 0; i < nh; i++) free(hdrs[i]);
   free(body); free(st);
   return NULL;
  }
  ai_chat_path(st->backend, path, sizeof path);
  rc = wpe_http_request(&st->conn, "POST", path,
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

int wpe_ai_stream_fd(wpe_ai_stream *st)
{ return st ? (st->is_subproc ? st->out_fd : st->conn.fd) : -1; }
/* True once the completed stream carried a backend failure (e.g. the claude CLI
   not logged in) rather than a genuine reply.  Callers that ACT on the reply
   must check this before applying, so an error message is never written to a
   file or run as a tool call. */
int wpe_ai_stream_had_error(wpe_ai_stream *st)
{ return st ? st->had_error : 0; }
int wpe_ai_stream_http_status(wpe_ai_stream *st)
{ return st ? wpe_http_stream_status(&st->hs) : 0; }

int wpe_ai_stream_pump(wpe_ai_stream *st,
                       void (*cb)(const char *delta, void *ud), void *ud,
                       int *done)
{
 char buf[4096];
 char *line;
 size_t llen;

 if (st->is_subproc) {                        /* claudecli: read the child pipe */
  for (;;) {
   ssize_t r = read(st->out_fd, buf, sizeof buf);
   if (r > 0) { wpe_http_stream_push(&st->hs, buf, (size_t)r); }
   else if (r == 0) { st->eof = 1; wpe_http_stream_eof(&st->hs); break; }
   else {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) break;
    st->eof = 1; wpe_http_stream_eof(&st->hs); break;
   }
  }
  if (st->eof && !st->done) {                 /* whole reply in; parse .result */
   char *delta = NULL;
   ai_parse_claudecli(st->hs.body ? st->hs.body : "", &delta, &st->had_error);
   if (delta) { if (*delta && cb) cb(delta, ud); free(delta); }
   st->done = 1;
  }
  *done = st->done;
  return 0;
 }

 for (;;) {
  ssize_t r = wpe_http_read(&st->conn, buf, sizeof buf);
  if (r > 0) { if (wpe_http_stream_push(&st->hs, buf, (size_t)r) < 0) return -1; }
  else if (r == 0) break;                    /* nothing more available now   */
  else { st->eof = 1; wpe_http_stream_eof(&st->hs); break; }  /* EOF/error    */
 }
 while (wpe_http_stream_next_line(&st->hs, &line, &llen)) {
  char *delta = NULL, *think = NULL;
  int d = 0;
  ai_parse_line(st->backend, line, llen, &delta, &think, &d);
  if (delta) { if (*delta) { st->got_content = 1; if (cb) cb(delta, ud); } free(delta); }
  if (think) {                                /* stash chain-of-thought as a fallback */
   size_t tl = strlen(think);
   if (tl) {
    size_t need = st->think_len + tl + 1;
    if (need > st->think_cap) {
     size_t nc = st->think_cap ? st->think_cap : 256;
     char *np;
     while (need > nc) nc *= 2;
     np = realloc(st->think, nc);
     if (np) { st->think = np; st->think_cap = nc; }
    }
    if (st->think_cap >= need) { memcpy(st->think + st->think_len, think, tl + 1); st->think_len += tl; }
   }
   free(think);
  }
  if (d) st->done = 1;
 }
 /* Turn finished with no content at all (a reasoning model that only "thought"):
    hand the accumulated thinking over as the reply, once, so nothing is blank. */
 if ((st->done || st->eof) && !st->got_content && !st->think_used &&
     st->think && st->think_len) {
  if (cb) cb(st->think, ud);
  st->think_used = 1;
 }
 *done = (st->done || st->eof) ? 1 : 0;
 return 0;
}

void wpe_ai_stream_free(wpe_ai_stream *st)
{
 if (!st) return;
 if (st->is_subproc) {
  if (st->out_fd >= 0) close(st->out_fd);
 } else {
  wpe_http_close(&st->conn);
 }
 if (st->pid > 0) { int status; waitpid(st->pid, &status, 0); }  /* claudecli or delayed mock */
 wpe_http_stream_free(&st->hs);
 free(st->think);
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
                      wpe_ai_progress_cb progress, void *ud,
                      char *errbuf, size_t errsz)
{
 wpe_ai_stream *st = wpe_ai_stream_start(req, errbuf, errsz);
 struct ai_collect c;
 long deadline, start;
 int done = 0, fd;

 if (!st) return NULL;
 c.buf = NULL; c.len = 0; c.cap = 0;
 fd = wpe_ai_stream_fd(st);
 start = ai_now_ms();
 deadline = start + timeout_ms;
 if (progress) progress(ud, 0);
 while (!done) {
  struct pollfd pf[2];
  int pr;
  long left = deadline - ai_now_ms();
  if (left <= 0) { if (errbuf) snprintf(errbuf, errsz, "timed out"); break; }
  pf[0].fd = fd;            pf[0].events = POLLIN; pf[0].revents = 0;
  /* Also watch the terminal so an Edit/Plan/Agent wait is not a dead freeze:
     Esc aborts it instead of forcing the user to wait out a slow local model. */
  pf[1].fd = STDIN_FILENO;  pf[1].events = POLLIN; pf[1].revents = 0;
  /* Wake ~8x/second so the caller can spin its "working" indicator even while
     the model is silent (no tokens arriving). */
  pr = poll(pf, 2, (int)(left > 120 ? 120 : left));
  if (pr < 0) { if (errno == EINTR) continue; break; }
  if (pr == 0) {                      /* timed slice: just animate progress */
   if (progress) progress(ud, (int)((ai_now_ms() - start) / 1000));
   continue;
  }
  if (pf[1].revents & POLLIN) {
   unsigned char ch;
   if (read(STDIN_FILENO, &ch, 1) == 1 && ch == 27) {  /* Esc */
    if (errbuf) snprintf(errbuf, errsz, "cancelled");
    free(c.buf);
    wpe_ai_stream_free(st);
    return NULL;
   }
   /* any other key: swallow it and keep waiting */
  }
  if ((pf[0].revents & POLLIN) &&
      wpe_ai_stream_pump(st, ai_collect_cb, &c, &done) < 0) {
   if (errbuf) snprintf(errbuf, errsz, "transport error");
   break;
  }
  if (progress) progress(ud, (int)((ai_now_ms() - start) / 1000));
 }
 wpe_ai_stream_free(st);
 if (!done) { free(c.buf); return NULL; }
 return c.buf ? c.buf : ai_strdup("");
}

#endif /* WPE_AI */

/* Keep this a non-empty translation unit for ISO C when WPE_AI is off. */
typedef int wpe_ai_core_translation_unit;
