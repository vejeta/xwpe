# Installing xwpe on Debian / Ubuntu

xwpe 1.6.6 is in **Debian unstable**:

```sh
sudo apt install xwpe
```

It migrates to testing after the usual delay. On stable/oldstable or Ubuntu (or
to track the latest), build from source -- it is four lines.

## Build from source

```sh
sudo apt install build-essential autoconf automake pkg-config texinfo \
  libncurses-dev libx11-dev libxft-dev libcairo2-dev libpango1.0-dev \
  libvterm-dev libjson-c-dev libgpm-dev zlib1g-dev librsvg2-bin
autoreconf -fi && ./configure && make && sudo make install
```

The X11 libraries enable `xwpe`'s anti-aliased Xft/Cairo rendering; for a
console-only build drop them and pass `--without-x` (only `libncurses-dev` plus
the build tools are then required). `texinfo` builds the `info xwpe` manual.

Run it:

```sh
wpe file.c          # terminal mode
xwpe file.c         # X11 mode
```

`make install` installs the syntax-highlighting rules, the in-app Help, the
option file and the man page. Skip it and you get only built-in C/C++
highlighting and no Help.

## Next steps

- Build options, install prefixes and running from the build tree without
  installing: [BUILDING.md](../../BUILDING.md)
- The `Alt-Q` IDE layer (compilers, language servers, debuggers):
  [docs/ide-setup.md](../ide-setup.md)
- Terminal and console notes (clipboard, tmux, mouse, Linux console):
  [docs/terminals.md](../terminals.md)
