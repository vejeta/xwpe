/* we_lsp.h -- Language Server Protocol client engine.
 *
 * Companion to the DAP client (we_dap.c): where DAP makes xwpe a *debugger*,
 * LSP makes it an *IDE* -- diagnostics as you type, go-to-definition, hover
 * (type/docs) and completion, from the same language servers that power VS
 * Code / Neovim / Emacs (Metals for Scala, clangd for C/C++, ...).
 *
 * LSP is JSON-RPC 2.0 over the SAME Content-Length framing as DAP/BSP, so this
 * layer reuses we_dap_proto.c's streaming reader for the wire.  Like the DAP
 * engine it is free of editor globals (the host supplies a callback for the one
 * asynchronous side-effect, diagnostics) so it can be integration-tested
 * against a real server without running xwpe.  The editor bridge (completion
 * popup, hover box, diagnostic marks, definition jump) lives elsewhere.
 *
 * Coordinates are LSP's: 0-based line AND character.  The editor bridge
 * converts to/from xwpe's 1-based lines.
 */
#ifndef WE_LSP_H
#define WE_LSP_H

#include <stddef.h>
#include "we_debreq.h"   /* E_D_REQ_IO, E_D_REQ_END, E_D_SEND_*: the requests' world */

struct json_object;

/* Severity as in LSP: 1 error, 2 warning, 3 information, 4 hint.
 * (line,character) is the diagnostic's range START and (end_line,end_character)
 * its END (both 0-based) so the editor can underline/recolor the exact span. */
typedef struct {
 void (*on_diagnostic)(const char *path, int line, int character,
                       int end_line, int end_character,
                       int severity, const char *message, void *ud);
 /* Called once per publishDiagnostics batch with the error/warning totals --
    for a non-spammy live "N errors, M warnings" status as you type. */
 void (*on_diagnostics_summary)(const char *path, int errors, int warnings,
                                void *ud);
 /* A document the server wants displayed (the Metals Doctor): a title and a
    plain-text body.  Lets the client show it in a window instead of letting the
    server open an external browser.  May be NULL. */
 void (*on_show_text)(const char *title, const char *body, void *ud);
 /* The server's transient status (metals/status): @text is what it is doing
    right now ("Indexing", "Importing build", ...) when @hide is 0, or @hide is
    1 when it has finished and the status should clear.  Lets the client show
    progress and tell "still indexing" apart from "no result".  May be NULL. */
 void (*on_status)(const char *text, int hide, void *ud);
 /* The server asks the client to re-query inlay hints (workspace/inlayHint/
    refresh) -- Metals sends this when re-indexing changes the inferred types,
    so the overlay can update from a stale snapshot to the full set.  May be
    NULL.  Fired from inside e_lsp_poll(), so the handler only sets a flag,
    and the fetch goes out once the read has returned. */
 void (*on_inlay_refresh)(void *ud);
 /* The server asks the client to re-query semantic tokens
    (workspace/semanticTokens/refresh) -- e.g. indexing resolved a cross-file
    type, so the highlighting can update.  Same contract as on_inlay_refresh
    (set a flag, refetch at a safe point).  May be NULL. */
 void (*on_semantic_refresh)(void *ud);
 void *ud;
 /* 1 when the server may start a build server other editors share (Metals
    and Bloop, which stays in the session of whoever started it): ending the
    server then signals the server alone, never its group, so the shared
    build server goes on.  0 for the others: the server and every helper it
    started are ended together. */
 int server_shares_daemon;
} e_lsp_host;

typedef struct e_lsp_session e_lsp_session;

/* One completion candidate (engine-owned strings, valid until the next
 * e_lsp_completion call or e_lsp_close). */
typedef struct {
 char *label;     /* what to show / insert (e.g. "println(x: Any): Unit") */
 char *insert;    /* text to insert if different from label, else NULL    */
 int   kind;      /* LSP CompletionItemKind (3 = function, 6 = variable…) */
} e_lsp_completion_item;

