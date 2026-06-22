# Installing xwpe on macOS

Both binaries build clean on macOS (Apple Silicon and Intel) against Homebrew:
`wpe` runs natively in a terminal (no XQuartz), and the graphical `xwpe` links
against XQuartz + Homebrew's Cairo/Pango/Xft stack and is exercised end-to-end
by the X11 test suite (see [tests/README.md](../../tests/README.md)).

> **macOS keyboard heads-up.** The Option key must be set as Meta, and the
> top-row `F1`-`F12` are taken by the system -- but every command also has a
> non-F-key binding, so xwpe is fully usable either way. See the
> **Keyboard & terminal on macOS** section below.

## Pre-built tap (shortest path)

A Homebrew tap on Codeberg packages the X11 build (`xwpe`, `xwe`, `wpe`, `we`)
so a fresh install is three commands:

```sh
brew install --cask xquartz                                       # one-time
brew tap mendezr/xwpe https://codeberg.org/mendezr/homebrew-xwpe
brew install xwpe          # add --HEAD to track main instead of the tag
```

That covers the editor itself; for the `Alt-Q` IDE layer continue with
[docs/ide-setup.md](../ide-setup.md). A terminal-only formula (no XQuartz, no GUI
deps) for `homebrew-core` is in review -- once accepted, plain `brew install
xwpe` will install `wpe`.

## From source

The [`contrib/setup.sh`](../../contrib/setup.sh) one-shot helper does the build
steps below for you (`sh contrib/setup.sh` -- `--dry-run` to preview,
`--skip-deps` if Homebrew is already populated).

Build with Homebrew's keg-only `ncurses` (the `PKG_CONFIG_PATH` line points
`configure` at it), and install to a user-writable prefix so `make install`
needs no sudo:

```sh
brew install autoconf automake pkg-config ncurses libvterm json-c texinfo
export PKG_CONFIG_PATH="$(brew --prefix ncurses)/lib/pkgconfig:$PKG_CONFIG_PATH"
autoreconf -fi
./configure --without-x --without-gpm --prefix="$HOME/.local"
make && make install
export PATH="$HOME/.local/bin:$PATH"      # add to ~/.zshrc to keep it
wpe foo.c                                 # syntax_def + Help come from the install
```

### Graphical `xwpe` (XQuartz)

Install [XQuartz](https://www.xquartz.org/) (`brew install --cask xquartz`, log
out/in once so `$DISPLAY` is wired up), then add the X/Cairo/Pango stack and
reconfigure with `--with-x`:

```sh
brew install --cask xquartz                                      # X server
brew install cairo pango libxft libxext libx11                   # rendering stack
./configure --with-x --without-gpm --prefix="$HOME/.local"
make && make install
xwpe foo.c                                                       # graphical build
```

That is a complete editor. For the `Alt-Q` IDE features continue with
[docs/ide-setup.md](../ide-setup.md) (install the servers and `source
contrib/xwpe-env`, which is what puts Metals/clangd on `PATH` and sets
`JAVA_HOME`).

**Using fish?** The two `export` lines above are bash/zsh; the fish equivalents
are `set -x PKG_CONFIG_PATH (brew --prefix ncurses)/lib/pkgconfig
$PKG_CONFIG_PATH` and `fish_add_path ~/.local/bin`. (The runtime variables --
`XWPE_LIB`, `JAVA_HOME`, server `PATH` -- are handled per-shell by
`contrib/xwpe-env`, see [docs/ide-setup.md](../ide-setup.md).)

## Keyboard & terminal on macOS

**The Meta key on a Mac is Option, not Command -- and you must enable it.**
xwpe's Alt-menus and the whole `Alt-Q` LSP layer need a key that sends a
Meta/`Esc` prefix. macOS reserves **Command (Cmd)** for the terminal and the
system, so Cmd-X / Cmd-R / Cmd-E act on
[kitty](https://sw.kovidgoyal.net/kitty/) / [iTerm2](https://iterm2.com/) and
never reach xwpe -- **Command is never Alt.** The Alt key is **Option (Opt)**,
but every Mac terminal defaults to using Option to type accented characters, so
you must switch it to Meta first:

- **kitty:** add `macos_option_as_alt yes` to `~/.config/kitty/kitty.conf`
  (kitty does NOT do this by default), then **fully quit and reopen kitty**
  (Cmd-Q) -- a config *reload* does not always apply this macOS setting.
  While you are in `kitty.conf`, also unbind `cmd+f`: kitty's default opens
  its own scrollback/search overlay on top of the editor, which looks like a
  rogue vi-style command line appearing under xwpe.  Recommended snippet:
  ```conf
  macos_option_as_alt yes      # Option sends Esc+ so Alt-menus reach xwpe
  map cmd+f no_op              # don't let kitty steal Cmd-F from the editor
  ```
  The arrow-vs-I-beam mouse pointer is handled by xwpe itself (it asks via
  OSC 22 when it enables mouse tracking); kitty and iTerm2 honour it,
  Terminal.app ignores it.
- **iTerm2:** Profiles -> Keys -> *Left Option key* -> *Esc+*.
- **Terminal.app:** Settings -> Profiles -> Keyboard -> *"Use Option as Meta
  key"*. Its dated terminfo also causes ncurses key/colour quirks, so prefer
  kitty or iTerm2. Keep `TERM=xterm-256color` everywhere.

Verify it before launching xwpe: run `cat -v`, press **Option-X** -- it must
print `^[x`. If it prints an accented glyph instead, Option is still a compose
key (the setting did not take -- check the file and that you fully restarted).
`Esc` alone prints `^[`; `Cmd-X` prints `^[[...u` (kitty's own protocol, which
xwpe cannot use -- that is why Command is never Alt). Once `cat -v` shows `^[x`,
drive xwpe with **Option** (Opt-Q, Opt-X, ...), exactly like Linux Alt.

**Function keys (F8/F9...) are grabbed by macOS.** By default the top-row keys
are Mission Control / brightness / volume, so xwpe never sees `F9` (build) or
`F8` (step). Either tick *System Settings -> Keyboard -> "Use F1, F2, etc. keys
as standard function keys"* (then press the **fn** key for the media actions),
or just use the equivalents that do not rely on the top row. Every function-key
action has one (the Borland heritage), so the top row is never required:

- **Build/debug:** `Alt-M` (Make = F9), `Alt-C` (Compile = Alt-F9), and the
  `Ctrl-G` debug prefix -- `Ctrl-G S` steps (= F8), `Ctrl-G T` traces (= F7),
  `Ctrl-G R` runs/continues, `Ctrl-G P` shows program output.
- **Menus / windows / help:** `Alt-Space` or `Esc` opens the menu bar (= F10)
  and `Alt-`*letter* jumps to any menu directly; `Alt-N` cycles windows
  (= F6) and `Alt-1`...`Alt-9` jump straight to a window by index; `Alt-Z`
  zooms (= F5); `Alt-H` opens Help (= F1). So even with the whole top row
  eaten by macOS, xwpe stays fully keyboard-drivable.

Verify a key reaches the terminal the same way: `cat -v`, press `F9`, it should
print `^[[20~` (not switch a Space or dim the screen).

## Notes

- **Install prefix / running the tests / `XWPE_LIB`**: see
  [BUILDING.md](../../BUILDING.md). On macOS, `--prefix="$HOME/.local"` keeps the
  whole install under your home directory with no sudo.
- **GPM is Linux-only**; use `--without-gpm` on macOS. The mouse still works
  through the terminal emulator -- see [docs/terminals.md](../terminals.md).
