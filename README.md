<p align="center">
  <img src="icons/xwpe.svg" width="128" alt="xwpe icon">
</p>

# xwpe

> [!WARNING]
> **1.6.5 is in development &mdash; not yet tagged, experimental.** It is under
> heavy testing (no release candidate yet). The LSP client (language-server
> features) in particular is new and changing fast. Expect rough edges; please
> try it and report what breaks &mdash; **testers welcome.** For a stable build,
> use the latest tagged release (**1.6.4**) or the Debian package.

Xwpe is a programming editor and IDE for UNIX terminals and X11,
inspired by the Borland C and Pascal family. It provides syntax
highlighting, integrated compiler and debugger interface, project
management, and a function-key driven menu system. Emacs cursor keys
(Ctrl-P/N/F/B/A/E) are built in.

The **1.6.x series** brought xwpe from its 1993 origins to 2026:
UTF-8 terminal support, working compilers and debuggers for 12 languages,
a **Debug Adapter Protocol client** (the same protocol VS Code and Neovim
use) so modern debuggers plug straight in, mouse support, X11 fixes, and a
12-chapter Texinfo manual.

<p align="center">
  <img src="screenshots/xwpe-screenshot.png" width="720" alt="xwpe 1.6.x: Cairo rendering, Unicode borders, color emoji">
</p>

<p align="center">
  <img src="screenshots/xwpe-xwayland.png" width="640" alt="xwpe on a Wayland desktop via XWayland, showing Xft color emoji and UTF-8 (Cyrillic, CJK, Greek)">
  <br><em>The same X11 build on a Wayland desktop, via XWayland &mdash; Xft color emoji and full UTF-8 (Cyrillic, CJK, Greek).</em>
</p>

<details><summary>Compare with xwpe 1.5 (2006)</summary>
<p align="center">
  <img src="screenshots/xwpe-screenshot-1.5.png" width="720" alt="xwpe 1.5: bitmap font, ASCII borders">
</p>
</details>

## Quick start

```sh
# Install dependencies (Debian/Ubuntu)
sudo apt install build-essential autoconf automake pkg-config texinfo \
  libncurses-dev libx11-dev libxft-dev libcairo2-dev libpango1.0-dev \
  libvterm-dev libjson-c-dev libgpm-dev zlib1g-dev librsvg2-bin

# Build and install
autoreconf -fi && ./configure && make && sudo make install

# Run
wpe file.c          # terminal mode
xwpe file.c         # X11 mode
```

