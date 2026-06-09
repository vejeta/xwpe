# The DAP debug client

xwpe's seventh debugger backend, `DEB_DAP`, speaks the **Debug Adapter
Protocol** — the wire protocol behind VS Code, Neovim and Emacs DAP.  Instead
of a hand-written, per-language text-scraping backend (as for gdb, jdb, pdb,
a68g), one DAP client gives source-level debugging for *any* language with a
DAP adapter.  Today: **Go** (Delve) and **Rust** (gdb).  Adding a language is a
one-row descriptor, never new plumbing.

This file is the contributor reference for that subsystem.  For *using* the
debugger (keys, requirements) see the "Debugging" chapter of the manual
(`docs/chapters/debugging.texi`).  For the general text backends see the
"Debugger backend architecture" section of `HACKING.md`.

## Three layers

```
  we_dap_proto.c/.h   wire format, transport-agnostic, NO xwpe globals
        |             (Content-Length framing, request builder, reassembly)
        v
  we_dap.c/.h         the session engine: spawn an adapter, run a synchronous
        |             request/response session, call host callbacks for UI
        v             side-effects.  NO editor globals -> unit-testable.
  we_debug.c          the editor bridge: maps Ctrl-G R / F8 / F7 / Ctrl-G W /
                      Ctrl-G Q onto the engine, paints stops, builds Watches.
```

The split is deliberate: the bottom two layers are free of editor state and are
tested against *real* adapters without running xwpe (see Testing).  Only the
bridge touches the file-scope debugger globals (`e_d_swtch`, `e_d_file`, the
breakpoint/watch arrays), which is why it lives inside `we_debug.c` next to the
jdb/pdb/a68g helpers rather than in a separate file.

### Layer 1 — `we_dap_proto.c`

* `e_dap_reader` — a growable byte buffer that reassembles `Content-Length:
  N\r\n\r\n<json>` frames from a stream; `e_dap_reader_pop()` returns the next
  complete message or NULL, resyncing past malformed input.
* `e_dap_build_request(seq, command, arguments, &len)` — serialises one request
  frame.  Does **not** consume `arguments` (no `json_object_get`).

JSON library is **json-c** (`libjson-c-dev`), a hard build dependency, chosen
over jsmn because we both parse *and* build messages and json-c's DOM is far
less code.

### Layer 2 — `we_dap.c` (engine)

Public API (`we_dap.h`):

| call | does |
|------|------|
| `e_dap_open(argv, program, entry_func, cwd, host)` | reverse-TCP transport (Delve): listen, spawn `dlv dap --client-addr=…`, accept, `initialize` |
| `e_dap_open_stdio(argv, program, entry_func, cwd, host)` | stdio transport (gdb/lldb): spawn with pipes, `initialize` |
| `e_dap_add_breakpoint(s, file, line)` | record a source breakpoint to install on Run |
| `e_dap_run(s)` | launch + setBreakpoints + configurationDone → first stop |
| `e_dap_step(s, verb)` | `"continue"`/`"next"`/`"stepIn"`/`"stepOut"` → next stop |
| `e_dap_evaluate(s, expr)` | `evaluate` in the top frame (context `watch`); malloc'd value |
| `e_dap_ended(s)` / `e_dap_close(s)` | program-exited flag / disconnect + reap |

The host supplies callbacks so the engine never touches the UI directly:

```c
typedef struct {
  void (*on_stopped)(const char *file, int line, const char *reason, void *ud);
  void (*on_output)(const char *text, void *ud);
  void (*on_terminated)(void *ud);
  void *ud;
} e_dap_host;
```

The model is **synchronous** (send a request, pump messages until the matching
response, dispatching intervening events) — the same shape as xwpe's text
backends, which keeps the bridge simple.

### Layer 3 — `we_debug.c` (bridge)

`DEB_DAP` is selected automatically by file extension.  Guarded one-line hooks
route the editor's existing debug entry points to the bridge when a DAP session
is (or should be) active:

| editor action | hook site | bridge fn |
|---------------|-----------|-----------|
| start / Ctrl-G R | `e_start_debug`, `e_deb_run` | `e_d_dap_start`, `e_d_dap_run` |
| F8 / F7 | `e_d_step_next` | `e_d_dap_step` (`sw` → `next` vs `stepIn`) |
| Watches refresh | `e_d_p_watches` | `e_d_dap_watches` (via `e_dap_evaluate`) |
| Ctrl-G Q | `e_d_quit` | `e_d_dap_quit` |