/* A source location (engine-owned path, valid until the next call/close). */
typedef struct {
 char *path;      /* filesystem path of the location's file */
 int   line;      /* 0-based                                */
 int   character; /* 0-based                                */
} e_lsp_location;

/* A text range [start, end), both ends 0-based (line, character) as in LSP. */
typedef struct {
 int start_line, start_char;
 int end_line, end_char;
} e_lsp_range;

/* One semantic token: the half-open span [start, start+length) on `line` (all
 * 0-based), whose meaning is the legend index `type` (resolve to a name like
 * "keyword"/"class"/"method" with e_lsp_semantic_legend).  `modifiers` is the
 * raw modifier bitset (unused for colouring so far). */
typedef struct {
 int line;
 int start;
 int length;
 int type;
 int modifiers;
} e_lsp_sem_token;

/* A symbol for the outline / workspace search (engine-owned strings). */
typedef struct {
 char *name;      /* symbol name (object/def/val/...)        */
 char *path;      /* file path (NULL = the current document) */
 int   line;      /* 0-based line of the symbol              */
 int   character; /* 0-based                                 */
 int   kind;      /* LSP SymbolKind                          */
} e_lsp_symbol;

/* One code action / quick-fix candidate (engine-owned). */
typedef struct {
 char *title;     /* what to show ("Organize imports", ...)         */
 int   has_edit;  /* 1 if a direct WorkspaceEdit is attached         */
} e_lsp_code_action;

/* One code lens (engine-owned): a "run | debug" / "test" / "N references"
 * annotation the server attaches above a definition. */
typedef struct {
 char *title;     /* the lens label ("run | debug", "test", ...)     */
 int   line;      /* 0-based line the lens annotates                 */
} e_lsp_code_lens;

/* One inlay hint (engine-owned): virtual text the server suggests showing
 * inline -- an inferred TYPE (kind 1, e.g. ": Int") or a PARAMETER name
 * (kind 2, e.g. "name = ") -- anchored between two real characters. */
typedef struct {
 char *label;     /* the hint text, padding (paddingLeft/Right) folded in */
 int   line;      /* 0-based line of the anchor position             */
 int   character; /* 0-based column of the anchor position           */
 int   kind;      /* 1 = Type, 2 = Parameter, 0 = unspecified        */
} e_lsp_inlay_hint;

/* Spawn the language server (argv NULL-terminated, e.g. {"metals",0}), run the
 * initialize/initialized handshake with headless InitializationOptions, cwd =
 * root_dir.  `lang` is the LSP languageId ("scala", "c", …) used on didOpen.
 * Returns a session or NULL.  Waits for the handshake: we_lsp_sync.c, the C
 * tests only. */
e_lsp_session *e_lsp_open(char *const argv[], const char *root_dir,
                          const char *lang, const e_lsp_host *host);

/* Like e_lsp_open but ASYNCHRONOUS: spawns the server and sends `initialize`,
 * then returns immediately with the session "starting" (the slow JVM boot does
 * NOT block the caller).  Drive e_lsp_poll() from an fd-loop on e_lsp_fd(s)
 * until e_lsp_started(s) is true; e_lsp_poll finishes the handshake.  Then
 * the caller sends didOpen.  `initialize` is a
 * request like the others: it gets the waiting line once e_lsp_requests_init
 * gave the session a clock, and e_lsp_close ends a start the user gave up.
 * Returns NULL only on spawn failure. */
e_lsp_session *e_lsp_open_async(char *const argv[], const char *root_dir,
                                const char *lang, const e_lsp_host *host);

/* Asked, when set, before a server process starts: @argv its command line,
 * @cwd the project it runs in.  It returns the start plan the user answered
 * for (we_startplan.h), whose words the server is started with, or NULL,
 * and then nothing starts.  The editor sets it (a server that runs the
 * folder's build code asks once per folder); a test may leave it unset, and
 * the command then runs as given, from a plan of its fixed words. */
struct WpeStartPlan;
typedef struct WpeStartPlan *(*e_lsp_plan_fn)(char *const argv[], const char *cwd);
void e_lsp_set_planner(e_lsp_plan_fn fn);

