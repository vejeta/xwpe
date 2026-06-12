# IDE features in xwpe: the Language Server (LSP) client

xwpe is a 1993 Borland-style console editor that learned a 2020s trick: it
speaks the **Language Server Protocol**, the same protocol VS Code, Neovim and
Emacs use. Open a source file and the **`Alt-Q`** prefix gives you the IDE
staples -- diagnostics, go-to-definition, hover, completion, rename, format,
call/type hierarchies, semantic highlighting -- driven by the *real* language
server for that language. The server is an external program; nothing of it
ships inside xwpe, so the editor binary stays a few megabytes.

The first wired language is **Scala, via [Metals](https://scalameta.org/metals/)**.
A Scala developer can use `wpe` in a plain terminal as a tiny, zero-config IDE
front-end -- no XQuartz, no Electron. **C and C++** (via
[clangd](https://clangd.llvm.org/), [see below](#c-and-c-clangd)), **Python**
(via pyright or pylsp, [see below](#python-pyright-or-pylsp)), **Go** (via gopls,
[see below](#go-gopls)) and **Rust** (via rust-analyzer,
[see below](#rust-rust-analyzer)) are wired too -- the same keys, the same engine.

<p align="center">
  <img src="demos/gifs/menu.gif" width="640"
       alt="Pressing Alt-Q ? in xwpe opens the Metals action menu: an upward-unfolding Borland-style dropdown listing every LSP command with its Alt-Q accelerator.">
  <br><em>The Metals action menu (<code>Alt-Q ?</code>): every command, discoverable, with its accelerator.</em>
</p>

---

## Quick start

You need three things on your `PATH` (Linux or macOS):

```sh
cs install metals scala-cli          # coursier -- https://get-coursier.io
sudo apt install openjdk-21-jdk      # an LTS JDK that *Metals itself* runs on
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64
```

Then open a Scala file and use the `Alt-Q` keys:

```sh
cd docs/examples/scala-lsp          # the bundled, fully-commented demo project
wpe main.scala
```

The **first** action starts Metals (a JVM boots and imports the build, ~1 min
cold). The editor **stays responsive** the whole time -- keep typing, scrolling,
opening menus. When Messages shows `Language server ready`, every action is
instant.

> **The JDK gotcha (read this if hover/definition come back empty):**
> `//> using jvm temurin:21` in `project.scala` pins only the *build* JVM.
> Metals' own presentation compiler -- which powers hover, completion and
> go-to-definition -- runs on **Metals' JVM**, i.e. whatever `JAVA_HOME` points
> at. On a too-new default (e.g. OpenJDK 26) the Scala 3 presentation compiler
> crashes (`asTerm called on not-a-Term`) and navigation silently returns empty.
> xwpe auto-pins `JAVA_HOME` to an installed LTS when the default is too new and
> tells you in Messages; set it yourself if none is installed. Full notes in the
> **Language servers** chapter of the manual (`info xwpe`, or **Help -> Info**).

---

## The `Alt-Q` actions

`Alt-Q` is the *Query the language server* prefix (chosen so it never collides
with `Alt-M` = Make). Two ways to drive it, Borland-style:

- **`Alt-Q` + letter** runs an action *directly* -- no menu drawn, no flicker
  (e.g. `Alt-Q D` jumps to the definition at once).
- **`Alt-Q ?`** (or `F1`, or a click on the status-bar entry) opens the drop-up
  **menu** -- arrows move, the letter or a click picks, `ESC` closes.

The status-bar entry names the server backing the active file -- `Alt-Q ? Metals`
for Scala, `Alt-Q ? clangd` for C/C++ -- and appears only while a server-backed
file is the active window.

| Key | Action | What it does |
|-----|--------|--------------|
| `Alt-Q E` | **E**rrors / diagnostics | Compiler errors + warnings in Messages, *and* inline (error cells red, warnings amber), live as you type. |
| `Alt-Q D` | **D**efinition | Jump to where the symbol is defined -- including into library/stdlib sources (opened **read-only**, see below). |
| `Alt-Q I` | **I**mplementation | Jump to the concrete override of an abstract `def`/trait member. |
| `Alt-Q T` | **T**ype definition | Jump to the class/trait of a value's *type*. |
| `Alt-Q H` | **H**over | Type + documentation of the symbol, in a popup. |
| `Alt-Q C` | **C**ompletion | Candidates for the word under the cursor; `RET` inserts. |
| `Alt-Q R` | **R**eferences | Every use of the symbol, listed in Messages. |
| `Alt-Q B` | called **B**y (incoming) | Callers of the method under the cursor (call hierarchy, upward). |
| `Alt-Q G` | **G**oes to (outgoing) | Methods the one under the cursor calls (call hierarchy, downward). |
| `Alt-Q K` | supertypes (vi up) | What the type extends/implements -- inheritance upward. |
| `Alt-Q J` | subtypes (vi down) | What extends/implements the type -- inheritance downward. |
| `Alt-Q V` | expand selection (**V**isual) | Grow the block mark by *syntax* (token -> expr -> call -> statement -> block); pairs with `Alt-Q F`. |
| `Alt-Q U` | highlight **U**ses | Mark every occurrence of the symbol in the current file (search by meaning, not text). |
| `Alt-Q O` | **O**utline | The file's symbols in a popup; `RET` jumps. |
| `Alt-Q Y` | inla**Y** hints (toggle) | Inferred `: T` types shown as a distinct grey pill at end-of-line on un-annotated vals. |
| `Alt-Q M` | se**M**antic colours (toggle) | Server-driven highlighting: types, calls, params and vars get distinct colours the regex lexer cannot tell apart. |
| `Alt-Q L` | code **L**enses | The `run`/`debug`/`test` and reference annotations the server attaches to definitions. |
| `Alt-Q W` | **W**orkspace symbol | Project-wide "Go to Symbol": type a query, jump to the match (opening its file). |
| `Alt-Q A` | code **A**ctions | Quick-fixes and refactors at the cursor; applying one rewrites the buffer (and is Undo-able -- see below). |
| `Alt-Q S` | **S**ignature help | The signature of the call the cursor is inside. |
| `Alt-Q N` | re**N**ame | Rename the symbol (current file; reports other files it touches). |
| `Alt-Q F` | **F**ormat | scalafmt -- whole file, or just the marked range (context-sensitive). |

After a code action, rename or format, the buffer is rebuilt and marked
modified; **`F2`** saves.

---

## Highlights

### Go to definition -- including into the standard library

`Alt-Q D` on a call leaps to its `def`. It follows *across files* (opening the
target in a new window) and *into library sources*: Metals extracts those under
`.metals/readonly/` and xwpe opens them **read-only**.

<p align="center">
  <img src="demos/gifs/definition.gif" width="640"
       alt="With the cursor on a describe(...) call, Alt-Q D jumps the cursor up to the def describe that defines it.">
  <br><em>Go to definition (<code>Alt-Q D</code>): the iconic IDE jump, over LSP.</em>
</p>

A read-only window is marked by a **padlock** at the left of the title bar and a
dimmed name -- you can read and copy, not edit. Language-server actions are not
offered inside such a source; run them from your project window.

### Code actions are real edits -- Undo and Redo them

`Alt-Q A` lists the server's quick-fixes and refactors. xwpe applies all three
shapes a server can deliver:

- a **direct edit** (e.g. "Convert to interpolation string"),
- a **server command** -- run over `workspace/executeCommand`, the result
  applied via the `workspace/applyEdit` the server sends back (e.g. "Organize
  imports"),
- an **unresolved action** -- Metals ships its refactors with only a `data`
  field, so xwpe does a `codeAction/resolve` round-trip to fetch the edit first
  (e.g. "Convert to named arguments": `greet("Ada", 42)` ->
  `greet(name = "Ada", age = 42)`).

Whichever shape it is, the rewrite is an **ordinary edit**: a single `Ctrl-U`
undoes it, `Ctrl-R` redoes it -- exactly like a hand edit. The same goes for
Rename (`Alt-Q N`) and Format (`Alt-Q F`).

<p align="center">
  <img src="demos/gifs/codeaction.gif" width="640"
       alt="Alt-Q A on the call greet(quote)Ada(quote), 42) offers Convert to named arguments; xwpe resolves and rewrites it to greet(name = (quote)Ada(quote), age = 42); Ctrl-U undoes the rewrite, Ctrl-R redoes it.">
  <br><em>A Metals refactor applied via <code>codeAction/resolve</code>, then <code>Ctrl-U</code> / <code>Ctrl-R</code>.</em>
</p>

### Semantic colours

`Alt-Q M` repaints the file from Metals' *semantic tokens*: types in cyan,
functions yellow, parameters magenta, variables green -- categories the
regular-expression lexer paints the same. Toggle off to return to the classic
colours.

<p align="center">
  <img src="demos/gifs/semantic.gif" width="640"
       alt="Alt-Q M repaints a Scala file from Metals semantic tokens, giving types, calls, parameters and variables their own colours.">
  <br><em>Semantic colours (<code>Alt-Q M</code>): distinctions the regex lexer cannot make.</em>
</p>

### Worksheets (a REPL in the buffer)

Open a file whose name ends in `.worksheet.sc` and Metals *evaluates* it: the
result of each top-level expression appears dim at end-of-line (`val a = 5 + 7`
grows a faint `: Int = 12`), updating a moment after each edit. No `Eval`
button, no `main` -- the lightweight way to try an API. (A plain `.sc` file is a
scala-cli *script*: you get inferred-type hints but not the `= 12` results.)

### It never freezes

Opening a `.scala`/`.sc` already starts the server in the background, so it is
warming while you read. The whole cold start -- JVM boot, build import, first
compile -- runs without blocking: you can keep typing and using other menus. Set
`XWPE_LSP_NO_EAGER=1` to start it on demand instead (the first `Alt-Q` action).

---

## C and C++ (clangd)

The same `Alt-Q` keys back C and C++, through
[clangd](https://clangd.llvm.org/) -- xwpe's home turf. Install it and open a
source file:

```sh
sudo apt install clangd        # Debian/Ubuntu
wpe main.c                     # or .h .cpp .cc .cxx .hpp ...
```

Unlike Metals there is **no JVM and no build-server import**, so clangd is ready
in *seconds*, and `Alt-Q D` (definition) follows straight into the system
headers under `/usr/include` -- which open **read-only** (the padlock), since
they are not yours to edit. Diagnostics, hover, completion, references, outline,
rename, format and code actions all work exactly as they do for Scala -- it is
the same engine; only the server and the language id differ.

For a **single file** clangd uses sensible default flags. For a **real
project**, give it the compile flags so its diagnostics and navigation match
your build -- either a `compile_commands.json` (from `bear -- make`, or CMake
with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`) or a one-line `compile_flags.txt`
(e.g. `-I./include`) in the project root. clangd finds it by walking up from the
file.

## Python (pyright or pylsp)

Python is wired through **two** servers, and xwpe uses whichever is installed --
it **prefers [pyright](https://github.com/microsoft/pyright)** (Microsoft's type
checker, the engine VS Code uses) and **falls back to
[pylsp](https://github.com/python-lsp/python-lsp-server)** (the Debian-native
`python3-pylsp`). Open a `.py` and the `Alt-Q` keys work; the status bar names
the active one (`Alt-Q ? pyright` or `Alt-Q ? pylsp`).

```sh
pipx install pyright              # preferred: full type inference (not in Debian)
# --- or the Debian-native fallback ---
sudo apt install python3-pylsp python3-pyflakes
```

- **pyright** gives type-aware hover/completion and flags undefined names and
  type errors out of the box. It is a Node program, so it is not in the Debian
  archive -- install it with `pipx`/`pip`/`npm`; xwpe looks for
  `pyright-langserver` on `PATH`.
- **pylsp** (in Debian) does navigation, hover, completion, references and rename
  via the bundled *jedi*. Its **diagnostics need `python3-pyflakes`** installed
  alongside (and `python3-pycodestyle` for style) -- without them it still
  navigates, it just doesn't lint.

With both installed xwpe uses pyright; install only pylsp and it uses pylsp -- no
config, just what's on `PATH`.

## Go (gopls)

Go is wired through [gopls](https://pkg.go.dev/golang.org/x/tools/gopls), the
official server. Install it and open a `.go` file:

```sh
sudo apt install gopls         # or: go install golang.org/x/tools/gopls@latest
```

The `Alt-Q` keys work as everywhere else (the bar shows `Alt-Q ? gopls`). One
Go-specific note: **gopls wants a module** -- work in a directory with a `go.mod`
(`go mod init name` creates one). A lone `.go` file with no module loads in a
degraded ad-hoc mode, so navigation may be limited until the module exists. gopls
is a type checker, so `Alt-Q E` flags undefined names and type errors like the
compiler.

## Rust (rust-analyzer)

Rust is wired through [rust-analyzer](https://rust-analyzer.github.io/), the
official server. Install it and open a `.rs` file:

```sh
sudo apt install rust-analyzer rust-src   # or: rustup component add rust-analyzer rust-src
```

The `Alt-Q` keys work as everywhere else. On the bottom bar the entry is shown
concisely as `Alt-Q ? rust` -- the full `rust-analyzer` is too wide to fit the
80-column button row without crowding out `Alt-X Quit`, so the short form is used
(the menu and this guide keep the full name). Two notes:

- Like gopls, rust-analyzer wants a **Cargo workspace** -- open files inside a
  crate (a directory with a `Cargo.toml`; `cargo new`/`cargo init` make one). It
  reads the standard library too, so install `rust-src` for go-to-definition into
  `std`.
- rust-analyzer **indexes std on first start**, so its cold start is the slowest
  of the wired servers -- the editor stays responsive (the async start), and the
  actions sharpen as the index settles.

`rust-analyzer` is a longer name than the other servers, so the status-bar hint
`Alt-Q ? rust-analyzer` is wider; the button row repacks itself to keep every
entry on screen.

## Bundled demo projects -- one per language

Each wired language has its own tiny, deliberately over-commented testbed under
[`docs/examples/`](examples/), exercising **every** `Alt-Q` action its server
supports.  Each line carries an inline comment telling you which action to try
right there, and an `actions.*` file is a dedicated code-action playground.  Open
the one for *your* language and read down the code -- and watch its tour GIF:

| Demo | Language | Server | Open with |
|------|----------|--------|-----------|
| [`scala-lsp/`](examples/scala-lsp/)   | Scala  | Metals          | `wpe main.scala`  |
| [`c-lsp/`](examples/c-lsp/)           | C/C++  | clangd          | `wpe main.cpp`    |
| [`python-lsp/`](examples/python-lsp/) | Python | pyright / pylsp | `wpe main.py`     |
| [`go-lsp/`](examples/go-lsp/)         | Go     | gopls           | `wpe main.go`     |
| [`rust-lsp/`](examples/rust-lsp/)     | Rust   | rust-analyzer   | `wpe src/main.rs` |

Each folder's `README.md` has the full `Alt-Q` key table, the server's setup, and
a short **tour GIF** (hover, references, outline, go-to-definition) recorded on
that testbed -- see [`docs/demos/`](demos/) for how the GIFs are made.

---

## How the demo GIFs are made

The clips above are **not** hand-captured screen recordings -- they are
*reproducible*, generated from short scripts with
[VHS](https://github.com/charmbracelet/vhs). Each `.tape` in
[`docs/demos/tapes/`](demos/tapes/) is a tiny program: it types keys, waits, and
VHS renders the terminal to a GIF (`ttyd` provides the headless terminal,
`ffmpeg` encodes). Because the keystrokes are scripted, re-recording after a UI
change is one command, and the GIFs never drift from reality by hand.

A neat trick: the server-backed tapes wrap the ~1-minute Metals cold start in a
`Hide` ... `Show` block, so the recording *runs* the real indexing but the GIF
jumps straight to the action. The `codeaction.tape`, for instance, opens
`actions.scala`, starts Metals hidden, then -- on screen -- moves the cursor to
the `greet(...)` call, runs `Alt-Q A`, picks the refactor, and presses `Ctrl-U`
then `Ctrl-R`.

Regenerate them from the repo root (with the editor built and the demo tools on
`PATH`):

```sh
WPE=./wpe DEMO=../scala-demo docs/demos/record.sh            # all tapes
WPE=./wpe DEMO=../scala-demo docs/demos/record.sh codeaction # just one
```

Full details, the tape list, and the key-sequence scripts for every headline
feature are in [`docs/demos/README.md`](demos/README.md) and
[`docs/demos/SCENARIOS.md`](demos/SCENARIOS.md).

---

## See also

- **Language servers** chapter of the manual -- the canonical reference
  (`info xwpe`, the `Language servers` node, or **Help -> Info** inside the
  editor). It covers every action, the JDK requirement, worksheets, the Doctor,
  and the cross-file model in depth.
- [`docs/examples/scala-lsp/`](examples/scala-lsp/) -- the runnable testbed.
- `HACKING-LSP.md` (source tree) -- the engine architecture, for contributors.
