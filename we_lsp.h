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

/* Severity as in LSP: 1 error, 2 warning, 3 information, 4 hint. */
typedef struct {
 void (*on_diagnostic)(const char *path, int line, int character,
                       int severity, const char *message, void *ud);
 /* Called once per publishDiagnostics batch with the error/warning totals --
    for a non-spammy live "N errors, M warnings" status as you type. */
 void (*on_diagnostics_summary)(const char *path, int errors, int warnings,
                                void *ud);
 void *ud;
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

/* A document symbol for the outline (engine-owned name). */
typedef struct {
 char *name;      /* symbol name (object/def/val/...)        */
 int   line;      /* 0-based line of the symbol              */
 int   character; /* 0-based                                 */
 int   kind;      /* LSP SymbolKind                          */
} e_lsp_symbol;

/* Spawn the language server (argv NULL-terminated, e.g. {"metals",0}), run the
 * initialize/initialized handshake with headless InitializationOptions, cwd =
 * root_dir.  `lang` is the LSP languageId ("scala", "c", …) used on didOpen.
 * Returns a session or NULL. */
e_lsp_session *e_lsp_open(char *const argv[], const char *root_dir,
                          const char *lang, const e_lsp_host *host);

/* textDocument/didOpen: hand the server the buffer text so it compiles/indexes.
 * `path` is a filesystem path (turned into a file:// URI internally). */
int e_lsp_did_open(e_lsp_session *s, const char *path, const char *text);

/* textDocument/didChange: replace the server's copy of the document with the
 * current buffer text (full-document sync), bumping the version.  Call this
 * before a request so hover/definition/completion/diagnostics reflect unsaved
 * edits rather than the on-disk file. */
int e_lsp_did_change(e_lsp_session *s, const char *path, const char *text);

/* Pump until the server publishes diagnostics for `path` (== it has compiled),
 * delivering every diagnostic through host->on_diagnostic.  Returns 1 if the
 * file's diagnostics arrived, 0 on timeout/EOF. */
int e_lsp_wait_diagnostics(e_lsp_session *s, const char *path, int timeout_ms);

/* Non-blocking: dispatch any messages already waiting from the server
 * (diagnostics -> host, server requests answered) and return immediately.
 * Call it on each keystroke so live diagnostics surface without blocking
 * typing.  Returns the number of messages handled. */
int e_lsp_poll(e_lsp_session *s);

/* textDocument/hover at (line,character): returns a malloc'd plain-text string
 * (markdown fences stripped) the caller frees, or NULL. */
char *e_lsp_hover(e_lsp_session *s, const char *path, int line, int character);

/* textDocument/signatureHelp at (line,character): the active overload's
 * signature label (e.g. "def println(x: Any): Unit").  malloc'd, caller frees,
 * or NULL.  Most useful with the cursor inside a call's parentheses. */
char *e_lsp_signature_help(e_lsp_session *s, const char *path,
                           int line, int character);

/* textDocument/definition: on success fills out_path/out_line/out_char with the
 * first location and returns 0; returns -1 if there is no definition. */
int e_lsp_definition(e_lsp_session *s, const char *path, int line, int character,
                     char *out_path, size_t out_sz, int *out_line, int *out_char);

/* textDocument/completion: fills up to `max` items and returns the count (>=0),
 * or -1 on error.  Items point at engine-owned memory (see e_lsp_completion_item). */
int e_lsp_completion(e_lsp_session *s, const char *path, int line, int character,
                     e_lsp_completion_item *items, int max);

/* textDocument/references: every use of the symbol at (line,character),
 * including its declaration.  Fills up to `max` locations (engine-owned paths);
 * returns the count (>=0) or -1. */
int e_lsp_references(e_lsp_session *s, const char *path, int line, int character,
                     e_lsp_location *locs, int max);

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

/* textDocument/rename: rename the symbol at (line,character) to `new_name`.
 * Applies the resulting edits for THIS file to `current_text` and returns the new
 * full text (malloc'd), or NULL.  *other_files, if non-NULL, is set to the number
 * of OTHER files the rename also touches (not applied here -- the caller warns). */
char *e_lsp_rename(e_lsp_session *s, const char *path, int line, int character,
                   const char *new_name, const char *current_text,
                   int *other_files);

/* shutdown/exit the server and reap it; frees the session. */
void e_lsp_close(e_lsp_session *s);

#endif /* WE_LSP_H */