/* The server's stdout fd (register it with the editor's fd-loop), or -1. */
int e_lsp_fd(e_lsp_session *s);

/* 1 once the initialize handshake has completed (server ready), else 0. */
int e_lsp_started(e_lsp_session *s);

/* 1 if the server has closed its stdout (it exited/crashed) -- the caller must
 * stop polling its fd and tear the session down.  Also 1 for a NULL session. */
int e_lsp_dead(e_lsp_session *s);
int e_lsp_input_lost(e_lsp_session *s);

/* textDocument/didOpen: hand the server the buffer text so it compiles/indexes.
 * `path` is a filesystem path (turned into a file:// URI internally). */
int e_lsp_did_open(e_lsp_session *s, const char *path, const char *text);

/* metals/didFocusTextDocument: tell Metals which file is in focus so it warms
 * the presentation compiler for it.  Hover and completion are PC-driven and
 * come back empty until the PC is active for the file -- which (for Metals)
 * only happens for the focused document.  Harmless no-op for non-Metals servers. */
int e_lsp_did_focus(e_lsp_session *s, const char *path);

/* textDocument/didChange: replace the server's copy of the document with the
 * current buffer text (full-document sync), bumping the version.  Call this
 * before a request so hover/definition/completion/diagnostics reflect unsaved
 * edits rather than the on-disk file. */
int e_lsp_did_change(e_lsp_session *s, const char *path, const char *text);

/* Run the loop until the server publishes diagnostics for `path` (== it has compiled),
 * delivering every diagnostic through host->on_diagnostic.  Returns 1 if the
 * file's diagnostics arrived, 0 on timeout/EOF.  Waits: we_lsp_sync.c, the C
 * tests only. */
int e_lsp_wait_diagnostics(e_lsp_session *s, const char *path, int timeout_ms);

/* Non-blocking: dispatch any messages already waiting from the server
 * (diagnostics -> host, server requests answered) and return immediately.
 * Call it on each keystroke so live diagnostics surface without blocking
 * typing.  Returns how many diagnostics batches were read: the server's news
 * that its view of a file changed.  Answers, log lines, progress and status
 * are not counted (they come with the editor's own requests), so an overlay
 * that refetches on news does not refetch on its own answer. */
int e_lsp_poll(e_lsp_session *s);

/* ---- requests answered on the loop -------------------------------------
 *
 * A request goes out at once and its answer is handed to a continuation
 * later, from e_lsp_poll(), so the editor keeps its keys while a server
 * indexes.  A response is matched to its request by id; diagnostics,
 * status and the server's own requests (workspace/applyEdit, ...) read
 * meanwhile are handled as they come.
 *
 * e_lsp_requests_init() gives the session the host's clock and one-shot
 * timers (the line engine's E_D_REQ_IO: now_ms, arm_timer, waiting,
 * notice, name, io; the rest is unused) and the user's deadline override
 * (XWPE_DEBUG_ANSWER_TIMEOUT's text, or NULL).  The oldest request out then
 * gets the waiting line after E_D_REQ_QUIET_MS, and is given up at the
 * deadline with the notice; `initialize` gets the waiting line and no
 * deadline (a server may import a build for minutes; the user ends it).
 * The host calls e_lsp_request_timer() when a timer it asked for goes off.
 * Without it there is no waiting line and no deadline (the C tests).
 *
 * The inlay hint and semantic token fetches are background requests: no
 * waiting line, no deadline, untouched by e_lsp_requests_give_up() (Esc),
 * and a newer fetch of the same kind tells the older one E_D_REQ_CANCELLED
 * and takes its place, so a stale overlay answer is never handed over.
 *
 * e_lsp_request() sends @method with @params (taken over, may be NULL) and
 * calls @fn exactly once: E_D_REQ_DONE with the response message (borrowed
 * for the call), or E_D_REQ_TIMED_OUT, E_D_REQ_CANCELLED or E_D_REQ_GONE
 * with NULL.  Returns E_D_SEND_OK, or why it was not sent (then @fn is
 * never called).  A response to a request given up on is dropped.  @fn may
 * send the next request; it must not close the session.
 *
 * The typed requests below run the same parsers the synchronous calls did,
 * in the continuation, and hand the caller the parsed value.  A request
 * that needs more than one round trip (the hierarchies, code lenses and
 * their resolve, a code action's resolve and command) runs its steps from
 * its continuations and tells the caller once, at the end.  An array handed
 * to a continuation is the engine's and valid for that call only: keep a
 * copy of what is needed later.  n = -1 means the request ended without an
 * answer (@how says why). */
