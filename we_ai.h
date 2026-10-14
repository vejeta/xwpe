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

/* Which engine runs the Agent (Alt-G g): the built-in tool loop (works with any
   completion backend), or a hosted external agent CLI -- Claude Code -- that runs
   its own tools (only usable in a --enable-ai-agent-host build).  Persisted as
   AIAgentEngine; overridable with XWPE_AI_AGENT_ENGINE=builtin|claude-code. */
enum { WPE_AI_ENGINE_BUILTIN = 0, WPE_AI_ENGINE_CLAUDE_HOST = 1 };
extern int   e_ai_agent_engine;
extern char *e_ai_resume_session;      /* passed as --resume <id>, or NULL     */
extern char *e_ai_last_session_id;     /* session_id from the last CLI reply   */

/* How much of a tool result the model gets: a command's output and a file read
 * with read_file are both cut here, and a cut result ends with a line saying
 * so (wpe_ai_tool_result_mark_cut), so the model asks for less instead of
 * taking the first part for the whole. */
#define WPE_AI_TOOL_OUT_MAX 6000

/* How long a command run for a tool result may take, in seconds, before the
 * editor ends it and tells the model it timed out: the AIToolTimeout key,
 * overridable with XWPE_AI_TOOL_TIMEOUT.  Every command the assistant runs
 * (the agent's run_command, the read tools, the post-edit hook) shares it. */
#define WPE_AI_TOOL_TIMEOUT_DEFAULT 60
extern int e_ai_tool_timeout;
/* The seconds a configuration value names; a value that is not a positive
 * number gives the default, so a mistyped key never removes the bound. */
int   wpe_ai_tool_timeout_from_text(const char *text);

/* Where the project's instructions file (.xwpe-ai.md, else AGENTS.md, in the
 * working directory) is folded into the prompt: the AIProjectInstructions key,
 * overridable with XWPE_AI_PROJECT_INSTRUCTIONS.  agent, the default: the
 * Agent, the Multi-file edit and Build & fix carry it, and the chat carries it
 * from the turn in which it first uses a tool; all: every chat turn as well;
 * off: never.  The file exists for work that acts on the repository; on the
 * current file it is context spent for nothing, a tenth of a local model's
 * window.  Any other name reads as agent. */
enum { WPE_AI_PROJECT_AGENT = 0, WPE_AI_PROJECT_ALL = 1, WPE_AI_PROJECT_OFF = 2 };
extern int   e_ai_project_scope;
int          wpe_ai_project_scope_from_name(const char *name);
const char  *wpe_ai_project_scope_name(int scope);

/* How many bytes of the instructions file the prompt carries: the
 * AIProjectInstructionsMax key, overridable with
 * XWPE_AI_PROJECT_INSTRUCTIONS_MAX.  The default stays small so the file cannot
 * crowd a local model's window; the pane line that reports a cut names the
 * key.  The buffers are allocated from the bound, so the cap only keeps a
 * mistyped value from asking for memory no instructions file needs. */
#define WPE_AI_PROJECT_MAX_DEFAULT 4095
#define WPE_AI_PROJECT_MAX_CAP     65536
extern int   e_ai_project_max;
/* The bytes a configuration value names; a value that is not a positive
 * number gives the default, one above the cap gives the cap. */
int          wpe_ai_project_max_from_text(const char *text);

/* Run a shell command; capture bounded stdout+stderr (malloc'd, never NULL on
 * a spawn failure -- returns a short note instead).  The command runs in a
 * session of its own, so it cannot reach the editor's screen, and is ended
 * after e_ai_tool_timeout seconds. */
char *wpe_ai_run_capture(const char *cmd);
/* The same, also handing back the command's exit status (0 = success, a
 * shell-style 128+signal when it was killed, 127 when it could not start),
 * for a caller that must know whether the command did its job. */
char *wpe_ai_run_capture_status(const char *cmd, int *status);
/* The same with an explicit bound: after timeout_s seconds the command's
 * process group is ended (TERM, then KILL), *timed_out is set and the result
 * is the "TOOL ERROR: timed out after N s" line the model reads.  Both
 * out-parameters may be NULL. */
char *wpe_ai_run_capture_bounded(const char *cmd, int timeout_s,
                                 int *status, int *timed_out);
/* Append the "(output cut at N bytes)" line to a result that was cut at
 * WPE_AI_TOOL_OUT_MAX.  Takes the malloc'd result and its length; returns the
 * (possibly reallocated) result, or the input unchanged when memory is short. */