That is the Debian/Ubuntu fast path. For a generic-Linux dependency list, the
**macOS** (Homebrew) build, the optional `configure` flags, and what
`make install` provides, see [**Building & installing**](#building--installing)
below. (`make install` is what enables full syntax highlighting and the in-app
Help -- skip it and only C/C++ highlight.)

## What's new in 1.6.5 (in development, not yet tagged)

xwpe plugs into the modern toolchain through the same standard protocols other
editors use &mdash; a real source-level debugger (DAP) and IDE navigation (LSP)
&mdash; while staying a few-megabyte terminal program: the discoverable Borland
UX, mouse and all, zero per-project config, and no X server or heavy GUI runtime
to install.  It runs the same over SSH, on a plain console, or in any terminal
emulator.

* **Debug Adapter Protocol (DAP) client &mdash; modern debuggers, no bespoke
  backend.** xwpe now speaks DAP, the wire protocol behind VS Code, Neovim
  and Emacs debugging.  Three languages are wired: **Go via Delve** (`dlv dap`),
  **Rust via gdb** (`gdb --interpreter=dap` &mdash; gdb is a first-class Rust
  debugger), and **Scala via Bloop** (the Scala Center `scala-debug-adapter`,
  reached through `scala-cli`).  Open a `.go`, `.rs` or `.scala` file, set a
  breakpoint, and Ctrl-G R drives a real source-level debug session &mdash;
  Run/Continue (Ctrl-G R), Step Over (F8), Step Into (F7), live watches
  (Ctrl-G W), and program output in Messages, the same keys you already use for
  gdb.  This is xwpe's seventh debugger backend (`DEB_DAP`), selected
  automatically by extension; the six text backends (gdb, jdb, pdb, a68g, sdb,
  dbx) are untouched.

  The engine carries **three transports** behind one API, because each adapter
  dictates its own &mdash; reverse-TCP (Delve is a server; program output read
  from a pty), stdio (gdb/lldb speak DAP on their own pipes; output via DAP
  events), and TCP-client (connect to an endpoint a build server already
  started, as for Scala/Bloop).  The JVM has no standalone DAP server, so xwpe
  runs a short Build Server Protocol handshake against `scala-cli`/Bloop to get
  one &mdash; no Scala artifact ships in xwpe, the toolchain stays external.
  Adding a language is a one-row descriptor; `lldb-dap` for C/C++ is next.

* **Language Server Protocol (LSP) client &mdash; an IDE, not just a debugger.**
  Where DAP lets xwpe *run* your code, LSP lets it *understand* it: xwpe speaks
  LSP to the same servers VS Code / Neovim / Emacs use. **Scala via Metals** is
  wired &mdash; open a `.scala` file and the **Alt-Q** prefix gives the IDE
  staples: **E** diagnostics (compiler errors in Messages, live as you type),
  **D** definition, **I** implementation, **T** type-definition, **H** hover
  (type + docs), **C** completion, **R** references, **O** file outline, **W**
  project-wide symbol search, **A** code actions / quick-fixes, **S** signature
  help, **N** rename, **F** format (scalafmt, whole-file or just the marked
  range), **B**/**G** call hierarchy (incoming / outgoing), **K**/**J** type
  hierarchy (super / sub), **V** expand selection by syntax, **U** highlight
  every use, **L** code lenses, **Y** inlay hints (inferred types) and **M**
  semantic colours (compiler-driven highlighting). `Alt-Q ?` opens a menu of
  them all. **C and C++ are wired too, via [clangd](https://clangd.llvm.org/)**
  (`apt install clangd`) &mdash; the same keys, the same engine, no JVM and no
  build-server wait, so it is ready in seconds and `Alt-Q D` follows straight
  into the system headers (opened read-only). **Python is wired via
  [pyright](https://github.com/microsoft/pyright) or
  [pylsp](https://github.com/python-lsp/python-lsp-server)** &mdash; xwpe uses
  whichever is on `PATH` (prefers pyright, the VS Code engine; falls back to the
  Debian-native `python3-pylsp`). **Go** ([gopls](https://pkg.go.dev/golang.org/x/tools/gopls),
  `apt install gopls`) and **Rust**
  ([rust-analyzer](https://rust-analyzer.github.io/), `apt install rust-analyzer`)
  are wired too. The engine (`we_lsp.c`) reuses the DAP JSON-RPC framing (no new
  dependency) and is integration-tested against a real Metals, clangd, Python
  server, gopls *and* rust-analyzer &mdash; **five languages, one engine**; adding
  a server is a one-row descriptor, not new plumbing.

  **Try it in your language.** Each has a small, fully-commented demo project
  that exercises every `Alt-Q` action in that language's own idioms &mdash; open
  it and read down the code &mdash; plus a **captioned tour GIF** that walks the
  breadth of LSP (hover, inlay hints, highlight-all-uses, references, outline)
  and ends on a **rename refactor with Undo**:

  | Demo (open with `wpe`) | Language | Server | Tour |
  |---|---|---|---|
  | [`docs/examples/scala-lsp/`](docs/examples/scala-lsp/)   | Scala  | Metals          | [tour](docs/demos/gifs/scala/tour.gif) |
  | [`docs/examples/c-lsp/`](docs/examples/c-lsp/)           | C/C++  | clangd          | [tour](docs/demos/gifs/c/tour.gif) |
  | [`docs/examples/python-lsp/`](docs/examples/python-lsp/) | Python | pyright / pylsp | [tour](docs/demos/gifs/python/tour.gif) |
  | [`docs/examples/go-lsp/`](docs/examples/go-lsp/)         | Go     | gopls           | [tour](docs/demos/gifs/go/tour.gif) |
  | [`docs/examples/rust-lsp/`](docs/examples/rust-lsp/)     | Rust   | rust-analyzer   | [tour](docs/demos/gifs/rust/tour.gif) |

  **Prerequisites (Debian/Ubuntu):** `cs install metals scala-cli`, **plus an LTS
  JDK (17 or 21) that *Metals* runs on**: `sudo apt install openjdk-21-jdk` then
  `export JAVA_HOME=/usr/lib/jvm/java-21-openjdk-amd64`. The subtle part: `//>
  using jvm temurin:21` in `project.scala` pins only the *build* JVM &mdash;
  Metals' own presentation compiler (hover, completion, go-to-definition) runs on
  the JVM `JAVA_HOME` points at, so it must be a JDK 17/21. On a too-new default
  JDK (e.g. OpenJDK 26) the Scala 3 presentation compiler crashes (`asTerm called
  on not-a-Term`) and navigation/hover silently return empty. *(macOS: the same
  rule applies &mdash; see the [macOS](#macos-terminal-wpe-via-homebrew) build
  section for the Homebrew equivalents.)* See
  **[`docs/LSP.md`](docs/LSP.md)** for the feature guide (every `Alt-Q` action,
  with clips), or the **Language servers** chapter of the manual (`info xwpe`,
  or Help&nbsp;&rarr;&nbsp;Info) for the full reference.

  <p align="center">
    <img src="docs/demos/gifs/c/tour.gif" width="760" alt="A tour of xwpe's LSP features on a C/C++ file via clangd: hover shows the signature, Alt-Q Y reveals inferred inlay-hint types end-of-line, Alt-Q U highlights every use of the symbol, references list in the Messages window, the file outline pops up, and finally Alt-Q N renames total to tally across the whole file -- then Ctrl-U undoes the entire refactor. The pressed keys are captioned along the bottom.">
    <br><em>One tour, the breadth of LSP &mdash; hover, inlay hints, highlight-all-uses, references, outline, and a <strong>rename refactor with Undo</strong> &mdash; here in C via clangd. Every wired language has its own captioned tour: <a href="docs/demos/gifs/scala/tour.gif">Scala</a> &middot; <a href="docs/demos/gifs/python/tour.gif">Python</a> &middot; <a href="docs/demos/gifs/go/tour.gif">Go</a> &middot; <a href="docs/demos/gifs/rust/tour.gif">Rust</a> &middot; <a href="docs/demos/gifs/c/tour.gif">C/C++</a>.</em>
  </p>

  <p align="center">
    <img src="docs/demos/gifs/menu.gif" width="540" alt="Pressing Alt-Q ? in xwpe opens the Metals action menu, an upward-unfolding Borland-style dropdown listing every LSP command with its Alt-Q accelerator.">
    <br><em>And it stays discoverable: <code>Alt-Q ?</code> unfolds the full action menu, every command with its accelerator. More clips in <a href="docs/demos/">docs/demos/</a>.</em>
  </p>

  <p align="center">
    <img src="screenshots/xwpe-go-dap-debug.png" width="720" alt="Debugging a Go program in xwpe via Delve over DAP: the editor stopped at a breakpoint on line 9 (highlighted), and a Watches window below showing the live value fact: 6 as the factorial loop runs.">
    <br><em>Debugging Go through Delve/DAP: stopped at a breakpoint, with a live watch (<code>fact</code>) updating as the loop runs.</em>
  </p>

  It "just works" the way a Borland IDE should: program output (not the
  adapter's own chatter) appears in Messages; stepping off the end of `main`
  cleanly reports <em>Program exited</em> instead of wandering into the Go
  runtime; and Go always uses Delve, never gdb (which is unreliable for Go's
  goroutine runtime).  Requires `dlv` and `go`, plus a `go.mod` in the source
  directory (Delve builds the package).  Needs the new `libjson-c-dev` build
  dependency.

* **GNU Algol 68 (ga68)** joins the classic `a68g` interpreter: native compile
  + gdb source-level debugging, with both stropping dialects highlighted.

<details>
<summary><strong>Earlier releases &mdash; what was new in 1.6.3 and 1.6.2</strong> (click to expand; see <code>CHANGELOG</code> for the full history)</summary>

### What's new in 1.6.3

* **No more scrollbar bleed with overlapping windows** (X11): with three
  windows stacked so one is covered by two others at once, a covered
  window's scrollbar no longer shows through.  The fluid-scrollbar chrome
  now clips each window to its true visible region with cairo_region_t set
  algebra and indexes windows by z-level, matching the cell compositor.
* **Debugger memory leak fixed** (the 24-year "location unknown" leak):
  each window's breakpoint-line and syntax-state arrays were freed with
  neither at window close; both are now released.  Verified with a full
  valgrind sweep over gdb, pdb and jdb -- 0 bytes definitely lost.
* **Event-driven interactive debugging**: when the debugged program
  blocks on fgets/scanf, type input directly in the Messages window.
  Architecture: poll()-based fd multiplexing of X11 + gdb pipe + pty
  (same pattern as st, cgdb, foot terminals).  Backspace works.
* **Dead key compose**: accented characters (e, n, u, a, etc.) via
  dead keys in the editor, debug console, and dialog text fields.
  Fallback compose table when XIM does not compose natively.
* **UTF-8 in dialog fields**: Search, Replace, Compiler Options and
  all other dialogs accept and render accented characters correctly.
  UTF-8-aware cursor movement, backspace, and delete.
* **Runs on Wayland** via XWayland (verified under a headless KWin
  session, see the screenshot above).  xwpe is a plain Xlib app, so
  Debian's GNOME/KDE Wayland sessions run it through XWayland with no
  changes.  A native `wl_surface` backend is on the roadmap for 1.7.
* printf without \n visible after each step (fflush via gdb)
* Perl and COBOL compiler support (perl -c, cobc)
* Cursor-relative error navigation with wrap-around (Alt-T/Alt-V)
* Dialog usability: Tab/Shift-Tab navigation, radio buttons, colors
* **Borland-faithful popups**: message and dialog boxes drop the (inert)
  maximize box -- Borland popups never had one -- and keep a single working
  close box; click [X] to dismiss, exactly like Esc
* **Per-menu test coverage** for every menu section in both front-ends
  (pyte for `wpe`, headless Xft for `xwpe`), plus the contextual status bar
* **ncurses mouse drag**: window move/resize via title bar and borders
  (xterm mode 1002, same protocol as Midnight Commander)
* **Full mouse on the bare Linux console (GPM)**: pointer, click, window
  drag and resize with no X -- the GPM connection is pumped through the
  event loop (regression fixed)
* **Borland "User Screen" (Alt-F5)** restored on the console: leave the
  editor and see a program's own full screen (ANSI colour, cursor
  positioning, a TUI) verbatim -- what the Messages window cannot show.
  Try it with `tests/inputs/paint.c` (F9, Ctrl-F9, then Alt-F5).
* **Dialog resize safety**: survives extreme terminal shrink without
  crash; dialog clipped and restored cleanly on grow
* **Icon set**: two-tier SVG icon, .desktop entry, _NET_WM_ICON for
  window manager title bar and taskbar
* Modernized color palette for dialogs and syntax highlighting
* **Emoji in terminal mode**: ncurses rendering of emoji codepoints
  (U+1F389 etc.) in editor and Messages. Works in kitty, gnome-terminal.
* **Ctrl-F9 Run via pty**: interactive program I/O in Messages window,
  no xterm dependency, no screen switching
* **kitty mouse fix**: compatible mouse tracking without SGR conflicts

### What was new in 1.6.2

* **Xft font rendering in X11**: anti-aliased TrueType fonts with
  fontconfig fallback. Color emoji via Noto Color Emoji (libXft 2.3.5+
  BGRA). Replaces the 1993 XDrawImageString bitmap rendering.
* **Pixmap double-buffering**: zero flicker on resize and repaints.
* **Full UTF-8 in X11**: accents, Cyrillic, CJK, and emoji with
  CELL_WIDE support (cursor, delete, select all work correctly on
  wide characters).
* 12 compilers: gcc, g++, gfortran, fpc, javac, python3, pdflatex, perl, cobc, a68g, go, rustc
* 5 debuggers: gdb, jdb (Java), pdb (Python), a68g (Algol 68), and a DAP
  client (Go via Delve, Rust via gdb) -- all with F8 stepping and live watches
* Program output in Messages buffer (Ctrl-G P) -- no terminal switching
* Mouse in terminal emulators (xterm protocol) and Linux console (GPM)
* 33-year-old Redo crash fixed, 30-year-old pipe leak fixed
* Automated tests: pyte (wpe/ncurses) + headless X11 GUI suite (xwpe/Xft)

See `CHANGELOG` for full details.

</details>

## Compiler support

| Compiler | Language | F9 | Error nav | Extensions |
|----------|----------|:--:|:---------:|------------|
| gcc      | C        | ok | ok | `.c` |
| g++      | C++      | ok | ok | `.cpp` `.cc` `.C` `.cxx` |
| gfortran | Fortran  | ok | ok | `.f` `.f90` `.f95` `.f03` `.f08` |
| fpc      | Pascal   | ok | ok | `.p` `.pas` `.pp` |
| javac    | Java     | ok | ok | `.java` |
| python3  | Python   | ok | ok | `.py` |
| pdflatex | LaTeX    | ok | ok | `.tex` |
| perl     | Perl     | ok | ok | `.pl` `.pm` |
| cobc     | COBOL    | ok | ok | `.cob` `.cbl` |
| a68g     | Algol 68 | ok | line | `.a68` `.alg` |
| go       | Go       | ok | ok | `.go` |
| rustc    | Rust     | ok | ok | `.rs` |

Any compiler that emits `file:line:column: message` diagnostics (clang,
rustc, go build, dmd, ghc, nim, ...) works with the default GNU pattern.
Custom formats are configurable via Options -> Programming using xwpe's
pattern language (`${FILE}`, `${LINE}`, `${COLUMN}`, wildcards).

## Debugger support

| Debugger | Language | Start | Step | Output | Auto-select |
|----------|----------|:-----:|:----:|:------:|-------------|
| gdb      | C/C++/Fortran/Pascal | Ctrl-G R | F8 | Ctrl-G P | `.c` `.cpp` `.f90` `.p` |
| jdb      | Java     | Ctrl-G R | F8 | Ctrl-G P | `.java` |
| pdb      | Python   | Ctrl-G R | F8 | Ctrl-G P | `.py` |
| a68g     | Algol 68 | Ctrl-G R | F8 | Ctrl-G P | `.a68` `.alg` |
| DAP (Delve) | Go   | Ctrl-G R | F8/F7 | Messages | `.go` |
| DAP (gdb)   | Rust | Ctrl-G R | F8/F7 | Messages | `.rs` |

The Go and Rust rows use the Debug Adapter Protocol -- the same wire protocol
VS Code, Neovim and Emacs DAP use -- so the debugger is a standard adapter
(`dlv dap` for Go; `gdb --interpreter=dap` or `lldb-dap` for Rust) rather than a
bespoke backend.  Go needs `dlv` + `go` and a `go.mod`; Rust needs `rustc` and
either `gdb` (default) or `lldb-dap` (used automatically where gdb is absent,
e.g. macOS; force it with `XWPE_DAP_ADAPTER=lldb`).  Scala (Metals) and C/C++
are planned on the same engine.

<p align="center">
  <img src="screenshots/xwpe-ga68-watch.gif" width="720" alt="Debugging a GNU Algol 68 (ga68) program in xwpe: Ctrl-G R compiles with ga68 and starts gdb, Ctrl-G W adds a watch on a variable, Window/Size-Move tiles the editor, Watches and Messages windows, and F8 single-steps while the watch value grows. The pressed keys are overlaid in the corner.">
  <br><em>ga68 + gdb: stepping a factorial with a live watch (<code>fact</code>: 1 &rarr; 2 &rarr; 6 &rarr; 24 &rarr; 120).</em>
</p>

Algol 68 has two toolchains, and xwpe debugs each `.a68`/`.alg` file with the
one that matches its dialect (detected from the file's content): **a68g** (the
Algol 68 Genie interpreter) with its `--monitor`, or **ga68** (the GCC Algol 68
front-end) with **gdb** on the compiled native binary, breaking at
`__algol68_main` instead of `main`.

a68g uses its built-in `--monitor` (a full gdb-class source-level debugger):
breakpoints, run/continue from a68g's automatic break at line 1, F8 step
(line-granular over interruptable units), watches (`evaluate EXPR`), and a
call stack -- the same Borland keys as the other backends.

Program output is captured in the Messages buffer (same window as
compiler errors). Ctrl-G P shows output with full scroll at any time.

## Building & installing

`sudo make install` is part of the normal build, not an afterthought: it
installs `syntax_def` (the syntax-highlighting rules), the in-app help, the
option file and the man page. **Skip it and you get only the built-in C/C++
highlighting and no Help.** To run straight from the build directory without
installing, copy the rules file into your home config:
`mkdir -p ~/.xwpe && cp syntax_def ~/.xwpe/`.

### Linux (any distribution, from source)

Needs a C compiler, autotools and pkg-config, plus **ncursesw** (required),
**libvterm** and **json-c**. **X11 + Xft + Cairo + Pango** are optional (only
the graphical `xwpe` uses them); **GPM** is optional (Linux-console mouse).

```sh
autoreconf -fi   # only from a git checkout
./configure      # --without-x = terminal-only;  --without-gpm = no GPM mouse
make
sudo make install
```

### Debian / Ubuntu

```sh
sudo apt install build-essential autoconf automake pkg-config texinfo \
  libncurses-dev libx11-dev libxft-dev libcairo2-dev libpango1.0-dev \
  libvterm-dev libjson-c-dev libgpm-dev zlib1g-dev librsvg2-bin
autoreconf -fi && ./configure && make && sudo make install
```

The X11 libraries enable `xwpe`'s anti-aliased Xft/Cairo rendering; for a
console-only build drop them and pass `--without-x` (only `libncurses-dev` plus
the build tools are then required). `texinfo` builds the `info xwpe` manual.

### macOS (terminal `wpe`, via Homebrew)

> **Untested on macOS so far.** The build is written to be portable (openpty's
> header is selected per platform; GPM is Linux-only and skipped), but no one
> has yet confirmed a clean macOS build -- treat the steps below as a starting
> point and please [open an issue](https://codeberg.org/mendezr/xwpe/issues)
> with what you hit, good or bad.
>
> On macOS the console editor `wpe` runs natively in Terminal.app or iTerm2 --
> no X server needed. (The graphical `xwpe` would require XQuartz, and in this
> release it looks and feels much like the terminal build, so `wpe` is the
> recommended way to try xwpe on a Mac.)

```sh
# 1. Build tools + libraries (Apple's clang from `xcode-select --install` works)
brew install autoconf automake pkg-config ncurses libvterm json-c texinfo

# 2. Help the configure script find Homebrew's keg-only ncurses
export PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig:$PKG_CONFIG_PATH"

# 3. Build the terminal editor (no X11)
autoreconf -fi
./configure --without-x --without-gpm
make

# 4. Run it -- `make` creates the ./wpe symlink for you (programming mode)
./wpe            # or: sudo make install   then   wpe
```

`make` builds the binary as `we` and symlinks `./wpe` to it (and `./xwpe`,
`./xwe` on X11 builds) -- the **mode is chosen by the name**, so launch it as
`./wpe` to get the programming layer (Alt-Q LSP, F9 compile, Ctrl-G debug);
`./we` is the plain editor.

**Syntax highlighting and Help need the data files.** Run `sudo make install`,
or (to stay in the build dir) `mkdir -p ~/.xwpe && cp syntax_def ~/.xwpe/` --
without it only C/C++ highlight and `Alt-Q`/Help have no content.

**Make the terminal send `Alt`.** xwpe's menus and the whole `Alt-Q` LSP layer
need the `Option`/`Alt` key to send a Meta/`Esc` prefix, which **Terminal.app
does not do by default** -- so `Alt-Q` etc. appear to do nothing. Fix it once:

- **Terminal.app:** Settings -> Profiles -> Keyboard -> tick *"Use Option as
  Meta key"*.
- **iTerm2:** Settings -> Profiles -> Keys -> set *Left Option key* to *Esc+*.
- Or use a terminal that does this out of the box (**kitty**, **WezTerm**,
  **Alacritty**). iTerm2/kitty are the smoothest for xwpe on macOS.

GPM is Linux-only, so `--without-gpm` is required (mouse still works through the
terminal via ncurses). `openpty()` (used by the debugger and Run panes) comes
from the macOS system library; `configure` finds it automatically.

The compilers, debugger and language servers are optional and detected on
`PATH` at runtime (see "External programs" below for what each enables). The
Homebrew equivalents of the Debian packages:

```sh
# clang/clang++ ship with the Xcode Command Line Tools (xcode-select --install)
brew install llvm            # clangd  -> C/C++ LSP (Alt-Q): the headline feature
brew install gopls           # Go LSP        (also: brew install go)
brew install rust-analyzer   # Rust LSP      (also: rustup for rustc/cargo)
brew install pyright         # Python LSP    (Python 3 ships with macOS; pdb is built in)
brew install gdb             # debugger (Ctrl-G); on Apple Silicon lldb via DAP is the
                             # better fit -- xwpe auto-falls-back to lldb there
```

For Scala (Metals), Homebrew installs Coursier as `coursier` (not `cs`), and
Metals needs an LTS JDK (17 or 21):

```sh
brew install coursier openjdk@21
coursier install metals scala-cli      # the `cs` name only exists with the standalone installer
export JAVA_HOME="$(brew --prefix openjdk@21)/libexec/openjdk.jdk/Contents/Home"
```

The JDK matters: Metals' presentation compiler (hover, completion,
go-to-definition) runs on the JVM `JAVA_HOME` points at -- on a too-new default
JDK the Scala 3 presentation compiler crashes (`asTerm called on not-a-Term`)
and navigation/hover silently return empty, so keep `JAVA_HOME` on the 17/21 JDK
above. (`//> using jvm temurin:21` in `project.scala` pins only the *build* JVM,
not Metals' own.)

**Put the servers on `PATH` -- Homebrew does not always.** xwpe finds each server
by name on `PATH`; the keg-only and Coursier ones are not there by default:

```sh
export PATH="$(brew --prefix llvm)/bin:$PATH"                  # clangd (llvm is keg-only)
export PATH="$PATH:$HOME/Library/Application Support/Coursier/bin"  # metals, scala-cli
which clangd rust-analyzer metals scala-cli                    # confirm before launching
```

Then open one of the bundled demo projects **as `wpe`** (programming mode) and
press `Alt-Q E` to start the server (`Alt-Q ?` lists every action):

```sh
./wpe docs/examples/c-lsp/main.cpp       # clangd -- ready in ~2s (compile_flags.txt is included)
./wpe docs/examples/rust-lsp/src/main.rs # rust-analyzer (the dir has Cargo.toml)
./wpe docs/examples/scala-lsp/main.scala # Metals -- FIRST start downloads + indexes (minutes)
```

Each `docs/examples/*-lsp/` directory has a `README.md` walking through every
`Alt-Q` action on that file.

### External programs

xwpe auto-detects the compiler, debugger and language server by file extension.
From the Debian/Ubuntu archive:

```sh
sudo apt install gcc g++ gfortran      # C/C++/Fortran
sudo apt install fpc                   # Free Pascal
sudo apt install default-jdk           # Java (javac + jdb)
sudo apt install python3               # Python (py_compile + pdb)
sudo apt install texlive-latex-base    # LaTeX (pdflatex)
sudo apt install perl                  # Perl (perl -c)
sudo apt install gnucobol              # COBOL (cobc)
sudo apt install algol68g              # Algol 68 (a68g + its monitor debugger)
sudo apt install golang-go             # Go (compile)
sudo apt install rustc                 # Rust (rustc -g)
sudo apt install gdb                   # C/C++/Fortran/Pascal/Rust debugger
```

Modern IDE/debug toolchains that are not in the archive:

```sh
# Scala -- IDE features (LSP via Metals) and debugging (DAP via Bloop):
cs install scala-cli metals            # coursier; https://get-coursier.io
sudo apt install openjdk-21-jdk        # an LTS JDK Metals runs on (see the manual)
# Go -- source-level debugging (DAP via Delve):
go install github.com/go-delve/delve/cmd/dlv@latest
```

### Console tips

On a Linux console (Ctrl+Alt+F2), bitmap fonts look tiny on HiDPI:

```sh
sudo apt install console-terminus
setfont Lat15-Terminus32x16            # readable on HiDPI
```

Mouse on console requires GPM (`sudo apt install gpm`): with the daemon
running you get the pointer, clicks, and window drag/resize on a bare VT,
no X needed. In terminal emulators (xterm, kitty, gnome-terminal, tmux),
mouse works natively.

## Syntax highlighting

Ships with C, C++, Fortran, Pascal, Java, Python, LaTeX, Perl, COBOL. Install with
`sudo make install` or copy `syntax_def` to `~/.xwpe/syntax_def`.

The format is documented in `docs/chapters/configuration.texi`. Adding
a language requires listing keywords, operators, and comment delimiters.

## Documentation

| Level | Access | Content |
|-------|--------|---------|
| **In-app help** | F1 | Menus, key bindings, basic usage |
| **Texinfo manual** | `info xwpe` (requires `info` package) | 13 chapters: editor, compiling, debugging, language servers, tutorials, reference |
| **Man pages** | `man xwpe` | Command-line options |
| **LSP / IDE guide** | [`docs/LSP.md`](docs/LSP.md) | The `Alt-Q` actions, per-language setup, read-only stdlib |
| **Try-it demos** | [`docs/examples/`](docs/examples/) | Runnable, commented testbeds (one per language) + compiler/debugger examples |
| **Demo GIFs** | [`docs/demos/`](docs/demos/) | Recorded feature clips and per-language tours |
| **For contributors** | [`HACKING.md`](HACKING.md), [`HACKING-LSP.md`](HACKING-LSP.md), [`HACKING-DAP.md`](HACKING-DAP.md) | Architecture of the editor, the LSP client, the DAP debugger |

## Known limitations

- **X11 clipboard**: internal buffers only. System clipboard
  (PRIMARY/CLIPBOARD) planned for v1.7.
- **Language server, one document at a time**: the LSP server attaches to a
  single open file; using an Alt-Q action in a different file transparently
  re-points it (a brief pause). Rename (Alt-Q N) applies to the current file
  only; edits it would make in other files are reported, not applied.
- **Dialog scrollbars**: window scrollbars use Unicode glyphs (1.6.3), but
  the scrollbars drawn inside dialogs are still ASCII. Cosmetic; planned for
  v1.7.

## Contributing

Issues, bug reports, and patches welcome on Codeberg:
https://codeberg.org/mendezr/xwpe

**We especially need testers.** xwpe was untested for nearly 20 years.
If you can try it -- any terminal, any compiler, any workflow -- open
an issue even if everything works.

See [tests/README.md](tests/README.md) for the automated test suite and the
full dependency list.  `tests/run-tests.sh` runs everything: the C unit tests,
the pyte (ncurses, `wpe`) suite, and the headless X11 GUI (`xwpe`) suite.  The
X11 step self-skips when its stack is absent (`xvfb matchbox-window-manager
xdotool x11-utils imagemagick python3-pil`); `--x11` runs just that layer.

## Project history

- **Fred Kruse** -- original author (1993, last release 1.4.2 ca. 1997)
- **Dennis Payne** -- continuation (1.5.x, 1997-2006), with
  contributions from ~25 developers (see CHANGELOG):
  https://www.identicalsoftware.com/xwpe/
- **Debian contributors** (Jari Aalto, Francesco P. Lovergine, Andreas
  Tille, et al.) -- patches now integrated upstream
- **Juan Manuel Mendez Rey** -- current maintainer (2026-),
  with Dennis Payne's blessing

Historical archive: https://codeberg.org/mendezr/xwpe-archives

## Architecture

For contributors and porters: see [HACKING.md](HACKING.md) for
internal architecture (SCREENCELL buffer model, double-buffer rendering,
popup save/restore, X11 extbyte system).

## Licence

GPL-2. See `COPYING`.
