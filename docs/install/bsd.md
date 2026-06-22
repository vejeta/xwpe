# Installing xwpe on the BSDs (FreeBSD / OpenBSD / NetBSD)

Since 1.6.6 xwpe builds and links the **full X11 build** on **FreeBSD 14**,
**OpenBSD 7.x** and **NetBSD 9** (verified in Vagrant VMs), and the console mouse
in `wpe` works over SSH on all three. The portability lives in `configure.ac`
(it links `libexecinfo` for `backtrace()`, falls back to the base `curses` /
accepts `ncurses.pc`, detects a usable `-std` flag, and takes the `vterm03`
module name) plus a `<sys/ioctl.h>` include and a terminfo-independent SGR mouse
decoder -- all no-ops on Linux.

## Build from source

Build it like any autotools project, with the platform's package manager for the
deps and `--without-gpm` (GPM is Linux-only); X is in the base system:

```sh
# FreeBSD:  pkg install autoconf automake pkgconf cairo pango json-c libvterm
# OpenBSD:  pkg_add  autoconf automake cairo pango json-c libvterm   (curses is base)
# NetBSD:   pkgin install autoconf automake pkgconf cairo pango json-c libvterm03 ncurses
autoreconf -fi
./configure --without-gpm      # add --without-x for the terminal-only wpe
make
```

See [BUILDING.md](../../BUILDING.md) for install prefixes and running from the
build tree, and [docs/ide-setup.md](../ide-setup.md) for the `Alt-Q` IDE layer.

## Ports

All three BSDs already package xwpe (at the old 1.5.30a):
[FreeBSD `devel/xwpe`](https://www.freshports.org/devel/xwpe/),
[OpenBSD `editors/xwpe`](https://openports.pl/path/editors/xwpe) and
[NetBSD pkgsrc `editors/xwpe`](https://pkgsrc.se/editors/xwpe).

A 1.6.6 update has been submitted to each (the 1.5.30a/1.6.5 tarballs predate
the portability fixes):

- **FreeBSD** -- bug [296205](https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=296205)
  against `devel/xwpe`, with the patch attached.
- **NetBSD** -- pull request to pkgsrc updating `editors/xwpe`.
- **OpenBSD** -- update posted to the `ports@openbsd.org` list.

Once these land, `pkg install xwpe` / `pkg_add xwpe` / `pkgin install xwpe` will
give 1.6.6 directly.
