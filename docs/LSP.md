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
front-end -- no XQuartz, no Electron.

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
- **`Alt-Q ?`** (or `F1`, or a click on the `Metals` status-bar entry) opens the
  drop-up **menu** -- arrows move, the letter or a click picks, `ESC` closes.

The `Metals` entry appears on the status bar only while a server-backed file is
the active window.

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
| `Alt-Q Y` | inla**Y** hints (toggle) | Inferred `: T` types shown dim at end-of-line on un-annotated vals. |
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

## The bundled demo project

[`docs/examples/scala-lsp/`](examples/scala-lsp/) is a tiny, deliberately
over-commented Scala 3 project that exercises **every** one of the 22 actions.
Each line carries an inline comment telling you which action to try right there,
and `actions.scala` is a dedicated playground for the three code-action shapes.
Open it with `wpe main.scala` and read down the code. See its
[`README.md`](examples/scala-lsp/README.md) for the full key table.

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
