# HACKING-LSP — the planned Language Server Protocol client

> **Status: ENGINE + 3 of 4 surfaces SHIPPED (Scala/Metals).** Done: the
> `we_lsp.c` engine (validated against real Metals) and the editor bridge for
> **diagnostics, go-to-definition and hover** (Alt-Q prefix).  Remaining:
> **completion** (the navigable popup widget + insert-on-Enter), and
> didChange-driven live diagnostics.  Companion to `HACKING-DAP.md`.
>
> Keys (editor bridge, `e_lsp_ui_inp` in we_debug.c, dispatched from
> `e_prog_switch` on Alt-Q): `Alt-Q E` start server + diagnostics, `Alt-Q D`
> go-to-definition, `Alt-Q H` hover.  Tests: `tests/test_lsp_scala.c` (engine vs
> real Metals: hover/definition/completion/diagnostics) and
> `tests/test_scala_lsp.py` (the bridge through wpe).

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

A per-server descriptor table, exactly like `DAP_LANGS[]`:

```c
typedef struct {
  const char  *ext;       /* ".c"/".scala"/".rs"/…           */
  char *const *argv;      /* the language server command line */
  int          bsp;       /* 1 = obtain the server via a build server (Scala)   */
} e_lsp_lang;
```

## Phased plan

1. **Transport/engine** (`we_lsp.c`): `initialize`/`initialized`, `shutdown`,
   `textDocument/didOpen` + `didChange` (sync the buffer to the server),
   request/response correlation + notification dispatch on the fd-loop.
2. **Diagnostics**: handle `textDocument/publishDiagnostics` → inline
   error/warning marks in the editor + a navigable list (reuse the F9 error
   list UI).  This is the highest value-for-effort surface and validates the
   engine end-to-end.
3. **Completion**: `textDocument/completion` → a Borland-style popup at the
   cursor; insert on Enter.
4. **Hover + go-to-definition**: type/doc on a key; jump to the definition with
   a "jump back" stack.
5. (later) rename, find-references, signature help, formatting.

## Target servers, in order

| Order | Server | Language | Why first |
|-------|--------|----------|-----------|
| 1 | **clangd** | C/C++ | in Debian (`clangd` package) → trivial dependency; xwpe's home turf |
| 2 | **Metals/Bloop** | Scala | reuses the `scala-cli`/BSP tooling already wired for Scala DAP |
| 3 | pyright / pylsp | Python | common, easy to install |
| 4 | rust-analyzer | Rust | the standard Rust server |
| 5 | gopls | Go | completes the DAP language set |

## Notes / constraints

* Keep the engine free of editor globals (like `we_dap.c`) so it can be
  integration-tested against a real server without running xwpe.
* `didChange` should send incremental ranges, not the whole buffer, on large
  files; start with full-document sync for correctness, optimise later.
* Diagnostics arrive asynchronously (not in response to a request) — they must
  land on the fd-loop and repaint without blocking typing.
* For Scala, the server (Metals) is itself reached through a build server, the
  same shape as the BSP bootstrap in `we_bsp.c` — that code is a head start.
