<p align="center">
  <img src="icons/xwpe.svg" width="128" alt="xwpe icon">
</p>

# xwpe

> [!NOTE]
> The current release is **1.6.6**. The editor, compiler integration and
> debugger are stable; the **LSP client** (language-server features) is newer
> and still maturing, so expect the occasional rough edge there &mdash; please
> report what breaks. **Testers welcome.**

Xwpe is a programming editor and IDE for UNIX terminals and X11,
inspired by the Borland C and Pascal family. It provides syntax
highlighting, integrated compiler and debugger interface, project
management, and a function-key driven menu system. Emacs cursor keys
(Ctrl-P/N/F/B/A/E) are built in.

The **1.6.x series** brought xwpe from its 1993 origins to 2026:
UTF-8 terminal support, working compilers and debuggers for 12 languages,
a **Language Server Protocol client** and a **Debug Adapter Protocol client**
(the same protocols VS Code and Neovim use) so modern language servers and
debuggers plug straight in &mdash; IDE navigation and a real source-level
debugger in a few-megabyte terminal program &mdash; plus mouse support, X11
fixes, and a 13-chapter Texinfo manual.

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

## Install

Pick your platform &mdash; each guide has the package (where one exists yet) and
the from-source steps. Packaging for the major distributions is landing now;
until a package reaches your distro, the from-source build works everywhere.

| Platform | Quickest path today | Full guide |
|----------|---------------------|------------|
| **Debian / Ubuntu** | build from source (a `.deb` is on the way) | [docs/install/debian.md](docs/install/debian.md) |
| **Fedora / RHEL** | build from source (Fedora review in progress) | [docs/install/fedora.md](docs/install/fedora.md) |
| **openSUSE** | Tumbleweed package (landing) &middot; or build | [docs/install/opensuse.md](docs/install/opensuse.md) |
| **Arch** | AUR: `yay -S xwpe` | [docs/install/arch.md](docs/install/arch.md) |
| **macOS** | Homebrew tap (1.6.6) &mdash; see guide | [docs/install/macos.md](docs/install/macos.md) |
| **FreeBSD / OpenBSD / NetBSD** | build from source (ports update on the way) | [docs/install/bsd.md](docs/install/bsd.md) |
| **Any UNIX, from source** | `autoreconf -fi && ./configure && make && sudo make install` | [BUILDING.md](BUILDING.md) |

**Fast path on Debian/Ubuntu** (a complete editor in four lines):

```sh
sudo apt install build-essential autoconf automake pkg-config texinfo \
  libncurses-dev libx11-dev libxft-dev libcairo2-dev libpango1.0-dev \
  libvterm-dev libjson-c-dev libgpm-dev zlib1g-dev librsvg2-bin
autoreconf -fi && ./configure && make && sudo make install
wpe file.c          # terminal mode
xwpe file.c         # X11 mode
```

`make install` is part of the build, not an afterthought: it installs the
syntax-highlighting rules, the in-app Help, the option file and the man page.
**Skip it and you get only built-in C/C++ highlighting and no Help.** Details,
the generic-Linux and macOS builds, `configure` flags and running from the build
tree are in [BUILDING.md](BUILDING.md).

**The `Alt-Q` IDE layer** (LSP/DAP &mdash; compilers, language servers and
debuggers) is set up separately, once the editor builds:
[docs/ide-setup.md](docs/ide-setup.md).

## What it does

A discoverable Borland-style UX (menus, mouse and all), zero per-project config,
no X server or heavy GUI runtime required &mdash; and it runs the same over SSH,
on a plain console, or in any terminal emulator.

- **Editor** &mdash; syntax highlighting, project management, function-key menus,
  Emacs cursor keys, UTF-8 throughout, anti-aliased Xft/Cairo text under X11.
- **Compilers** &mdash; F9 build + error navigation for 12 languages (see the
  table below); any `file:line:col:` diagnostics work.
- **Debuggers** &mdash; source-level stepping and live watches via gdb, jdb, pdb,
  a68g, and a **DAP client** (Go/Rust/Scala) (see the table below).
- **IDE navigation (LSP)** &mdash; hover, go-to-definition, references, rename,
  diagnostics and more via the `Alt-Q` prefix, against the same servers VS Code
  uses: **C/C++ (clangd), Python, Go, Rust, Scala (Metals)** &mdash; one engine,
  five languages. Setup: [docs/ide-setup.md](docs/ide-setup.md); feature guide:
  [docs/LSP.md](docs/LSP.md).
- **Mouse** everywhere &mdash; terminal emulators (xterm protocol), the bare
  Linux console (GPM), and X11. Runs on Wayland via XWayland.

<p align="center">
  <img src="docs/demos/gifs/c/tour.gif" width="760" alt="A tour of xwpe's LSP features on a C/C++ file via clangd: hover shows the signature, Alt-Q Y reveals inferred inlay-hint types, Alt-Q U highlights every use of the symbol, references list in Messages, the file outline pops up, and Alt-Q N renames a symbol across the whole file -- then Ctrl-U undoes the entire refactor.">
  <br><em>One tour, the breadth of LSP &mdash; hover, inlay hints, highlight-all-uses, references, outline, and a <strong>rename refactor with Undo</strong> (here in C via clangd). Per-language tours and more clips in <a href="docs/demos/">docs/demos/</a>.</em>