typedef void (*e_lsp_answered_fn)(E_D_REQ_END how, struct json_object *resp,
                                  void *ctx);
/* A text answer (hover): @text is malloc'd and the callee's to free, or
   NULL when there is none or the request ended without an answer. */
typedef void (*e_lsp_text_fn)(E_D_REQ_END how, char *text, void *ctx);
/* A location answer: @found is 1 with @path (borrowed), @line, @character
   (0-based) set, else 0. */
typedef void (*e_lsp_location_fn)(E_D_REQ_END how, int found, const char *path,
                                  int line, int character, void *ctx);
/* A completion answer: @items (engine-owned, valid until the next completion
   or the close), @n of them, or n = -1 when the request ended unanswered. */
typedef void (*e_lsp_items_fn)(E_D_REQ_END how,
                               const e_lsp_completion_item *items, int n,
                               void *ctx);
/* An edit answer (format): @newtext is the text sent with the request with
   the server's edits applied (malloc'd, the callee's), or NULL for none. */
typedef void (*e_lsp_edit_fn)(E_D_REQ_END how, char *newtext, void *ctx);
/* Locations (references, highlight): @n of them. */
typedef void (*e_lsp_locations_fn)(E_D_REQ_END how, const e_lsp_location *locs,
                                   int n, void *ctx);
/* Symbols (outline, workspace search, call and type hierarchy). */
typedef void (*e_lsp_symbols_fn)(E_D_REQ_END how, const e_lsp_symbol *syms,
                                 int n, void *ctx);
/* Ranges (selection range), innermost first. */
typedef void (*e_lsp_ranges_fn)(E_D_REQ_END how, const e_lsp_range *ranges,
                                int n, void *ctx);
/* Code lenses, each resolved where the server needed it. */
typedef void (*e_lsp_lenses_fn)(E_D_REQ_END how, const e_lsp_code_lens *lenses,
                                int n, void *ctx);
/* Inlay hints. */
typedef void (*e_lsp_inlays_fn)(E_D_REQ_END how, const e_lsp_inlay_hint *hints,
                                int n, void *ctx);
/* Semantic tokens, decoded. */
typedef void (*e_lsp_tokens_fn)(E_D_REQ_END how, const e_lsp_sem_token *tokens,
                                int n, void *ctx);
/* Code actions offered at a position. */
typedef void (*e_lsp_actions_fn)(E_D_REQ_END how, const e_lsp_code_action *acts,
                                 int n, void *ctx);
/* A workspace edit applied to the text sent (rename, a code action):
   @newtext malloc'd and the callee's, or NULL when the file is untouched
   (or nothing came); @others counts the other files it would change. */
typedef void (*e_lsp_applied_fn)(E_D_REQ_END how, char *newtext, int others,
                                 void *ctx);

/* The three "jump to a location" requests. */
#define E_LSP_LOCATE_DEFINITION       0
#define E_LSP_LOCATE_IMPLEMENTATION   1
#define E_LSP_LOCATE_TYPE_DEFINITION  2

void e_lsp_requests_init(e_lsp_session *s, const E_D_REQ_IO *io,
                         const char *timeout_text);
int  e_lsp_request(e_lsp_session *s, const char *method,
                   struct json_object *params, e_lsp_answered_fn fn, void *ctx);
void e_lsp_request_timer(e_lsp_session *s, int which);
/* Every request out, background ones included, is told @why (the close,
   a test's own deadline). */
void e_lsp_requests_cancel(e_lsp_session *s, E_D_REQ_END why);
/* Every request the user waits for is told @why (Esc); the background
   fetches go on. */
