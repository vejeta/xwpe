# Installing xwpe on openSUSE

xwpe is in the **openSUSE `editors` devel project** and has been submitted to
**Factory**; once it reaches a Tumbleweed snapshot, `sudo zypper install xwpe`
will install it directly. Until then (and on Leap), build from source.

## Build from source

```sh
sudo zypper install gcc make autoconf automake pkg-config texinfo \
  ncurses-devel libX11-devel libXft-devel cairo-devel pango-devel \
  libvterm-devel libjson-c-devel gpm-devel zlib-devel
autoreconf -fi && ./configure && make && sudo make install
```

For a console-only build drop the X libraries and pass `--without-x` -- only
`ncurses-devel` plus the build tools are then required. `texinfo` builds the
`info xwpe` manual.

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