</p>

<p align="center">
  <img src="screenshots/xwpe-go-dap-debug.png" width="720" alt="Debugging a Go program in xwpe via Delve over DAP: the editor stopped at a breakpoint on line 9 (highlighted), and a Watches window below showing the live value fact: 6 as the factorial loop runs.">
  <br><em>A real source-level debugger: Go through Delve/DAP &mdash; stopped at a breakpoint, with a live watch (<code>fact</code>) updating as the loop runs. More in <a href="docs/ide-setup.md">docs/ide-setup.md</a>.</em>
</p>

For the full release-by-release history (1.6.6, 1.6.5 LSP/DAP, 1.6.3, 1.6.2),
see the [CHANGELOG](CHANGELOG).

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

Any compiler that emits `file:line:column: message` diagnostics (clang, rustc,
go build, dmd, ghc, nim, ...) works with the default GNU pattern. Custom formats
are configurable via Options -> Programming.

## Debugger support

| Debugger | Language | Start | Step | Output | Auto-select |
|----------|----------|:-----:|:----:|:------:|-------------|
| gdb      | C/C++/Fortran/Pascal | Ctrl-G R | F8 | Ctrl-G P | `.c` `.cpp` `.f90` `.p` |
| jdb      | Java     | Ctrl-G R | F8 | Ctrl-G P | `.java` |
| pdb      | Python   | Ctrl-G R | F8 | Ctrl-G P | `.py` |
| a68g     | Algol 68 | Ctrl-G R | F8 | Ctrl-G P | `.a68` `.alg` |
| DAP (Delve) | Go   | Ctrl-G R | F8/F7 | Messages | `.go` |
| DAP (gdb)   | Rust | Ctrl-G R | F8/F7 | Messages | `.rs` |

The Go and Rust rows use the Debug Adapter Protocol (the same wire protocol VS
Code, Neovim and Emacs DAP use). Setup and per-language notes:
[docs/ide-setup.md](docs/ide-setup.md).

## Documentation

| Level | Access | Content |
|-------|--------|---------|
| **In-app help** | F1 | Menus, key bindings, basic usage |
| **Texinfo manual** | `info xwpe` | 13 chapters: editor, compiling, debugging, language servers, tutorials, reference |
| **Man pages** | `man xwpe` | Command-line options |
| **Install guides** | [docs/install/](docs/install/) | Per-platform install + build |
| **Build from source** | [BUILDING.md](BUILDING.md) | Dependencies, `configure` flags, tests |
| **IDE setup (LSP/DAP)** | [docs/ide-setup.md](docs/ide-setup.md) | Compilers, language servers, debuggers, environment |
| **LSP / IDE guide** | [docs/LSP.md](docs/LSP.md) | The `Alt-Q` actions, per-language setup |
| **Terminal & console notes** | [docs/terminals.md](docs/terminals.md) | Clipboard, tmux/screen, mouse, Linux console |
| **Try-it demos** | [docs/examples/](docs/examples/) | Runnable, commented testbeds (one per language) |
| **Demo GIFs** | [docs/demos/](docs/demos/) | Recorded feature clips and per-language tours |
| **For contributors** | [HACKING.md](HACKING.md), [HACKING-LSP.md](HACKING-LSP.md), [HACKING-DAP.md](HACKING-DAP.md) | Editor, LSP client and DAP debugger internals |

## Known limitations

- **Language server, one document at a time**: the LSP server attaches to a
  single open file; an Alt-Q action in a different file transparently re-points
  it (a brief pause). Rename (Alt-Q N) applies to the current file only; edits it
  would make in other files are reported, not applied.
- **Dialog scrollbars**: window scrollbars use Unicode glyphs (1.6.3), but the
  scrollbars drawn inside dialogs are still ASCII. Cosmetic; planned for v1.7.

## Contributing

Issues, bug reports, and patches welcome on Codeberg:
https://codeberg.org/mendezr/xwpe

**We especially need testers.** xwpe was untested for nearly 20 years. If you can
try it &mdash; any terminal, any compiler, any workflow &mdash; open an issue
even if everything works. See [tests/README.md](tests/README.md) for the
automated test suite.

## Project history

- **Fred Kruse** &mdash; original author (1993, last release 1.4.2 ca. 1997)
- **Dennis Payne** &mdash; continuation (1.5.x, 1997-2006), with contributions
  from ~25 developers (see CHANGELOG): https://www.identicalsoftware.com/xwpe/
- **Debian contributors** (Jari Aalto, Francesco P. Lovergine, Andreas Tille,
  et al.) &mdash; patches now integrated upstream
- **Juan Manuel Mendez Rey** &mdash; current maintainer (2026-), with Dennis
  Payne's blessing

Historical archive: https://codeberg.org/mendezr/xwpe-archives

## Architecture

For contributors and porters: see [HACKING.md](HACKING.md) for internal
architecture (SCREENCELL buffer model, double-buffer rendering, popup
save/restore, X11 extbyte system).

## Licence

GPL-2. See `COPYING`.
