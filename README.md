<p align="center">
  <img src="icons/xwpe.svg" width="128" alt="xwpe icon">
</p>

# xwpe

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

The X11 libraries (`libxft-dev`, `libcairo2-dev`, `libpango1.0-dev`)
enable the anti-aliased Xft/Cairo rendering used by `xwpe`. They are
optional: `configure` detects them and falls back automatically, so a
console-only build needs only `libncurses-dev` (plus the build tools).
`texinfo` is needed to build the `info xwpe` manual.

## What's new in 1.6.5

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
  dbx) are untouched.  A modern Scala/macOS user can run **wpe in a terminal**
  (iTerm2/Terminal, mouse and all) as a tiny zero-config debug front-end &mdash;
  no XQuartz needed.

  The engine carries **three transports** behind one API, because each adapter
  dictates its own &mdash; reverse-TCP (Delve is a server; program output read
  from a pty), stdio (gdb/lldb speak DAP on their own pipes; output via DAP
  events), and TCP-client (connect to an endpoint a build server already
  started, as for Scala/Bloop).  The JVM has no standalone DAP server, so xwpe
  runs a short Build Server Protocol handshake against `scala-cli`/Bloop to get
  one &mdash; no Scala artifact ships in xwpe, the toolchain stays external.
  Adding a language is a one-row descriptor; `lldb-dap` for C/C++ is next.

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

## What's new in 1.6.3

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

## What was new in 1.6.2

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

## Building

```sh
autoreconf -fi   # only needed from git
./configure
make
sudo make install
```

Optional:

```sh
./configure --without-x     # terminal-only (no X11)
./configure --without-gpm   # no GPM mouse
```

### External programs

xwpe auto-detects compiler and debugger by file extension:

```sh
sudo apt install gcc g++ gfortran      # C/C++/Fortran
sudo apt install fpc                   # Free Pascal
sudo apt install default-jdk           # Java (javac + jdb)
sudo apt install python3               # Python (py_compile + pdb)
sudo apt install texlive-latex-base    # LaTeX (pdflatex)
sudo apt install perl                  # Perl (perl -c)
sudo apt install gnucobol              # COBOL (cobc)
sudo apt install gdb                   # C/C++/Fortran/Pascal debugger
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
| **Texinfo manual** | `info xwpe` (requires `info` package) | 12 chapters: editor, compiling, debugging, tutorials, reference |
| **Man pages** | `man xwpe` | Command-line options |

## Known limitations

- **X11 clipboard**: internal buffers only. System clipboard
  (PRIMARY/CLIPBOARD) planned for v1.7.
- **Dialog scrollbars**: window scrollbars use Unicode glyphs (1.6.3), but
  the scrollbars drawn inside dialogs are still ASCII. Cosmetic; planned for
  v1.6.4/v1.7.

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
