# xwpe

Xwpe is a development environment designed for use on UNIX systems.
Fred Kruse wrote xwpe and released it as free software under the GNU
General Public License. The user interface was designed to mimic the
Borland C and Pascal family, with Emacs-compatible cursor keys
(Ctrl-P/N/F/B/A/E) built in. Extensive support is provided for
programming: syntax highlighting, integrated compiler and debugger
interface, project management, and a function-key driven menu system.

The **1.6.x series ("xwpe 2026")** is the release cycle that brought
xwpe from 1993 to the present: UTF-8 terminal support, modern autotools,
working compiler integration for gcc/g++/gfortran/fpc, a 12-chapter
Texinfo manual, and an archived collection of 560+ historical documents
that were scattered across the early web. See [Project history](#project-history)
for the full lineage.

## Project history

This release continues the work of:

- **Fred Kruse** -- original author of xwpe (last official release 1.4.2,
  ca. 1996-1997).
- **Dennis Payne** -- xwpe-alpha continuation project (1.5.x series, final
  release 1.5.30a in 2006). Project home:
  https://www.identicalsoftware.com/xwpe/
- **Guus Bonnema** and other contributors via the GitHub fork at
  https://github.com/amagnasco/xwpe -- selected bug fixes integrated
  upstream here.
- **Debian xwpe contributors** (Jari Aalto, Francesco P. Lovergine,
  Andreas Tille, Helmut Grohne, Robert Millan, and others) -- the patches
  carried for years in https://salsa.debian.org/debian/xwpe/ are now
  integrated upstream and can be dropped from the Debian package.

## Current maintainer

Juan Manuel Mendez Rey \<juan.mendezr@proton.me\>, with the explicit
blessing of Dennis Payne.

## Project home

https://codeberg.org/mendezr/xwpe

Issues and contributions on Codeberg please. The previous GitHub fork at
amagnasco/xwpe is no longer the active project.

## What changed in 1.6.0

xwpe's screen buffer was a flat `char` array since 1993: 1 byte per
character, 1 byte per color attribute. This worked when terminals were
80x24, character sets were ASCII or Latin-1, and every character was
exactly 1 byte wide.

Modern terminals use UTF-8, where a single visible character can be 1 to
4 bytes (e.g. `é` = 2 bytes, `中` = 3 bytes, `⭐` = 4 bytes). The old
buffer stored each byte as a separate screen cell, causing borders to
shift, gaps to appear, and stale content after scrolling.

Version 1.6.0 replaces the byte buffer with `SCREENCELL`: a struct that
holds one decoded character (as `wchar_t`) plus its color attribute per
cell. The display loop decodes UTF-8 via `mbrtowc()` and stores one
`SCREENCELL` per visible column. The terminal refresh uses ncursesw's
`add_wch()` for wide characters and `wcwidth()` for characters that
occupy two columns (emoji, CJK). Each cell in the buffer corresponds
exactly to one column on screen -- borders align correctly regardless
of content.

This also enabled terminal resize support (via `KEY_RESIZE`) and
keyboard remapping for modern PC keyboards.

### Technical changes

| Component | Before (1.5.x) | After (1.6.0) | Why |
|-----------|----------------|---------------|-----|
| Screen buffer | `char *schirm` (2 bytes/cell: char + attr) | `SCREENCELL *schirm` (int ch + int attr) | Holds decoded `wchar_t` instead of raw bytes; 1 cell = 1 visual column |
| Buffer macros | `e_pr_char(x,y,c,frb)` via byte arithmetic `*(schirm + 2*MAXSCOL*y + 2*x)` | Same name, struct field access `schirm[y*MAXSCOL+x].ch` | Cleaner, no byte offset math, supports values > 255 |
| Display loop | `e_pr_line()`: 1 byte per iteration, `i++, j++` | `mbrtowc()` decodes multi-byte, `i += nb` (bytes consumed), `j++` (1 visual column) | One character per column regardless of byte count |
| Wide chars | Not handled | `wcwidth()` after decode; if width=2, fill second cell with space, advance j by 2 | Emoji (❌,⭐) and CJK take 2 columns |
| Terminal refresh | `addch(c)` per byte | `add_wch(&cc)` for wide chars, `addch(sp_chr[c])` for ACS, `addch(c)` for ASCII | `add_wch` is ncursesw's native wide character output |
| Change detection | `schirm[x] != altschirm[x]` (byte compare) | `schirm[idx].ch != altschirm[idx].ch` (struct field compare) | Same logic, different data type |
| Buffer alloc | `MALLOC(2 * MAXSCOL * MAXSLNS)` | `MALLOC(sizeof(SCREENCELL) * MAXSCOL * MAXSLNS)` | 8 bytes/cell instead of 2; buffer ~320KB for 80x50 |
| Save/restore | `e_gt_byte`/`e_pt_byte` (byte-by-byte copy) | `memcpy` of `SCREENCELL` structs | Full cell preserved including wide char value |
| ncurses link | `-lncurses` | `-lncursesw` | Wide character API (`add_wch`, `setcchar`) only in ncursesw |
| Scrollbar chars | `MCI='+'`, `MCA='0'` (ASCII) | `MCI=7` (ACS_S9), `MCA=11` (ACS_DIAMOND) | Restores visual style from 1990s terminal aesthetics |
| Resize | Not handled | `KEY_RESIZE` → realloc buffers, resize windows, repaint | Modern terminals change size dynamically |
| Keyboard | VT100/Sun mapping | PC keyboard mapping (PageUp, Home, End, Delete) | Original mapping was for hardware terminals no longer in use |
| GPM mouse | `Gpm_Open()` unconditionally | Check `/dev/gpmctl` first | Avoids "gpm: not found" error when daemon not running |

## What changed in 1.6.1

* **Modernised build system**: replaced the 1998 hand-written `configure.in`
  + `Makefile.in` + 1500-line custom `aclocal.m4` with standard
  `configure.ac` + `Makefile.am` (automake). `./configure && make` now
  works out of the box with any modern autoconf/automake. All ncursesw
  flags, `-std=gnu17`, and K&R warning suppressions are handled
  automatically -- no manual CFLAGS needed.
* **pkg-config for ncursesw**: detection uses `PKG_CHECK_MODULES` instead
  of a fragile cascade of `AC_CHECK_LIB` calls.
* **Optional X11 and GPM**: `--without-x` and `--without-gpm` configure
  flags for minimal builds.
* **Debian `dh_autoreconf` compatible**: the new `configure.ac` works with
  autoconf 2.69+, eliminating the need to skip `dh_autoreconf` in
  `debian/rules`.

## What changed in 1.6.2

* **Completed SCREENCELL migration**: fixed display artefacts after
  closing menus, dialogs, and compile popups. The popup save/restore
  system now uses properly initialised buffers throughout.
* **Compiler integration modernised**: error navigation (Alt-T/Alt-V)
  works with modern GCC/g++/gfortran/fpc. Updated default compilers
  (gfortran instead of f77, fpc instead of pc). Non-GNU compilers no
  longer receive invalid gcc-specific flags.
* **Output capture fixed**: compilers that emit blank lines between
  errors (gfortran, clang) no longer have their output truncated.

See CHANGELOG for full details.

## Compiler support

### Tested (v1.6.2)

| Compiler | Language | F9 | Alt-T/V | Default extensions |
|----------|----------|:--:|:-------:|-------------------|
| gcc      | C        | ok | ok | `.c` |
| g++      | C++      | ok | ok | `.cpp` `.cc` `.C` `.cxx` |
| gfortran | Fortran  | ok | ok | `.f` `.f90` `.f95` `.f03` `.f08` |
| fpc      | Pascal   | ok | ok | `.p` `.pas` `.pp` |
| javac    | Java     | ok | ok | `.java` |
| python3  | Python   | ok | ok | `.py` |

### Should work (GNU `file:line:column:` format)

Any compiler that emits `file:line:column: message` diagnostics works
with the default GNU error pattern:

clang, clang++, Rust (`rustc`), Go (`go build`), D (`dmd`, `ldc2`),
Zig, Vala (`valac`), GNU Guile, Haskell (`ghc`), Nim, Crystal,
Swift, Kotlin/Native.

### Configurable via Options -> Programming

Compilers with other diagnostic formats can be configured with a custom
Interpreter String using xwpe's pattern language
(`${FILE}`, `${LINE}`, `${COLUMN}`, wildcards):

- **javac** -- `file:line:` (no column); pattern: `${FILE}:${LINE}:*`
- **scalac** -- similar to javac
- **Node.js** (`tsc`) -- `file(line,column):` format
- **Free Pascal** is already configured with `${FILE}(${LINE},${COLUMN})*`
- **SBCL** (Common Lisp), **Clojure**, **Unison** -- custom formats,
  all expressible in xwpe's pattern language

## Architecture notes

Fred Kruse's 1993 design proved remarkably resilient. The core systems
that made the v1.6.x restoration possible -- rather than a rewrite --
include:

- **Pattern-based error parser**: a mini-language (`${FILE}:${LINE}:${COLUMN}:*`)
  that can match any compiler's diagnostic format. Designed in 1993, it
  handles gcc, g++, gfortran, fpc, and any future compiler with minimal
  configuration.
- **schirm/altschirm double buffer**: xwpe maintains two screen buffers and
  only repaints cells that differ. This is the same approach modern UI
  frameworks use (virtual DOM diffing). Kruse did it in C in 1993.
- **Popup save/restore**: manual save of screen regions under popups, with
  restore on close. We evaluated migrating to ncurses panels but
  concluded that the original architecture is cleaner for xwpe's use case.

The hardest part of the restoration was not fixing bugs in Kruse's logic
-- it was adapting the byte-level assumptions to modern character
encodings. The SCREENCELL migration (1.6.0) touched every rendering path,
and its ripple effects (uninitialised buffers, negative character values
in the special-character table) took the entire 1.6.2 cycle to resolve.

## Documentation

xwpe has three levels of documentation:

| Level | Access | Content |
|-------|--------|---------|
| **In-app help** | Press F1 inside xwpe | Menus, key bindings, basic usage |
| **Texinfo manual** | `info xwpe` after install | Full 12-chapter manual: installation, interface, editor, configuration, compiling, debugging, projects, tutorials, known issues, history, command reference |
| **Man pages** | `man xwpe`, `man wpe`, `man we` | Command-line options, quick reference |

The Texinfo manual is installed automatically by `make install` and
registered with `install-info`. It is also available as HTML:

```sh
make -C docs info    # build xwpe.info (built automatically by make)
make -C docs html    # build HTML version in docs/xwpe.html/
```

## Syntax highlighting

xwpe ships with syntax highlighting for C, C++, Fortran, Pascal, Java,
and Python in the `syntax_def` file. After building, copy it to your
personal config directory:

```sh
mkdir -p ~/.xwpe
cp syntax_def ~/.xwpe/syntax_def
```

Or install system-wide with `sudo make install`.

### Adding new languages

The `syntax_def` file is a plain text file. Each language block contains:

1. File extension (e.g. `.py`)
2. Number of keywords
3. Keywords (space-separated)
4. Number of multi-character operators
5. Multi-character operators
6. Single-character operators, comment delimiters, and special chars

See `docs/chapters/configuration.texi` for the full format documentation.
Example: the Python block in `syntax_def` defines 35 keywords, `#` as
line comment, and `"` / `'` as string delimiters.

## Known limitations

- **X11 mode**: UTF-8 display not yet implemented in xwpe (the X11
  binary). Terminal mode (`wpe`) has full UTF-8 support.
- **Window resize**: terminal resize is detected and handled, but editor
  windows do not scale proportionally.
- **javac**: Java uses `file:line:` format (no column number); error
  parsing requires manual configuration of the interpreter string.
- **Compiler flags**: the compile command (`e_comp`) uses GNU-style flags
  for GNU compilers and omits them for others. Exotic compilers may need
  manual Options -> Programming configuration.

## Contributing

Issues, bug reports, and patches welcome on Codeberg:
https://codeberg.org/mendezr/xwpe

**We especially need testers.** xwpe was untested for nearly 20 years.
If you can try it on your system and report what works and what doesn't
-- any terminal, any compiler, any workflow -- that is enormously
valuable. Open an issue even if everything works: knowing what
configurations are solid helps us prioritise.

The historical archive of mailing lists, manpages, tutorials, and Debian
BTS history is preserved at https://codeberg.org/mendezr/xwpe-archive.

## Building

Standard autotools build:

```sh
autoreconf -fi   # only needed if building from git
./configure
make
sudo make install
```

Common build dependencies on Debian/Ubuntu:

```sh
sudo apt install build-essential autoconf automake pkg-config \
  libncurses-dev libx11-dev libgpm-dev zlib1g-dev
```

Optional features:

```sh
./configure --without-x     # disable X11 support
./configure --without-gpm   # disable GPM mouse support
```

Run the editor in terminal mode with `wpe`, in X11 mode with `xwpe`.

### Console tips (Linux tty without X11)

If you run `wpe` on a Linux console (Ctrl+Alt+F2), modern HiDPI
framebuffers make bitmap fonts look tiny. For the classic 90s look:

```sh
# Install Terminus console font
sudo apt install console-terminus

# Use the large variant (readable on HiDPI)
setfont Lat15-Terminus32x16
```

For a permanent setup, edit `/etc/default/console-setup`:

```
FONTFACE="Terminus"
FONTSIZE="32x16"
```

Mouse works on the console via GPM (`sudo apt install gpm`).
In terminal emulators (xterm, kitty, gnome-terminal, tmux), mouse
works natively without GPM.

## External programs (optional)

xwpe integrates with external compilers and debuggers. Install the
ones you need:

```sh
# Compilers (install what you use)
sudo apt install gcc g++              # C/C++
sudo apt install gfortran             # Fortran
sudo apt install fpc                  # Free Pascal
sudo apt install default-jdk          # Java (javac + jdb)
sudo apt install python3              # Python (py_compile + pdb)

# Debuggers
sudo apt install gdb                  # C/C++/Fortran/Pascal debugging
# jdb is included with default-jdk    # Java debugging (built-in to xwpe)
# pdb is included with python3        # Python debugging (built-in to xwpe)
```

xwpe auto-detects the compiler by file extension and auto-selects
the debugger (gdb for C/C++/Fortran/Pascal, jdb for Java).

## Testing

See [tests/README.md](tests/README.md) for the automated test suite.

## Licence

GPL-2. See `COPYING`.
