# Installing xwpe on Fedora / RHEL

A Fedora package is in review ([Bugzilla
2486113](https://bugzilla.redhat.com/show_bug.cgi?id=2486113)); once it is
accepted, `sudo dnf install xwpe` will install it directly. Until then, build
from source.

## Build from source

```sh
sudo dnf install gcc make autoconf automake pkgconf-pkg-config texinfo \
  ncurses-devel libX11-devel libXft-devel cairo-devel pango-devel \
  libvterm-devel json-c-devel gpm-devel zlib-devel
autoreconf -fi && ./configure && make && sudo make install
```

(`librsvg2-tools` adds `rsvg-convert` if you want the rasterised icon installed;
it is optional.) For a console-only build drop the X libraries and pass
`--without-x` -- only `ncurses-devel` plus the build tools are then required.

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
