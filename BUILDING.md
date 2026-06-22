# Building xwpe from source

For the per-platform dependency commands, see the install guides:
[Debian/Ubuntu](docs/install/debian.md) &middot;
[Fedora/RHEL](docs/install/fedora.md) &middot;
[openSUSE](docs/install/opensuse.md) &middot;
[Arch](docs/install/arch.md) &middot;
[macOS](docs/install/macos.md) &middot;
[BSD](docs/install/bsd.md). This file covers the build itself.

## What xwpe needs

xwpe needs a C compiler, autotools and pkg-config, plus **ncursesw** (required),
**libvterm** and **json-c**. **X11 + Xft + Cairo + Pango** are optional -- only
the graphical `xwpe` uses them; the terminal `wpe` builds without them
(`--without-x`). **GPM** is optional (Linux-console mouse; not on macOS).

## One command (Linux, macOS or BSD)

```sh
sh contrib/setup.sh             # deps -> build -> install -> wire your shell
sh contrib/setup.sh --dry-run   # ...or print exactly what it will run, first
```

It installs the build dependencies with whatever package manager it finds
(Linux: apt / dnf / zypper / pacman / emerge; macOS: Homebrew; the BSDs: pkg /
pkg_add / pkgin), builds and installs xwpe, and adds the environment helper to
your shell profile. It does **not** install the optional language servers --
those are in [docs/ide-setup.md](docs/ide-setup.md). Re-open your terminal when
it finishes. (It is a short, readable POSIX script:
[`contrib/setup.sh`](contrib/setup.sh).)

## Or do it by hand

The same four steps:

1. **Build** xwpe (this file).
2. **Install** the compilers / language servers you want --
   [docs/ide-setup.md](docs/ide-setup.md).
3. **Wire your shell** so xwpe finds them -- `sh contrib/xwpe-env --persist`;
   see [docs/ide-setup.md](docs/ide-setup.md). **Skip this and the `Alt-Q` LSP
   features report "server not found"** even though you installed them.
4. **Run** a demo -- `wpe docs/examples/c-lsp/main.cpp`.

For a plain editor, step 1 alone is enough; steps 2-4 add the
compiler/debugger/LSP layer.

## Build

Install the dependencies for your platform (see the install guides above), then,
from a git checkout:

```sh
autoreconf -fi   # only from a git checkout
./configure      # --without-x = terminal-only;  --without-gpm = no GPM mouse
make
sudo make install
```

`make install` is part of the normal build, not an afterthought: it installs
`syntax_def` (the syntax-highlighting rules), the in-app help, the option file
and the man page. **Skip it and you get only the built-in C/C++ highlighting and
no Help.**

## Running from the build tree without installing

Point `XWPE_LIB` at the build directory -- xwpe then loads its data files from
there:

```sh
XWPE_LIB="$(pwd)" ./wpe foo.c       # syntax_def + help + options from the build dir
```

(`XWPE_LIB` overrides the compiled-in install path; export it to make it
permanent -- or let `contrib/xwpe-env` set it, see
[docs/ide-setup.md](docs/ide-setup.md). A `~/.xwpe/syntax_def` copy also works,
but only for highlighting.)

## The install prefix

`--prefix="$HOME/.local"` keeps the whole install (`wpe`, `syntax_def`, the Help
files, the man page) under your home directory, no sudo. For a system-wide
install use `sudo make install` (default prefix `/usr/local`; on Apple Silicon
that directory must already exist and be writable). A bare `make install` with
neither a prefix nor sudo fails with `gmkdir: cannot create directory
'/usr/local/share': Permission denied` -- so pick one of the two. To **skip
installing entirely**, `XWPE_LIB="$(pwd)" ./wpe foo.c` (or `source
contrib/xwpe-env`) runs straight from the build tree with full syntax + Help.
(**GPM is Linux-only**, `--without-gpm` elsewhere; the mouse still works through
the terminal emulator.)

## Running the test suite

`tests/run-tests.sh` runs everything: the C unit tests, the pyte (ncurses,
`wpe`) suite, and the headless X11 GUI (`xwpe`) suite. The X11 step self-skips
when its stack is absent. On macOS the X11 harness needs a few extra tools:

```sh
brew install xdotool xclip netpbm imagemagick                    # X11 test harness
# XQuartz already provides Xvfb, xwd and twm; no separate install needed.
# Pillow + pytest + pyte are auto-bootstrapped into tests/.venv on first run.
```

`tests/run-tests.sh --x11` self-skips with a precise reason if any of these are
absent, so installing them is optional unless you want full coverage. See
[tests/README.md](tests/README.md) for the per-tool dependency table and the
macOS notes (twm fallback, ImageMagick 7 / xwdtopnm bridge, `xwpe.altMask`
override).
