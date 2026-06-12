# The DAP debug client

xwpe's seventh debugger backend, `DEB_DAP`, speaks the **Debug Adapter
Protocol** — the wire protocol behind VS Code, Neovim and Emacs DAP.  Instead
of a hand-written, per-language text-scraping backend (as for gdb, jdb, pdb,
a68g), one DAP client gives source-level debugging for *any* language with a
DAP adapter.  Today: **Go** (Delve), **Rust** (gdb/lldb-dap) and **Scala/JVM**
(Bloop's `scala-debug-adapter`, reached over a BSP handshake).  Adding a language
on one of the three existing transports is a one-row descriptor; only a genuinely
new transport (as Scala's BSP-hosted endpoint was) needs new plumbing.

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
| `e_dap_open_tcp(host, port, program, entry_func, host_cb)` | TCP-client transport (Scala/Bloop): connect to an endpoint a build server already started, `initialize` (no spawn, no pty) |
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

## Three transports — and when each

DAP standardises the *messages*, **not** how you connect.  Each adapter dictates
its transport; it is not our choice:

| Transport | Used by | Why | Program output |
|-----------|---------|-----|----------------|
| **reverse-TCP** | Delve (`dlv dap` is a TCP server), remote/attach | the adapter is a server you connect to | a **pty**: dlv's stdio is free, so the debuggee gets a real terminal we read |
| **stdio** | gdb, lldb-dap (Rust/C/C++…) | spawn-a-local-binary; DAP flows on the adapter's own stdin/stdout | DAP **`output` events**: the adapter's stdio is consumed by the protocol, so the program can't use it |
| **TCP-client** | Scala/Bloop (`e_dap_open_tcp`) | a build server already started the adapter and handed back a `tcp://` endpoint — we just connect | DAP **`output` events** (no pty) |

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

### The JVM/Scala case — BSP bootstrap (`we_bsp.c`)

The JVM has **no standalone DAP server**: `com.microsoft.java-debug` and the
Scala Center `scala-debug-adapter` are *libraries* that a host (jdt.ls, Bloop,
sbt) must wire up.  So Scala can't be "spawn a binary".  Instead `we_bsp.c`
drives a short **Build Server Protocol** handshake against `scala-cli` (which
bundles Bloop, which hosts `scala-debug-adapter`) and gets back a DAP endpoint:

```
.bsp/<name>.json (scala-cli setup-ide writes it) → launch the BSP server (stdio)
build/initialize → build/initialized → workspace/buildTargets (NON-test target!)
→ buildTarget/compile → buildTarget/scalaMainClasses
→ debugSession/start {dataKind:"scala-main-class"} → "tcp://HOST:PORT"
```

BSP is JSON-RPC with the **same Content-Length framing as DAP**, so `we_bsp.c`
reuses `e_dap_reader` for the wire — no second parser, no new dependency.  The
bridge then calls `e_dap_open_tcp(host, port, …)` and the *existing* engine takes
over; the only DAP-order wrinkle is that scala-debug-adapter is strict —
`setBreakpoints` must come **after** `launch` *and* the `initialized` event
("Empty debug session" otherwise), handled by the session's `wait_init` flag.
The `e_bsp_session` (the build server) is kept alive in `g_bsp` for the whole
debug session and closed *after* the DAP socket on quit, because it hosts the
adapter.  Nothing Scala ships in xwpe; `scala-cli` is an external coursier tool
(`cs install scala-cli`), the same way `dlv`/`gdb` are external.  Editor gate:
`.scala` is unknown to `e_check_c_file` (no compiler-table entry — Bloop builds
it), so `e_d_dap_source_ext()` lets DAP languages carry breakpoints anyway.

## Adding a language

Append one row to `DAP_LANGS[]` in `we_debug.c`:

```c
typedef struct {
  const char  *ext;         /* ".go" / ".rs" / ".scala"                   */
  char *const *argv;        /* adapter command line (argv[0] must be in PATH) */
  char *const *argv_alt;    /* alternative adapter, or NULL (e.g. lldb-dap)*/
  const char  *entry_func;  /* stop-at-entry function, or NULL            */
  int          stdio;       /* 0 = reverse-TCP, 1 = stdio                 */
  int          compile;     /* 0 = adapter builds, 1 = xwpe compiles first*/
  int          bsp;         /* 1 = JVM/Scala: BSP-bootstrap a DAP endpoint */
} e_d_dap_lang;

static const e_d_dap_lang DAP_LANGS[] = {
  { ".go",    DAP_ARGV_GO,    NULL,             "main.main", 0, 0, 0 },
  { ".rs",    DAP_ARGV_RUST_GDB, DAP_ARGV_RUST_LLDB, NULL,   1, 1, 0 },
  { ".scala", DAP_ARGV_SCALA, NULL,             NULL,        0, 0, 1 },
};
```

* `compile = 1` languages are built by `e_d_dap_compile` (compiler + flags from
  the file's Options entry → `<stem>.dbg`, diagnostics shown in Messages on
  failure) before the adapter launches; `compile = 0` languages let the adapter
  build (dlv compiles the Go package; Bloop compiles the Scala target).
* `bsp = 1` routes the open through `we_bsp.c` (see above) + `e_dap_open_tcp`
  instead of spawning the adapter directly; `argv[0]` is the build tool that must
  be in PATH (`scala-cli`), not a DAP server.
* For non-BSP languages you usually also want a matching compiler entry in
  `we_prog.c` (`e_ini_prog`) so `e_check_c_file` recognises the extension; BSP
  languages skip that and rely on `e_d_dap_source_ext()` for the breakpoint gate.
* `entry_func` gives a "stop at main" like gdb's temp breakpoint.  Leave it NULL
  when the entry maps to a runtime stub with no source (Rust under gdb) or when
  the user is expected to set a line breakpoint (Scala).

`lldb-dap` for C/C++ is a drop-in on the stdio path.

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
| `tests/test_dap_lldb.c` | real `lldb-dap` (Rust, stdio) | SKIP without `lldb-dap`+`rustc` |
| `tests/test_go_dap.py` | the Go editor path (pyte) | SKIP without `dlv`+`go` |
| `tests/test_rust_dap.py` | the Rust editor path (pyte; force lldb with `XWPE_DAP_ADAPTER=lldb`) | SKIP without `rustc`+`gdb` |

The C integration tests link only `we_dap.c` + `we_dap_proto.c` (+ `libjson-c`,
`libutil`), proving the engine works with no editor.  `make check` runs them
all; the pyte tests are run from `tests/`.

## Choosing the adapter (gdb vs lldb-dap)

A `DAP_LANGS` row can name a preferred adapter **and** an alternative
(`argv` + `argv_alt`).  Rust lists `gdb --interpreter=dap` first and `lldb-dap`
as the alternative; both ride the same stdio transport with no engine change.
`e_d_dap_choose_argv()` picks at session start, in order:

1. `XWPE_DAP_ADAPTER=<substring>` — if set and it names an *installed*
   candidate, force it (e.g. `XWPE_DAP_ADAPTER=lldb` — scripts/CI).
2. the saved dialog choice `e_dap_adapter` — set in **Debug → debugger Options**
   (Ctrl-G O: Auto / gdb / lldb radios), persisted in `~/.xwpe/xwperc` as
   `DAPAdapter : N` via the `[Programming]` section (`WpeWriteProgramming` /
   the option reader in we_opt.c).  `1`=gdb, `2`=lldb, matched by name.
3. otherwise the first candidate in `PATH` — so a **gdb-less macOS box
   auto-selects lldb-dap**, while Linux defaults to gdb.

The env value and the saved choice feed the *same* name-matching path, so the
selection is robust to the descriptor's primary/alt order.

lldb-dap is the native Rust/C/C++ debugger on macOS and works identically here
(it even reports the mangled Rust symbol, e.g. `main::main::h…`).

## Status and next

The DAP **client** (task #49) is done.  Wired, and integration-tested against
real adapters (`tests/test_dap_*`, each self-skips when its tooling is absent):

* **Go** — Delve (`dlv dap`), reverse-TCP transport.
* **Rust** — gdb (`--interpreter=dap`) **or** lldb-dap, stdio transport.
* **Scala/JVM** — Bloop's `scala-debug-adapter`, reached over the BSP bootstrap
  (`we_bsp.c`: `scala-cli setup-ide` → BSP `debugSession/start` → a `tcp://`
  endpoint) and driven over the TCP transport.  This is the third transport, no
  longer a blocked follow-up.

Remaining:

* **C/C++** via gdb/lldb-dap — a real new *language*, not a new transport.  Would
  need a `DEB_DAP` opt-in versus the legacy gdb text backend for `.c`/`.cpp`
  (those extensions already route to the text backend today).  Once opted in it
  is two `DAP_LANGS` rows (gdb + lldb-dap alternative), like Rust.
* **DAP *server* mode** (task #50, deferred to v2.0) — expose xwpe itself as a
  debuggee target, so other DAP clients (VS Code, nvim-dap) can drive an xwpe
  debug session.