The six text backends are untouched: every hook is `if (DAP) return …;` at the
top of the function.

## Two transports — and when each

DAP standardises the *messages*, **not** how you connect.  Each adapter dictates
its transport; it is not our choice:

| Transport | Used by | Why | Program output |
|-----------|---------|-----|----------------|
| **reverse-TCP** | Delve (`dlv dap` is a TCP server), remote/attach | the adapter is a server you connect to | a **pty**: dlv's stdio is free, so the debuggee gets a real terminal we read |
| **stdio** | gdb, lldb-dap (Rust/C/C++…) | spawn-a-local-binary; DAP flows on the adapter's own stdin/stdout | DAP **`output` events**: the adapter's stdio is consumed by the protocol, so the program can't use it |

The transport therefore decides **where the debuggee's output goes**, which is
the part that actually matters to us:

* **reverse-TCP**: `e_dap_open` creates a controlling pty (`openpty` in the
  parent, raw mode), the child takes the slave as its ctty/stdio, the debuggee
  inherits it, and `dap_read_message` polls the pty master alongside the socket.
  (dlv *requires* a tty — it runs the inferior in foreground and `tcsetpgrp`s its
  stdin, which fails on a plain pipe with "inappropriate ioctl for device".)
* **stdio**: no pty; the engine forwards `output` events of category
  `stdout`/`stderr` — but only **after the first stop** (`forward_output`),
  because gdb tags its banner and pending-breakpoint chatter as `stdout` during
  launch/configuration and that would otherwise clutter Messages.

`dap_wfd()`/`dap_rfd()` abstract the transport fd so the message pump is
transport-neutral; `dap_spawn` (TCP, appends `--client-addr`) and
`dap_spawn_stdio` (two pipes) are the only transport-specific spawn code.

Rule of thumb: **spawn-a-local-binary adapter → stdio; connect-to-a-server, or
remote/attach → TCP.**

## Adding a language

Append one row to `DAP_LANGS[]` in `we_debug.c`:

```c
typedef struct {
  const char  *ext;         /* ".go" / ".rs"                              */
  char *const *argv;        /* adapter command line                       */
  const char  *entry_func;  /* stop-at-entry function, or NULL            */
  int          stdio;       /* 0 = reverse-TCP, 1 = stdio                 */
  int          compile;     /* 0 = adapter builds, 1 = xwpe compiles first*/
} e_d_dap_lang;

static const e_d_dap_lang DAP_LANGS[] = {
  { ".go", DAP_ARGV_GO,   "main.main", 0, 0 },  /* dlv builds the package      */
  { ".rs", DAP_ARGV_RUST, NULL,        1, 1 },  /* rustc -g, then gdb debugs it*/
};
```

* `compile = 1` languages are built by `e_d_dap_compile` (compiler + flags from
  the file's Options entry → `<stem>.dbg`, diagnostics shown in Messages on
  failure) before the adapter launches; `compile = 0` languages let the adapter
  build (dlv compiles the Go package).
* You almost always also want a matching compiler entry in `we_prog.c`
  (`e_ini_prog`) so `e_check_c_file` recognises the extension (needed by
  `e_breakpoint` and `e_d_quit`) and supplies the compiler/flags.
* `entry_func` gives a "stop at main" like gdb's temp breakpoint.  Leave it NULL
  when the language's entry maps to a runtime stub with no source (Rust's `main`
  binds to the C runtime stub under gdb; rely on the user's line breakpoint).

`lldb-dap` for Rust/C/C++ is a drop-in: `{ ".rs", {"lldb-dap"}, NULL, 1, 1 }`.

## Session lifecycle and its per-adapter quirks

Canonical flow: `initialize` → (`initialized` event) → `launch` →
`setBreakpoints` → `configurationDone` → `stopped` → `stackTrace`/`evaluate` →
`continue`/`next`/… → `terminated`.  Adapters differ in the fine print, and the
engine handles both:

* **Launch response timing.**  dlv replies to `launch` *immediately*; gdb defers
  it until *after* `configurationDone` (per spec).  Blocking on the launch
  response would deadlock gdb, so the engine waits for it only when
  `launch_mode_debug` (dlv); for gdb the late response is consumed by the
  stop-pump.
* **`mode`.**  dlv's launch carries `"mode":"debug"` (build the package); gdb's
  launches a prebuilt binary with no mode.
