/* we_ai.h - optional AI assistant integration for xwpe (Chat / Edit / Agent).
 *
 * The whole feature is compiled only when configure was run with --enable-ai
 * (which defines WPE_AI).  A build without it contains no AI code at all, so the
 * editor is byte-for-byte the classic one.  This header stays empty-but-valid
 * when the feature is off.
 */
#ifndef WE_AI_H
#define WE_AI_H

#ifdef WPE_AI

#include <stddef.h>   /* size_t */

/* ----- backend selection ------------------------------------------------- */
enum {
  WPE_AI_OLLAMA    = 0, /* native Ollama HTTP API on localhost (default)       */
  WPE_AI_OPENAI    = 1, /* generic OpenAI-compatible /v1/chat/completions      */
  WPE_AI_CLAUDE    = 2, /* Anthropic Messages API (needs an API key)           */
  WPE_AI_MOCK      = 3, /* in-process canned stream: deterministic, no network */
  WPE_AI_CLAUDECLI = 4  /* subprocess: the `claude` CLI, using its own login   */
};

/* True if the Claude Code CLI (`claude`) is on PATH -- lets the UI offer the
 * subprocess backend only when it can actually work. */
int  wpe_ai_claude_cli_available(void);

/* Effective config (defaults baked in so an EMPTY config file just works).
 * Persisted in the Programming section as AIBackend/AIEndpoint/AIModel and
 * overridable at run time by XWPE_AI_BACKEND / _ENDPOINT / _MODEL. */
extern int   e_ai_backend;    /* one of the enum values; default OLLAMA        */
extern char *e_ai_endpoint;   /* base URL; default http://localhost:11434      */
extern char *e_ai_model;      /* model name; "" => auto-pick first from server */

int         wpe_ai_backend_from_name(const char *name);
const char *wpe_ai_backend_name(int backend);

/* True when the ED_AI_ENABLE edopt bit is set on the desktop (runtime toggle). */
int  wpe_ai_enabled(void);

/* Apply built-in defaults + XWPE_AI_* env overrides over whatever the config
 * file loaded.  Idempotent; call once at startup and after loading options. */
void wpe_ai_config_init(void);

/* ----- observability ----------------------------------------------------- */
/* Append one line to $XWPE_AI_TRACE, if set (same idiom as XWPE_UI_TRACE).    */
void wpe_ai_trace(const char *fmt, ...);

/* ----- chat request model ------------------------------------------------ */
typedef struct { const char *role; const char *content; } wpe_ai_msg;
typedef struct {
 const char       *model;      /* NULL/"" => use e_ai_model                    */
 const wpe_ai_msg *msgs;       /* conversation so far (system first, then turns)*/
 int               nmsgs;
} wpe_ai_req;

/* ----- reachability / models (zero-config plug-and-play) ----------------- */
/* Quick TCP reachability probe.  0 = reachable; -1 with a friendly hint in
 * errbuf (e.g. "Ollama not reachable at ... - run `ollama serve`"). */
int  wpe_ai_preflight(int backend, char *errbuf, size_t errsz);

/* List available model names (Ollama /api/tags, OpenAI /v1/models, mock: one).
 * Fills names[0..max) with malloc'd strings; returns the count or -1. */
int  wpe_ai_list_models(int backend, char **names, int max,
                        char *errbuf, size_t errsz);

/* If e_ai_model is empty, fill it with the first available model.  0 on success
 * (or already set); -1 if none could be determined (errbuf set). */
int  wpe_ai_ensure_model(char *errbuf, size_t errsz);

/* ----- streaming chat ---------------------------------------------------- */
typedef struct wpe_ai_stream wpe_ai_stream;

/* Connect + POST; returns an in-flight stream (fd ready for the fd-loop) or
 * NULL with a reason in errbuf. */
wpe_ai_stream *wpe_ai_stream_start(const wpe_ai_req *req, char *errbuf, size_t errsz);
int   wpe_ai_stream_fd(wpe_ai_stream *st);

/* Drain what is readable now (non-blocking).  For every new assistant text
 * delta, calls cb(delta, ud).  Sets *done=1 when the reply is complete.
 * Returns 0, or -1 on a transport error. */
int   wpe_ai_stream_pump(wpe_ai_stream *st,
                         void (*cb)(const char *delta, void *ud), void *ud,
                         int *done);
int   wpe_ai_stream_http_status(wpe_ai_stream *st);
void  wpe_ai_stream_free(wpe_ai_stream *st);

/* Blocking convenience: collect the FULL assistant reply (for Edit/Agent, which
 * need the whole answer before acting).  Returns malloc'd text or NULL. */
char *wpe_ai_complete(const wpe_ai_req *req, int timeout_ms,
                      char *errbuf, size_t errsz);

/* ----- line diff (we_ai_diff.c) ----------------------------------------- */
/* Unified-style line diff of a -> b (space/-/+ prefixes).  Malloc'd; NULL on
 * allocation failure.  Editor-free (unit-testable). */
char *wpe_ai_diff(const char *a, const char *b);

/* Structured diff for per-hunk accept/reject.  A segment is either kept context
 * (is_change=0, lines in a[]) or a change hunk (is_change=1, old lines in a[],
 * new lines in b[]).  Returns the segment count; *segs is malloc'd. */
typedef struct {
 int    is_change;
 char **a; int an;
 char **b; int bn;
} wpe_ai_seg;
int  wpe_ai_diff_segments(const char *a, const char *b, wpe_ai_seg **segs);
void wpe_ai_segs_free(wpe_ai_seg *segs, int n);

/* ----- editor entry points (we_ai_ui.c) --------------------------------- */
struct FNST;                        /* editor window (edit.h)                 */
int  e_ai_ui_key(struct FNST *f);   /* Alt-B: open/prompt the AI assistant    */

#endif /* WPE_AI */

#endif /* WE_AI_H */