void e_lsp_requests_give_up(e_lsp_session *s, E_D_REQ_END why);
/* Is a request (or the start) waiting for its answer? */
int  e_lsp_requests_pending(const e_lsp_session *s);
/* Is any answer owed, a background fetch's included? */
int  e_lsp_requests_owed(const e_lsp_session *s);
/* The method of the oldest request out and the seconds it has waited, for a
   refusal that names it; NULL when none is out. */
const char *e_lsp_request_waited(const e_lsp_session *s, long *secs);

int e_lsp_hover_async(e_lsp_session *s, const char *path, int line,
                      int character, e_lsp_text_fn fn, void *ctx);
int e_lsp_locate_async(e_lsp_session *s, int which, const char *path,
                       int line, int character, e_lsp_location_fn fn,
                       void *ctx);
int e_lsp_completion_async(e_lsp_session *s, const char *path, int line,
                           int character, int max, e_lsp_items_fn fn,
                           void *ctx);
/* Whole file when @range is NULL; else the inclusive range it holds. */
int e_lsp_format_async(e_lsp_session *s, const char *path,
                       const char *current_text, const e_lsp_range *range,
                       e_lsp_edit_fn fn, void *ctx);
/* The active signature's label at a call (malloc'd text, or NULL). */
int e_lsp_signature_help_async(e_lsp_session *s, const char *path, int line,
                               int character, e_lsp_text_fn fn, void *ctx);
/* Every use of the symbol, its declaration included; at most @max. */
int e_lsp_references_async(e_lsp_session *s, const char *path, int line,
                           int character, int max, e_lsp_locations_fn fn,
                           void *ctx);
/* Every occurrence of the symbol in this file; at most @max. */
int e_lsp_document_highlight_async(e_lsp_session *s, const char *path, int line,
                                   int character, int max,
                                   e_lsp_locations_fn fn, void *ctx);
/* Who calls the symbol (@outgoing 0) or what it calls (1); n = 0 when the
   cursor is not on a callable. */
int e_lsp_call_hierarchy_async(e_lsp_session *s, const char *path, int line,
                               int character, int outgoing, int max,
                               e_lsp_symbols_fn fn, void *ctx);
/* What the type extends (@subtypes 0) or what extends it (1); n = 0 when
   the cursor is not on a type. */
int e_lsp_type_hierarchy_async(e_lsp_session *s, const char *path, int line,
                               int character, int subtypes, int max,
                               e_lsp_symbols_fn fn, void *ctx);
/* The nested "smart selection" ranges around the position. */
int e_lsp_selection_range_async(e_lsp_session *s, const char *path, int line,
                                int character, int max, e_lsp_ranges_fn fn,
                                void *ctx);
/* The file's code lenses, each resolved where it needs it. */
int e_lsp_code_lenses_async(e_lsp_session *s, const char *path, int max,
                            e_lsp_lenses_fn fn, void *ctx);
/* Background: the inlay hints for lines [start_line, end_line]. */
int e_lsp_inlay_hints_async(e_lsp_session *s, const char *path,
                            int start_line, int end_line, int max,
                            e_lsp_inlays_fn fn, void *ctx);
/* Background: the whole file's semantic tokens, decoded; at most @max. */
int e_lsp_semantic_tokens_async(e_lsp_session *s, const char *path, int max,
                                e_lsp_tokens_fn fn, void *ctx);
/* The file's outline, flattened depth-first. */
int e_lsp_document_symbols_async(e_lsp_session *s, const char *path, int max,
                                 e_lsp_symbols_fn fn, void *ctx);
/* Every symbol in the project matching @query. */
int e_lsp_workspace_symbols_async(e_lsp_session *s, const char *query, int max,
                                  e_lsp_symbols_fn fn, void *ctx);
/* The quick-fixes and refactors offered at the position, with the
   diagnostics covering it sent along.  The list is kept for
   e_lsp_code_action_run_async until the next code action request. */
int e_lsp_code_actions_async(e_lsp_session *s, const char *path, int line,
                             int character, int max, e_lsp_actions_fn fn,
                             void *ctx);
