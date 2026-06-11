# HACKING-LSP — the Language Server Protocol client

> **Status: SHIPPED for five languages -- 20+ actions + live diagnostics.** The
> `we_lsp.c` engine (validated vs real servers) plus the editor bridge on the
> `Alt-Q` prefix.  Companion to `HACKING-DAP.md`.  Wired servers:
> **Scala** (Metals), **C/C++** (clangd), **Python** (pyright, pylsp fallback),
> **Go** (gopls) and **Rust** (rust-analyzer) -- each one row in
> `e_lsp_servers[]`, no new code (see [the descriptor](#the-server-descriptor-table)).
> The bridge is the same for all of them; only Scala has extra per-server prep
> (the BSP/`scala-cli` bootstrap and the JDK pin).
>
> Keys (editor bridge, `e_lsp_ui_inp` in we_debug.c, dispatched from
> `e_prog_switch` on Alt-Q):
>
> | key | action | engine call |
> |-----|--------|-------------|
> | `Alt-Q E` | diagnostics (start server, list in Messages) | `e_lsp_wait_diagnostics` |
> | `Alt-Q D` | go-to-definition | `e_lsp_definition` |
> | `Alt-Q I` | go-to-implementation | `e_lsp_implementation` |
> | `Alt-Q T` | go-to-type-definition | `e_lsp_type_definition` |
> | `Alt-Q H` | hover (type/docs) | `e_lsp_hover` |
> | `Alt-Q C` | completion (popup, insert) | `e_lsp_completion` |
> | `Alt-Q R` | references (list in Messages) | `e_lsp_references` |
> | `Alt-Q B` | incoming calls -- called By (list in Messages) | `e_lsp_call_hierarchy` |
> | `Alt-Q G` | outgoing calls -- Goes to (list in Messages) | `e_lsp_call_hierarchy` |
> | `Alt-Q K` | supertypes -- vi K=up=parents (list in Messages) | `e_lsp_type_hierarchy` |
> | `Alt-Q J` | subtypes -- vi J=down=children (list in Messages) | `e_lsp_type_hierarchy` |
> | `Alt-Q V` | expand selection -- grow block mark by syntax (Visual) | `e_lsp_selection_range` |
> | `Alt-Q U` | document highlight -- mark Uses in this file | `e_lsp_document_highlight` |
> | `Alt-Q Y` | inlaY hints toggle -- inferred types dim at end-of-line (opt-in) | `e_lsp_inlay_hints` |
> | `Alt-Q S` | signature help | `e_lsp_signature_help` |
> | `Alt-Q O` | outline (popup -> jump) | `e_lsp_document_symbols` |
> | `Alt-Q L` | code Lenses (run/test/refs, popup -> jump) | `e_lsp_code_lenses` |
> | `Alt-Q W` | workspace symbol search (prompt, popup -> jump) | `e_lsp_workspace_symbols` |
> | `Alt-Q A` | code actions / quick-fixes (popup -> apply) | `e_lsp_code_actions` (+ `e_lsp_apply_code_action`) |
> | `Alt-Q N` | rename | `e_lsp_rename` (+ `e_lsp_replace_buffer`) |
> | `Alt-Q F` | format -- selection if marked, else whole file | `e_lsp_format` / `e_lsp_format_range` (+ `e_lsp_replace_buffer`) |
>
> `e_lsp_definition`/`_implementation`/`_type_definition` share the engine driver
> `lsp_locate`; their three bridge actions share `e_lsp_ui_jump`.  The completion
> / outline / workspace-symbol / code-action popups share `e_lsp_pick`.
> `lsp_apply_workspace_edit` is the shared WorkspaceEdit applier behind both
> rename and code-action apply (handles `changes` and `documentChanges`, counts
> other-file edits which the bridge warns about).
>
> Every editor change is pushed to the server (`e_lsp_did_change`, debounced on
> newline) and `e_lsp_poll` drains diagnostics non-blocking on each keystroke,
> so errors update live in Messages (the `on_diagnostics_summary` count) without
> an explicit save.  Rename/format apply server `TextEdit`s via `lsp_apply_edits`
> (pure, offset-based) then rebuild the buffer with `e_lsp_replace_buffer`.
>
> Inline diagnostic marks: `on_diagnostic` now also gets the range END, so the
> bridge keeps the problem ranges (`g_diag_active`, swapped in atomically per
> batch by `on_diagnostics_summary`, which also repaints the source window).
> `e_pr_c_line` (we_progn.c) calls `e_lsp_diag_active_for` once per line and
> `e_lsp_diag_attr_at` per character to recolor problem cells -- the same
> per-char colour override breakpoints/selection use, so it works in wpe and
> xwpe.  Colours are synthesized for the active backend (`e_lsp_diag_color`: a
> `16*bg+fg` VGA byte under colour, an existing marked attr under mono ncurses),
> so no FARBE field or colour-scheme persistence is touched.
> Tests: `tests/test_lsp_scala.c` (engine vs real Metals: every action incl.
> rename/format/diagnostic-range), `tests/test_scala_lsp*.py` (the bridge through
> wpe, incl. `_diag_marks.py` asserting the red cells render).

## Validated flow (2026-06-09, real Metals 1.6.7)

Proven by `xwpe-dev/tmp/scala-lsp-spike/lsp_probe.py` (the blueprint for
`we_lsp.c`), against a single `Factorial.scala` with a `.bsp/scala-cli.json`
created by `scala-cli setup-ide` (the same scaffolding the Scala DAP backend
already makes).  Results: hover `f` -> `var f: Long`; definition of a `f` use ->
its declaration line; completion at `println` -> `println(): Unit`,
`println(x: Any): Unit`; plus `publishDiagnostics` after compile.

Launch: `cs install metals` -> `~/.local/bin/metals` (stdio JSON-RPC, JVM).
Exact sequence:

```
initialize { rootUri, capabilities, initializationOptions } -> capabilities
initialized
textDocument/didOpen { uri, languageId:"scala", version, text }
  <- metals/status "importing..."  (answer nothing; it's a notification)
  <- textDocument/publishDiagnostics for the uri  == compiled
textDocument/hover       { uri, position } -> contents.value (markdown)
textDocument/definition  { uri, position } -> [ { uri, range } ]
textDocument/completion  { uri, position } -> { items: [ { label, ... } ] }
```

`initializationOptions` that keep Metals headless (no UI providers to implement
up front):

```json
{ "compilerOptions": { "isCompletionItemDetailEnabled": true,
                       "isHoverDocumentationEnabled": true },
  "statusBarProvider": "log-message", "inputBoxProvider": false,
  "quickPickProvider": false, "executeClientCommandProvider": false,
  "didFocusProvider": false, "isHttpEnabled": false,
  "fallbackScalaVersion": "3.3.7" }
```

GOTCHAS the engine MUST handle (or Metals blocks / desyncs):
* Answer EVERY server->client request: `window/showMessageRequest` (reply the
  first action or null), `client/registerCapability`,
  `window/workDoneProgress/create`, `workspace/configuration` (reply an array of
  nulls).  An unanswered request stalls Metals.
* The first request after `didOpen` may return empty while the presentation
  compiler warms -- retry hover/completion a few times with a short backoff.
* Reader must stay byte-aligned on `Content-Length` framing and resync if it
  ever sees a header block without a length (LSP4J occasionally interleaves).
* Requires `scala-cli` in PATH + a `.bsp` connection (reuse the Scala DAP
  bootstrap); modern Scala only (Metals dropped 2.11).

## Why LSP, after DAP

DAP made xwpe a **debugger** for modern languages.  LSP makes it an **IDE**.
The features developers who use Metals / clangd / rust-analyzer rely on most —
**completion, go-to-definition, find-references, type-on-hover, rename, and
inline diagnostics as you type** — are exactly what xwpe does not yet have (it
offers syntax highlighting and post-compile error parsing only).

Without LSP, xwpe competes on footprint and a zero-config Borland-style debug
UX.  *With* a minimal LSP client it becomes a genuine lightweight front-end for
the same language servers that power VS Code / Neovim / Emacs — but with the
discoverable Turbo-Vision UX and a few-MB binary, no Electron, no Lua config.

## The leverage: it is the same plumbing as DAP

LSP, DAP and BSP are **all JSON-RPC 2.0 with the same `Content-Length` framing**.
The DAP work already built the parts that are protocol-shaped, not DAP-specific:

| Already built (for DAP/BSP) | Reused by LSP |
|-----------------------------|---------------|
| `we_dap_proto.c` — Content-Length reader + frame builder | the wire format is identical |
| event-driven fd-loop (`we_fdloop.c`), non-blocking subprocess I/O | the LSP server is a long-lived child on the loop |
| json-c DOM (parse *and* build) | request/response + notification handling |
| subprocess spawn + lifecycle (`we_dap.c`, `we_bsp.c`) | spawn `clangd`, `metals`, … the same way |

So the genuinely **new** work is the *UI surface*, not the transport: completion
popups, hover boxes, diagnostic underlines/gutter marks, and a definition jump.
That is bigger than DAP (which reused the existing step/watch UI), but the
low-level half is done.

## Architecture (mirrors the DAP split)

```
  we_dap_proto.c/.h   wire format (REUSED — already transport-agnostic)
        |
        v
  we_lsp.c/.h         the session engine: spawn a server, initialize, manage
        |             open documents (didOpen/didChange), issue requests
        v             (completion/hover/definition), dispatch publishDiagnostics.
  we_debug.c-style    the editor bridge: completion popup, hover box, diagnostic
  bridge in we_edit   marks, go-to-definition jump, bound to editor keys.
```

### The server descriptor table

A new language server is a **row in `e_lsp_servers[]`** (we_debug.c), not new
code.  The real descriptor:

```c
typedef struct {
  const char  *lang;    /* matches e_lsp_lang_for(): "c"/"scala"/"rust"/… (the LSP languageId) */
  char *const *argv;    /* NULL-terminated spawn command; argv[0] is the PATH check */
  const char  *label;   /* short name for the status bar ("Metals", "clangd", "rust") */
  const char  *missing; /* hint shown when the binary is not installed */
} e_lsp_server;
```

`e_lsp_lang_for(f)` maps a file extension to the `lang` key (also the wire
`languageId`); `e_lsp_server_for(lang)` returns the matching row.  A language may
have **several rows in preference order**: `e_lsp_server_for` returns the first
whose `argv[0]` is on `PATH`, else the first row (so the bar and `missing` hint
still name the preferred server).  Python uses this — a `pyright` row then a
`pylsp` row — so it picks pyright when present, pylsp otherwise, zero config.
`argv` carries extra arguments where needed (pyright is
`{"pyright-langserver","--stdio",NULL}`).  The shipped table:

| lang | argv[0] | label | notes |
|------|---------|-------|-------|
| `scala`  | `metals`             | Metals  | BSP/`scala-cli` bootstrap + JDK pin (only server with prep) |
| `c`,`cpp`| `clangd`             | clangd  | in Debian; no JVM; xwpe's home turf |
| `python` | `pyright-langserver` | pyright | preferred; not in Debian (pip/npm) |
| `python` | `pylsp`              | pylsp   | fallback; in Debian (`python3-pylsp` + `python3-pyflakes` for diagnostics) |
| `go`     | `gopls`              | gopls   | needs a `go.mod` module |
| `rust`   | `rust-analyzer`      | rust    | label kept short to fit the 80-col bar; needs a Cargo crate + `rust-src` |

**Two non-obvious paths must both be touched per server** (see also the
`project_lsp_add_server` memory):

1. **The `languageId` thread** (we_lsp.c): the session stores `lang_id`;
   `e_lsp_did_open` sends it (it used to be hardcoded `"scala"`, which would make
   clangd mis-parse).  Server-specific notifications are gated by lang —
   `metals/didFocusTextDocument` is skipped unless `lang_id == "scala"`.
2. **The status-bar label is a SEPARATE hardcoded path the engine test does not
   exercise.**  The bottom-bar entry "Alt-Q ? Metals" is a static `WOPT` literal
   (`eblst_lsp_o[5]/eblst_lsp_u[5].t` in we_main.c).  `e_lsp_bar_label(f)`
   (we_debug.c, called from we_wind.c before `e_pr_uul`) rewrites it to
   "Alt-Q ? <label>" and re-packs the button row with `e_lsp_pack_bar` (adaptive
   gap so the 7 standard buttons + the LSP entry still fit 80 cols — this is why
   `rust-analyzer` is abbreviated to `rust`).  Add a server and forget this, and
   the bar keeps saying "Metals".

## How the engine was built (history)

The engine was delivered in the order below; it is recorded here because the same
sequence is the cheapest path for anyone porting the client elsewhere:

1. **Transport/engine** (`we_lsp.c`): `initialize`/`initialized`, `shutdown`,
   `textDocument/didOpen` + `didChange` (full-document sync), request/response
   correlation + notification dispatch on the fd-loop.
2. **Diagnostics** (`textDocument/publishDiagnostics`): inline marks + a
   navigable list — the highest value-for-effort surface, and it validates the
   engine end-to-end.
3. **Completion** (`textDocument/completion`): a Borland-style popup at the
   cursor; insert on Enter.
4. **Hover + go-to-definition**, then references, rename, signature help,
   formatting, the call/type hierarchies, code actions and the rest of the
   `Alt-Q` table above.

## Servers wired (all shipped)

| Server | Language | In Debian? | Per-server notes |
|--------|----------|------------|------------------|
| **clangd** | C/C++ | yes (`clangd`) | trivial dependency; no JVM; the first server CI runs for real |
| **Metals** | Scala | no (`cs install metals`) | reuses the `scala-cli`/BSP tooling from the Scala DAP backend |
| **pyright** | Python | no (pip/npm) | preferred; lints out of the box |
| **pylsp** | Python | yes (`python3-pylsp`) | fallback; diagnostics need `python3-pyflakes` |
| **rust-analyzer** | Rust | yes (`rust-analyzer`) | needs a Cargo crate + `rust-src` for std go-to-def |
| **gopls** | Go | yes (`gopls`) | needs a `go.mod`; a real compiler, so diagnostics are reliable |

## Testing & demos

Each server is covered by **two layers** (both needed — they exercise different
code):

1. **Engine C test** `tests/test_lsp_<server>.c` (model: `test_lsp_clangd.c`):
   drives `we_lsp.h` directly against a *real* server —
   diagnostics/hover/definition/completion/references/outline/rename. Fast,
   self-skips (exit 77) when the server is absent (so Metals costs 0 in Debian
   CI, which has no JDK; clangd-class servers run for real in seconds). This
   layer **bypasses `we_debug.c`**.
2. **One pyte bridge test** `tests/test_<server>_lsp.py` (model:
   `test_clangd_lsp.py`): drives the *editor* — extension→server detection, the
   eager start, the "Alt-Q ? <label>" bar text, Alt-Q E diagnostics. This is the
   only layer that catches the bar-label path; `skipif(which(server) is None)`.

Wire a new engine test into `Makefile.am` (`check_PROGRAMS` + `TESTS` +
`EXTRA_DIST`); add the pyte test to `EXTRA_DIST`; add the server to the Debian
`debian/tests/control` Test-Depends so CI runs it for real where the binary is
packaged.

**Per-language demo testbeds + tour GIFs** live under `docs/`: each language has
`docs/examples/<lang>-lsp/` — a small, fully-commented project where every
`Alt-Q` action is annotated in that language's idioms (verified to compile *and*
to parse clean under its server) — and a tour GIF
`docs/demos/gifs/<lang>/tour.gif` recorded from `docs/demos/tapes/<lang>/tour.tape`
by `docs/demos/record-tours.sh` (records against a throwaway copy so server
caches stay out of the repo). The user-facing walkthrough is `docs/LSP.md` /
`docs/chapters/lsp.texi`.

## Notes / constraints

* Keep the engine free of editor globals (like `we_dap.c`) so it can be
  integration-tested against a real server without running xwpe.
* `didChange` should send incremental ranges, not the whole buffer, on large
  files; start with full-document sync for correctness, optimise later.
* Diagnostics arrive asynchronously (not in response to a request) — they must
  land on the fd-loop and repaint without blocking typing.
* For Scala, the server (Metals) is itself reached through a build server, the
  same shape as the BSP bootstrap in `we_bsp.c` — that code is a head start.
* **Too-new JDK gotcha (cost a debug cycle 2026-06-10).** On Debian sid the
  default JDK was OpenJDK 26; the Scala 3 compiler (3.3.7 *and* 3.8.3) crashes at
  start-up with `assertion failed: asTerm called on not-a-Term val <none>` in
  `Definitions.init`/`ObjectClass`. **There are TWO JVMs and only one of them is
  fixed by `project.scala`:** `//> using jvm temurin:21` pins the *build* JVM
  (scala-cli/Bloop) — so the build compiles, SemanticDB is written, and
  diagnostics work — but Metals' **presentation compiler** (hover, completion,
  go-to-definition) runs in *Metals' own* JVM, which is whatever `JAVA_HOME` /
  the `metals` launcher selects. On a too-new default JDK the PC crashes and
  every navigation/hover action silently returns `[]`/null while diagnostics look
  fine — a confusing split. **The real fix is to run Metals on an LTS JDK: set
  `JAVA_HOME` (and prepend its `bin` to `PATH`) before launching `wpe`.**
  Verified: `JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64` stops the crash
  (metals.log then warns about java-21 not java-26, no `asTerm`). xwpe now
  auto-pins Metals' JVM to a supported JDK when the default is too new; surfaced
  to users in `docs/chapters/lsp.texi` / `docs/LSP.md` (the "two JVMs"
  subsection).
* **`e_lsp_buffer_text` newline-doubling bug (fixed 2026-06-10).** xwpe stores
  each line in `b->bf[y].s` terminated by an embedded `WPE_WR` (0x0A) marker
  (that is how `e_write`/Save delimits lines on disk). The buffer→string
  serializer used `strlen()` (copying that 0x0A) **and** appended its own `\n`,
  doubling every newline: a 17-line file reached the server as 34 lines, so every
  LSP position was off by whole lines and definition/hover returned nothing —
  and this, not Metals, was the real cause of the "hover always null" report.
  Fix: the serializer `e_lsp_join_lines()` (we_lsp.c, unit-tested) copies each
  line up to `WPE_WR` then appends one `\n` (mirrors `e_write`). Lesson: the
  didOpen/didChange text MUST be byte-identical to what `File>Save` writes —
  diff it against disk when positions look wrong.
* Cross-file navigation works (definition/implementation/type-definition/
  workspace-symbol jump open the target file via `e_d_goto_break`→`e_edit`), but
  the server holds ONE open document at a time: switching the Alt-Q focus to a
  different file makes `e_lsp_ensure` close+reopen Metals for it. Rename applies
  only the current file's WorkspaceEdit; other files are counted and reported.
