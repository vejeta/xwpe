# Installing xwpe on Arch Linux

xwpe is on the **AUR**: [aur.archlinux.org/packages/xwpe](https://aur.archlinux.org/packages/xwpe).
With an AUR helper:

```sh
yay -S xwpe          # or: paru -S xwpe
```

Or build from source directly.

## Build from source

```sh
sudo pacman -S --needed base-devel autoconf automake pkgconf texinfo \
  ncurses libx11 libxft cairo pango libvterm json-c gpm zlib
autoreconf -fi && ./configure && make && sudo make install
```

(On Arch the development headers ship with the main packages, so there are no
separate `-devel` packages.) For a console-only build drop the X libraries and
pass `--without-x` -- only `ncurses` plus the build tools are then required.

Run it:

```sh
wpe file.c          # terminal mode
xwpe file.c         # X11 mode
```

`make install` installs the syntax-highlighting rules, the in-app Help, the
option file and the man page. Skip it and you get only built-in C/C++
highlighting and no Help.

## Next steps

- Build options, install prefixes and running from the build tree:
  [BUILDING.md](../../BUILDING.md)
- The `Alt-Q` IDE layer (compilers, language servers, debuggers):
  [docs/ide-setup.md](../ide-setup.md)
- Terminal and console notes (clipboard, tmux, mouse, Linux console):
  [docs/terminals.md](../terminals.md)