/* Run code action @index of the last list on @current_text: an edit in
   hand is applied at once (@fn told before this returns); an action that
   carries only data is resolved first; a command runs as
   workspace/executeCommand, and the edit the server sends back with
   workspace/applyEdit is applied to the text sent. */
int e_lsp_code_action_run_async(e_lsp_session *s, int index, const char *path,
                                const char *current_text, e_lsp_applied_fn fn,
                                void *ctx);
/* Rename the symbol to @new_name: the edits for this file applied to
   @current_text. */
int e_lsp_rename_async(e_lsp_session *s, const char *path, int line,
                       int character, const char *new_name,
                       const char *current_text, e_lsp_applied_fn fn,
                       void *ctx);

/* Count the diagnostics batches published for @path from now on (the C
   tests' "has it compiled?"): e_lsp_diagnostics_seen() says how many. */
void e_lsp_diagnostics_watch(e_lsp_session *s, const char *path);
int  e_lsp_diagnostics_seen(const e_lsp_session *s);

/* ---- the synchronous calls (we_lsp_sync.c, linked into the C tests only;
 * each waits for its answer, so the editor never calls them) ------------- */

/* textDocument/hover at (line,character): returns a malloc'd plain-text string
 * (markdown fences stripped) the caller frees, or NULL. */
char *e_lsp_hover(e_lsp_session *s, const char *path, int line, int character);

/* textDocument/signatureHelp at (line,character): the active overload's
 * signature label (e.g. "def println(x: Any): Unit").  malloc'd, caller frees,
 * or NULL.  Most useful with the cursor inside a call's parentheses. */
char *e_lsp_signature_help(e_lsp_session *s, const char *path,
                           int line, int character);

/* textDocument/definition (and the implementation / type-definition variants):
 * on success fill out_path/out_line/out_char with the first location and return
 * 0; return -1 if there is none. */
int e_lsp_definition(e_lsp_session *s, const char *path, int line, int character,
                     char *out_path, size_t out_sz, int *out_line, int *out_char);
int e_lsp_implementation(e_lsp_session *s, const char *path, int line, int character,
                         char *out_path, size_t out_sz, int *out_line, int *out_char);
int e_lsp_type_definition(e_lsp_session *s, const char *path, int line, int character,
                          char *out_path, size_t out_sz, int *out_line, int *out_char);

/* workspace/symbol: search every symbol in the project matching `query`.  Fills
 * up to `max` symbols (engine-owned name+path); returns the count (>=0) or -1. */
int e_lsp_workspace_symbols(e_lsp_session *s, const char *query,
                            e_lsp_symbol *syms, int max);

/* textDocument/codeAction at (line,character): quick-fixes / refactors offered
 * there (organize imports, import missing, ...).  Fills up to `max` actions
 * (engine-owned titles); returns the count (>=0) or -1. */
int e_lsp_code_actions(e_lsp_session *s, const char *path, int line, int character,
                       e_lsp_code_action *acts, int max);

/* Run the code action chosen by index from the LAST e_lsp_code_actions call
 * on `current_text` (e_lsp_code_action_run_async, waited for) and return the
 * new full text (malloc'd), or NULL when the file is untouched.
 * *other_files counts other files it would touch. */
char *e_lsp_apply_code_action(e_lsp_session *s, int index, const char *path,
                              const char *current_text, int *other_files);

/* textDocument/completion: fills up to `max` items and returns the count (>=0),
 * or -1 on error.  Items point at engine-owned memory (see e_lsp_completion_item). */
int e_lsp_completion(e_lsp_session *s, const char *path, int line, int character,
                     e_lsp_completion_item *items, int max);

/* textDocument/references: every use of the symbol at (line,character),
 * including its declaration.  Fills up to `max` locations (engine-owned paths);
 * returns the count (>=0) or -1. */
int e_lsp_references(e_lsp_session *s, const char *path, int line, int character,
                     e_lsp_location *locs, int max);