* **`initialized`.**  gdb emits it; dlv does not.  We don't block on it — by the
  time breakpoints are sent it has arrived (gdb) or never comes (dlv), and
  either way the engine just sends breakpoints + configurationDone.

## Gotchas (all instrument-first, all fixed)

The cheap way to re-derive any of these is `DAP_TRACE`-style logging in
`dap_wait_response` plus an X11 screenshot of Messages.

1. **dlv "Failed to launch DIR/"** — strip the trailing slash from the program
   directory; dlv's package resolution rejects `DIR/`.
2. **dlv needs a controlling pty** — foreground `tcsetpgrp` fails on a pipe.
3. **dlv banner in Messages** — `dlv dap` emits "Type 'dlv help' …" as a DAP
   `console` event; with the pty model we drop DAP output events entirely for
   the TCP transport (program output comes from the pty).
4. **No `^M` in program output** — put the pty in raw mode (`cfmakeraw`), else
   `\n` is cooked to `\r\n`.
5. **gdb defers the launch response** (see lifecycle) — don't block on it.
6. **gdb banner / pending-breakpoint chatter is category `stdout`** — forward
   program output only after the first stop.
7. **`e_dap_close` hung on gdb** — gdb ptracing a stopped inferior ignores
   SIGTERM; reap with SIGTERM → 200 ms grace → SIGKILL (`PTRACE_O_EXITKILL`
   takes the inferior down too).
8. **Watch-before-Run mis-routed `.go` to gdb** — the Watches pane is also a
   text window; `e_start_debug` scans *all* windows for a DAP-language source,
   not the focused one.
9. **Step-over past `main` wandered into the runtime** — after `main` returns,
   `next` has no user line, so it climbs into `runtime/proc.go` (Go) or the C
   runtime.  `e_d_dap_in_user_code()` (stop file under the program dir) makes
   step-over run on to the next user line or to exit instead.  The cursor jump is
   also *deferred*: `e_d_dap_on_stopped` only records the stop, `e_d_dap_paint_stop`
   paints it once after run/step settles, so intermediate runtime stops never
   flash.

## Testing

| test | drives | gate |
|------|--------|------|
| `tests/test_dap.c` | the wire format (`we_dap_proto`) alone | always |
| `tests/test_dap_dlv.c` | real `dlv dap` (Go, reverse-TCP) | SKIP without `dlv`+`go` |
| `tests/test_dap_gdb.c` | real `gdb --interpreter=dap` (Rust, stdio) | SKIP without `gdb`+`rustc` |
| `tests/test_go_dap.py` | the Go editor path (pyte) | SKIP without `dlv`+`go` |
| `tests/test_rust_dap.py` | the Rust editor path (pyte) | SKIP without `rustc`+`gdb` |

The C integration tests link only `we_dap.c` + `we_dap_proto.c` (+ `libjson-c`,
`libutil`), proving the engine works with no editor.  `make check` runs them
all; the pyte tests are run from `tests/`.

## Status and next

Wired: Go (Delve, reverse-TCP), Rust (gdb, stdio).  Natural follow-ups, all on
the existing two transports:

* **C/C++** via gdb/lldb-dap — a real new *language*.  Would need a `DEB_DAP`
  opt-in versus the legacy gdb text backend for `.c`/`.cpp` (those extensions
  already route to the text backend today).
* **lldb-dap** — *optional, deferred until there's demand.*  On Linux it adds
  nothing over gdb for Rust: gdb already debugs Rust well, is installed
  everywhere, and rustc embeds the pretty-printer script gdb auto-loads.  It is
  a *substitute* for what we have, not an addition.  Keep it on the radar for:
  - **lldb-only environments** — a user who has LLVM/lldb but not gdb, or
    prefers it;
  - **macOS / non-Linux** — lldb is the native debugger there and gdb may be
    absent or worse (relevant if xwpe is ever used on macOS, e.g. by the
    maintainer);
  - it is the same adapter for Swift/Zig too.
  Cost when wanted: one `DAP_LANGS` row — `{ ".rs", {"lldb-dap"}, NULL, 1, 1 }`
  (or a config toggle to pick the adapter) on the stdio transport that already
  exists.  No new plumbing.
* **Scala** via Metals — heavier: Metals is an LSP server that exposes DAP
  through a build server (Bloop/sbt), so it needs a discovery/handshake layer
  on top of the TCP transport, not just a descriptor row.
* **DAP *server* mode** (task #50) — expose xwpe itself as a debuggee target.