char *wpe_ai_tool_result_mark_cut(char *result, size_t len);
/* Single-quote one argument for /bin/sh -c, so a path with a space or a
 * quote in it reaches a command as the one argument it is. */
void  wpe_ai_shell_quote(char *dst, size_t sz, const char *src);

/* Put the calling child process in a session of its own, between a fork and
 * the exec that follows it.  Every process the assistant starts goes through
 * this, so none of them can paint over the editor; see we_ai.c. */
int   wpe_ai_child_new_session(void);

/* Effective config (defaults baked in so an EMPTY config file just works).
 * Persisted in the Programming section as AIBackend/AIEndpoint/AIModel and
 * overridable at run time by XWPE_AI_BACKEND / _ENDPOINT / _MODEL. */
extern int   e_ai_backend;    /* one of the enum values; default OLLAMA        */
extern char *e_ai_endpoint;   /* base URL; default http://localhost:11434      */
extern char *e_ai_model;      /* model name; "" => auto-pick first from server */
extern char *e_ai_model_fallback; /* retried once when the primary model fails (or NULL) */
extern char *e_ai_edit_hook;      /* command run on a file after an AI edit (or NULL) */
extern char *e_ai_host_command;   /* agent-host CLI, e.g. "claude"/"aider" (or NULL => claude) */
/* Which host adapter parses the CLI's output: stream-json (Claude Code) or a
   plain "prompt in -> text out" CLI whose stdout is rendered as the answer. */
#define WPE_AI_HOST_ADAPTER_CLAUDE 0
#define WPE_AI_HOST_ADAPTER_TEXT   1
extern int   e_ai_host_adapter;
extern char *e_ai_cafile;     /* extra CA/self-signed cert to trust; NULL=system */
extern char *e_ai_search_url; /* web_search URL template, "%s" = query; NULL=default */

/* Named OpenAI-compatible provider profiles the user can switch between (Groq,
 * OpenRouter, a local bridge, ...).  Each carries its own endpoint, model and CA
 * file; its API key lives in ~/.config/xwpe/openai-api-key-<name> (falling back
 * to the generic key).  e_ai_provider is the active profile name, or NULL for an
 * ad-hoc endpoint typed straight into the dialog. */
struct wpe_ai_provider { char *name, *endpoint, *model, *cafile; };
extern char *e_ai_provider;
/* The longest provider name the configuration carries.  Also the width of the
 * name labels in the provider picker. */
#define WPE_AI_PROVIDER_NAME_MAX 79
/* True when a typed name can be stored: non-empty, within the limit, and
 * without '|' (the field separator of the AIProvider line in xwperc) or '/'
 * (the key file is named openai-api-key-<name>, so '/' would make a path). */
int         wpe_ai_provider_name_usable(const char *name);
extern char *e_ai_key;        /* session OpenAI key from the dialog; NULL=use files */
/* Persist an OpenAI key to ~/.config/xwpe/openai-api-key[-<provider>] (0600). */
int         wpe_ai_write_openai_key(const char *provider, const char *key);
/* Read the stored OpenAI key file for a provider (NULL => generic); NULL if none. */
char       *wpe_ai_read_openai_key_file(const char *provider);
int         wpe_ai_provider_count(void);
const struct wpe_ai_provider *wpe_ai_provider_get(int i);
const struct wpe_ai_provider *wpe_ai_provider_find(const char *name);
void        wpe_ai_provider_set(const char *name, const char *endpoint,
                                const char *model, const char *cafile);

int         wpe_ai_backend_from_name(const char *name);
const char *wpe_ai_backend_name(int backend);
/* Ollama default endpoint when the given URL is an OpenAI-style (path-carrying)
 * base rather than an Ollama one; NULL to keep the current endpoint. */
const char *wpe_ai_ollama_endpoint_fixup(const char *url);

/* True when the assistant is on for this session: XWPE_AI_ENABLE=1 in the
 * environment, else the ED_AI_ENABLE edopt bit on the desktop (the saved
 * toggle).  The effective state, which the Options dialog's Enable box shows. */
int  wpe_ai_enabled(void);

/* Apply built-in defaults + XWPE_AI_* env overrides over whatever the config
 * file loaded.  Call once at startup, after the config file has been read. */
void wpe_ai_config_init(void);

