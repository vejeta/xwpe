# xwpe

Xwpe is a development environment designed for use on UNIX systems.
Fred Kruse wrote xwpe and released it as free software under the GNU
General Public License. The user interface was designed to mimic the
Borland C and Pascal family. Extensive support is provided for
programming: syntax highlighting, integrated compiler and debugger
interface, project management, and a function-key driven menu system.

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

## Building

This release ships with the original autoconf-based build (configure.in
+ handwritten Makefile.in) for continuity with Payne's 1.5.30a. A
modernisation to autoreconf-driven autotools is planned for the next
release.

```sh
./configure
make
sudo make install
```

Common configure-time dependencies on Debian/Ubuntu:

```sh
sudo apt install build-essential libncurses-dev libx11-dev libgpm-dev
```

Run the editor in terminal mode with `wpe`, in X11 mode with `xwpe`.

## Licence

GPL-2. See `COPYING`.