/* Call hierarchy for the symbol at (line,character): prepareCallHierarchy to
 * pin the symbol, then its incoming calls (outgoing == 0: who calls it) or its
 * outgoing calls (outgoing != 0: what it calls).  Fills up to `max` symbols
 * (engine-owned name + the caller/callee's own location) in `out`; returns the
 * count (>=0) or -1.  0 means the cursor was not on a callable, or none. */
int e_lsp_call_hierarchy(e_lsp_session *s, const char *path, int line,
                         int character, int outgoing,
                         e_lsp_symbol *out, int max);

/* Type hierarchy for the type at (line,character): prepareTypeHierarchy to pin
 * the type, then its supertypes (subtypes == 0: what it extends/implements) or
 * its subtypes (subtypes != 0: what extends/implements it).  Fills up to `max`
 * symbols (engine-owned name + each type's own location) in `out`; returns the
 * count (>=0) or -1.  0 means the cursor was not on a type, or none. */
int e_lsp_type_hierarchy(e_lsp_session *s, const char *path, int line,
                         int character, int subtypes,
                         e_lsp_symbol *out, int max);

/* textDocument/selectionRange at (line,character): the server's nested
 * "smart selection" ranges, flattened innermost-first into `out` (out[0] is the
 * tightest range around the cursor, each following one strictly encloses the
 * previous).  Fills up to `max`; returns the count (>=0) or -1.  Drives an
 * editor "expand selection" that grows by syntactic structure, not by lines. */
int e_lsp_selection_range(e_lsp_session *s, const char *path, int line,
                          int character, e_lsp_range *out, int max);

/* textDocument/documentHighlight: every occurrence of the symbol under the
 * cursor IN THIS FILE (read/write/text).  Fills up to `max` start locations
 * (engine-owned, all the current file); returns the count (>=0) or -1.  Lighter
 * than references -- single file, for "highlight all uses" as you rest on a name. */
int e_lsp_document_highlight(e_lsp_session *s, const char *path, int line,
                             int character, e_lsp_location *locs, int max);

/* textDocument/codeLens (resolving each lens that needs it): the run/test/
 * reference annotations the server attaches to definitions in `path`.  Fills up
 * to `max` lenses (engine-owned titles + the line each annotates); returns the
 * count (>=0) or -1.  Discovery only -- running a lens reuses xwpe's existing
 * Scala BSP/DAP path (see Debugging). */
int e_lsp_code_lenses(e_lsp_session *s, const char *path,
                      e_lsp_code_lens *lenses, int max);

/* textDocument/inlayHint: the inferred-type / parameter-name hints the server
 * would show inline, for the inclusive line range [start_line, end_line].  Fills
 * up to `max` hints (engine-owned labels) in `out`; returns the count, or -1. */
int e_lsp_inlay_hints(e_lsp_session *s, const char *path,
                      int start_line, int end_line,
                      e_lsp_inlay_hint *out, int max);

/* textDocument/documentSymbol: the file's outline (objects, defs, vals, ...),
 * flattened depth-first.  Fills up to `max` symbols (engine-owned names);
 * returns the count (>=0) or -1. */
int e_lsp_document_symbols(e_lsp_session *s, const char *path,
                           e_lsp_symbol *syms, int max);

/* textDocument/formatting: ask the server (scalafmt for Scala) to reformat the
 * document.  `current_text` is the buffer's current full text; the server's edits
 * are applied to it and the new full text is returned (malloc'd, caller frees),
 * or NULL if there is nothing to do / on error. */
char *e_lsp_format(e_lsp_session *s, const char *path, const char *current_text);

/* textDocument/rangeFormatting: like e_lsp_format but only the inclusive range
 * [start_line,start_char .. end_line,end_char] (0-based) is reformatted -- the
 * rest of `current_text` is left byte-for-byte.  Returns the new full text
 * (malloc'd, caller frees), or NULL if there is nothing to do / on error. */
char *e_lsp_format_range(e_lsp_session *s, const char *path,
                         const char *current_text,
                         int start_line, int start_char,
                         int end_line, int end_char);

