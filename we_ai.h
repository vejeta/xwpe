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

/* ----- permission dial (agent / plan modes) ------------------------------ */
enum { WPE_AI_POLICY_ASK = 0, WPE_AI_POLICY_EDITS = 1, WPE_AI_POLICY_AUTO = 2 };
extern int  e_ai_policy;               /* persisted AIPolicy; XWPE_AI_POLICY   */
int         wpe_ai_policy_from_name(const char *name);
const char *wpe_ai_policy_name(int policy);

/* claudecli control: how much the CLI may do on its own, and session resume.
 * The UI sets these before each call; the CLI cannot prompt in -p mode, so the
 * permission must be pre-granted according to the policy. */
enum { WPE_AI_CLI_TEXTONLY = 0, WPE_AI_CLI_EDITS = 1, WPE_AI_CLI_AUTO = 2 };
extern int   e_ai_cli_mode;
extern char *e_ai_resume_session;      /* passed as --resume <id>, or NULL     */
extern char *e_ai_last_session_id;     /* session_id from the last CLI reply   */

/* Run a shell command; capture bounded stdout+stderr (malloc'd, never NULL on
 * a spawn failure -- returns a short note instead). */
char *wpe_ai_run_capture(const char *cmd);

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

/* Called repeatedly while wpe_ai_complete waits, ~8x/second, with the seconds
 * elapsed so far -- the UI uses it to animate a "working" spinner so a slow
 * local model does not look frozen.  May be NULL. */
typedef void (*wpe_ai_progress_cb)(void *ud, int elapsed_s);

/* Collect the FULL assistant reply (for Edit/Plan/Agent, which need the whole
 * answer before acting).  Pumps a progress callback while it waits and aborts if
 * the user presses Esc.  Returns malloc'd text, or NULL (errbuf says why:
 * "cancelled", "timed out", ...). */
char *wpe_ai_complete(const wpe_ai_req *req, int timeout_ms,
                      wpe_ai_progress_cb progress, void *ud,
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
int  e_ai_menu(struct FNST *f);     /* bottom-bar "Alt-B AI" click: action menu */
int  wpe_ai_busy(void);             /* a background AI task (async Edit) is running */
void wpe_ai_cancel(void);           /* tear down the running background task       */
struct CNT;                         /* desktop (ECNT, edit.h)                  */
void e_ai_refresh_bars(struct CNT *cn); /* re-pick editor bars after the toggle */

/* ----- workspace layer (we_ai_ws.c, editor-side) ------------------------ */
/* Scope: the full paths the agent may study -- the open text windows, plus
 * the open .prj members (with_project) and the current file's directory
 * (with_folder, bounded).  Returns count; *out = malloc'd array of malloc'd
 * paths (free with wpe_ai_free_list). */
int   wpe_ai_scope_files(struct FNST *f, int with_project, int with_folder,
                         char ***out);
void  wpe_ai_free_list(char **list, int n);
/* Text of a scope file: the in-memory buffer when it is open (so unsaved
 * edits count), else the file on disk.  Malloc'd, or NULL. */
char *wpe_ai_read_scope_file(struct FNST *f, const char *path);
/* Checkpoint before an edits/auto run: a git ref (stash create / HEAD) when
 * the workspace is a git tree, else snapshot copies of the scope files. */
int   wpe_ai_checkpoint_create(struct FNST *f, char **scope, int nscope);
int   wpe_ai_checkpoint_active(void);
/* After the run: compute what changed on disk, write it to Messages as
 * `path:line: [AI] ...` lines registered for Alt-T / Alt-V, and run the
 * review loop (a = keep all, r = revert all, f = file by file, c = commit). */
int   wpe_ai_changeset_review(struct FNST *f);
/* Sessions tied to the workspace (claudecli session id and/or a transcript). */
void  wpe_ai_session_load(struct FNST *f);
void  wpe_ai_session_save(struct FNST *f);
void  wpe_ai_session_reset(struct FNST *f);
void  wpe_ai_session_append(const char *role, const char *content);
int   wpe_ai_session_messages(wpe_ai_msg *buf, int max);

#endif /* WPE_AI */

#endif /* WE_AI_H */