/* ----- what the file carries vs. what the session uses ------------------- */
/* The xwperc keys an XWPE_AI_* variable can shadow for one session.  The
 * file's own value of each is kept apart from the effective e_ai_* global, so
 * Save Options never turns a one-session override into configuration: the
 * writer gets the file's value for a key the environment still shadows, and
 * the effective value once the user has changed that key in the dialog. */
enum wpe_ai_cfg_key {
  WPE_AI_CFG_BACKEND = 0,     /* AIBackend     <- XWPE_AI_BACKEND      (int)    */
  WPE_AI_CFG_POLICY,          /* AIPolicy      <- XWPE_AI_POLICY       (int)    */
  WPE_AI_CFG_ENDPOINT,        /* AIEndpoint    <- XWPE_AI_ENDPOINT     (string) */
  WPE_AI_CFG_MODEL,           /* AIModel       <- XWPE_AI_MODEL        (string) */
  WPE_AI_CFG_CAFILE,          /* AICAFile      <- XWPE_AI_CAFILE       (string) */
  WPE_AI_CFG_AGENT_ENGINE,    /* AIAgentEngine <- XWPE_AI_AGENT_ENGINE (int)    */
  WPE_AI_CFG_HOST_ADAPTER,    /* AIHostAdapter <- XWPE_AI_HOST_ADAPTER (int)    */
  WPE_AI_CFG_SEARCH_URL,      /* AISearchURL   <- XWPE_AI_SEARCH_URL   (string) */
  WPE_AI_CFG_TOOL_TIMEOUT,    /* AIToolTimeout <- XWPE_AI_TOOL_TIMEOUT (int)    */
  WPE_AI_CFG_PROJECT_SCOPE,   /* AIProjectInstructions <- XWPE_AI_PROJECT_INSTRUCTIONS (int) */
  WPE_AI_CFG_PROJECT_MAX,     /* AIProjectInstructionsMax <- XWPE_AI_PROJECT_INSTRUCTIONS_MAX (int) */
  WPE_AI_CFG_KEYS
};
/* The value Save Options writes for a key (int keys / string keys; a string
 * key answers "" when it has no value). */
int         wpe_ai_cfg_int_to_save(enum wpe_ai_cfg_key key);
const char *wpe_ai_cfg_str_to_save(enum wpe_ai_cfg_key key);
/* The user changed this key in the dialog: from now on its effective value is
 * what the file should carry, environment override or not. */
void        wpe_ai_cfg_user_changed(enum wpe_ai_cfg_key key);

/* True when the model name is one the Claude backends accept (a CLI alias or a
 * claude-* id); false for a model from another backend.  wpe_ai_normalize_model
 * drops a model incompatible with the active backend (Claude only -- Ollama /
 * OpenAI take any name), resetting it to the backend default. */
int  wpe_ai_model_is_claude(const char *m);
void wpe_ai_normalize_model(void);

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

/* GET an arbitrary http/https URL, returning its body (bounded to maxbytes,
 * caller frees *out).  Backs the built-in agent's web_fetch tool.  0 on
 * success, -1 on failure with a reason in errbuf. */
int  wpe_ai_web_fetch(const char *url, size_t maxbytes, char **out,
                      char *errbuf, size_t errsz);

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
/* Human message if the finished stream carried an HTTP error; NULL when OK. */
char *wpe_ai_stream_error_message(wpe_ai_stream *st);
/* True when a completed stream reported a backend failure rather than a reply
 * (e.g. the claude CLI is not logged in); callers that write files or run tools
 * must refuse to act on it. */
int   wpe_ai_stream_had_error(wpe_ai_stream *st);
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

/* Split text into its lines, the '\n' removed; a last line without '\n' still
 * counts.  *lines is a malloc'd array of malloc'd strings, the same shape as a
 * segment's a[]/b[] below (free with wpe_ai_free_lines).  Returns the count. */
int  wpe_ai_split_lines(const char *t, char ***lines);
void wpe_ai_free_lines(char **lines, int n);

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
/* The hosted agent engine (Alt-G g on an external CLI) is streaming a turn.
 * Esc then asks the engine to stop that turn and keeps the session alive, the
 * same key the built-in loop cancels with; both are 0 / a no-op in a build
 * without the agent host. */
int  wpe_ai_host_turn_active(void);
void wpe_ai_host_interrupt(void);
/* True while an Edit or a Complete started from `f` is waiting for its reply.
 * The reply is spliced over the lines that were sent, at the line numbers
 * captured when the request left, so the editor refuses the keys that would
 * move them until the task ends. */