/* textDocument/rename: rename the symbol at (line,character) to `new_name`.
 * Applies the resulting edits for THIS file to `current_text` and returns the new
 * full text (malloc'd), or NULL.  *other_files, if non-NULL, is set to the number
 * of OTHER files the rename also touches (not applied here -- the caller warns). */
char *e_lsp_rename(e_lsp_session *s, const char *path, int line, int character,
                   const char *new_name, const char *current_text,
                   int *other_files);

/* Ask the server to end (`shutdown`, `exit`) and free the session at once,
 * without waiting: every request still out is told E_D_REQ_GONE, and the
 * owner of the editor's children ends the server on its ladder (a grace,
 * then SIGKILL; SIGTERM first when it was never asked on its pipe). */
void e_lsp_close(e_lsp_session *s);

/* Join editor lines into the document body for the server, emitting exactly one
 * '\n' per line.  Each lines[i] ends at its first newline (the editor's WPE_WR
 * terminator) or NUL; a NULL element is an empty line.  Pure -- no session, no
 * I/O -- so the serialization invariant is unit-testable.  Returns a malloc'd
 * string (caller frees) or NULL on OOM. */
char *e_lsp_join_lines(char *const *lines, int n);

/* Dedup gate for a pushed display document (the Metals Doctor, which the server
 * re-pushes on every build event): return 1 and adopt @body when it differs
 * from the last one shown, 0 when identical or NULL.  *last is the caller's
 * stored copy (pass &static_ptr, NULL-initialized; this owns it).  Pure --
 * unit-testable. */
int e_lsp_doc_is_new(char **last, const char *body);

/* Decode an LSP semantic-tokens `data` array (@n ints, groups of 5:
 * deltaLine, deltaStartChar, length, tokenType, tokenModifiers) into absolute
 * tokens.  Fills up to @max into @out and returns the count.  PURE -- no
 * session, no I/O -- so the (fiddly) delta decoding is unit-testable. */
int e_lsp_semantic_decode(const int *data, int n, e_lsp_sem_token *out, int max);

/* textDocument/semanticTokens/full: the server's token classification for the
 * whole file.  Fills up to @max tokens in @out (each `type` is a legend index;
 * resolve with e_lsp_semantic_legend).  Returns the count (>=0) or -1. */
int e_lsp_semantic_tokens(e_lsp_session *s, const char *path,
                          e_lsp_sem_token *out, int max);

/* The token-type NAME for legend index @type ("keyword", "class", ...), as the
 * server declared in its initialize-response legend, or NULL if out of range. */
const char *e_lsp_semantic_legend(e_lsp_session *s, int type);

/* --- Semantic-token truecolor (1.6.6) ---------------------------------------
 * A tiny, fixed set of 24-bit colours for the semantic-token categories the
 * 16-colour palette cannot do justice to.  The headline case is
 * method/function/macro: they fell back to bright red because the 16 colours
 * have no orange, so they get a dedicated orange slot here.  This is the single
 * source of truth for BOTH renderers that can show 24-bit colour -- X11/Xft and
 * the direct-colour console -- while the 16-colour mapping (e_lsp_sem_fg in
 * we_debug.c) stays the fallback wherever truecolor is unavailable. */
#define LSP_SEM_TC_NONE (-1)

/* Upper bound on truecolor slots -- renderers size their colour tables to
   16 + LSP_SEM_TC_MAX so a foreground index of 16+slot selects a slot's RGB. */
#define LSP_SEM_TC_MAX 8

/* The truecolor SLOT for a semantic token-type NAME (as from
 * e_lsp_semantic_legend), or LSP_SEM_TC_NONE when the category has no dedicated
 * 24-bit colour and should use the 16-colour mapping. */
int e_lsp_sem_truecolor(const char *type);

/* The 24-bit RGB (each component 0..255) for truecolor slot @slot.  Returns 1
 * and fills *@r,*@g,*@b (any may be NULL) on success, or 0 if @slot is out of
 * range. */
int e_lsp_sem_slot_rgb(int slot, int *r, int *g, int *b);

#endif /* WE_LSP_H */