int  wpe_ai_window_locked(struct FNST *f);
/* Say on the status row why the window will not take the key, and that Esc
 * cancels the task that holds it. */
void wpe_ai_report_window_locked(struct FNST *f);
/* e_close_window calls this before freeing a window: every pending AI task,
 * stream or queued request that targets it is dropped so nothing paints or
 * applies through the freed window. */
void wpe_ai_window_closing(struct FNST *closing);
/* The editor key loop calls this just before it waits for a key: a reply that
 * finished while a dialog, prompt, picker or the file manager was open is held
 * until then, and this runs its review or apply now, before the next key. */
void wpe_ai_deliver_held_reply(void);
/* One-shot status-line message (Turbo Vision style): takes the bottom bar row;
 * the next keystroke restores the key hints.  wpe_ai_flash_clear returns 1 if it
 * cleared a flash (called from the editor key loop on every key). */
void wpe_ai_flash(struct FNST *f, const char *msg);
int  wpe_ai_flash_clear(struct FNST *f);
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
/* A prompt block naming the files currently open in the editor (working set),
 * the focused one marked.  Writes into buf and returns it ("" if none). */
char *wpe_ai_open_windows_block(struct FNST *f, char *buf, size_t n);
/* The one question Edit, Complete and Multi-file ask before anything is sent:
 * is this a real file window whose text the user may change (not the AI pane,
 * not Messages, not a read-only viewer or tool pane)?  1 when it is. */
int   wpe_ai_window_is_editable_file(struct FNST *w);
/* Text of a scope file: the in-memory buffer when it is open (so unsaved
 * edits count), else the file on disk.  Malloc'd, or NULL. */
char *wpe_ai_read_scope_file(struct FNST *f, const char *path);
int   wpe_ai_path_outside_workspace(struct FNST *f, const char *path);
/* Checkpoint before an edits/auto run: a git ref (stash create / HEAD) when
 * the workspace is a git tree, else snapshot copies of the scope files. */
int   wpe_ai_checkpoint_create(struct FNST *f, char **scope, int nscope);
int   wpe_ai_checkpoint_active(void);
/* After the run: compute what changed on disk, write it to Messages as
 * `path:line: [AI] ...` lines registered for Alt-T / Alt-V, and run the
 * review loop (a = keep all, r = revert all, f = file by file, c = commit). */
int   wpe_ai_changeset_review(struct FNST *f);
/* Reload an open window from disk after a tool wrote its file, so the agent's
 * change is visible immediately, as one undo step.  Returns 1 when a window
 * showed the file, 0 when the path is not open (nothing happens). */
int   wpe_ai_reload_open_window(struct FNST *f, const char *path);
/* Refresh an open window from disk WITHOUT a new undo step: for the post-edit
 * hook's rewrite, which belongs to the undo step the write already took, so
 * one Ctrl-U reverts write and reformat together.  Returns 1 when refreshed. */
int   wpe_ai_refresh_open_window(struct FNST *f, const char *path);
/* The window's buffer written to a temporary copy beside its file (same
 * directory and extension), for a hook that must not touch the file itself
 * before the user saves.  Fills `tmp` with the copy's path; 1 on success. */
int   wpe_ai_window_hook_copy(struct FNST *w, char *tmp, size_t sz);
/* Replace the window's buffer with a file's text, without a new undo step. */
int   wpe_ai_window_take_file(struct FNST *w, const char *path);
/* Sessions tied to the workspace (claudecli session id and/or a transcript). */
void  wpe_ai_session_load(struct FNST *f);
void  wpe_ai_session_save(struct FNST *f);
void  wpe_ai_session_reset(struct FNST *f);
void  wpe_ai_session_append(const char *role, const char *content);
int   wpe_ai_session_messages(wpe_ai_msg *buf, int max);
/* Whether the pane has named this session's project instructions (.xwpe-ai.md,
   else AGENTS.md) yet; the chat says it once per session.  Cleared with the
   session. */
int   wpe_ai_session_project_announced(void);
void  wpe_ai_session_mark_project_announced(void);
/* Whether a chat turn of this session has answered with a tool line yet: from
   that turn on the chat acts on the repository and, under the default scope,
   carries the project instructions.  Cleared with the session. */
int   wpe_ai_session_tool_turn_opened(void);
void  wpe_ai_session_mark_tool_turn(void);

#endif /* WPE_AI */

#endif /* WE_AI_H */
