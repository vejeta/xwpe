#!/bin/sh
# Build xwpe from Codeberg main on OpenBSD. Runs as root in the Vagrant VM.
#
# FIRST-RUN CAVEAT: OpenBSD package names / pkg-config paths may need a tweak
# round (like the FreeBSD pcre2 box issue) -- that is packaging, separate from
# any real code-portability error. Iterate: run, paste the error, fix.
set -eu

ARCHIVE="https://codeberg.org/mendezr/xwpe/archive/main.tar.gz"

echo "=== installing build deps (pkg_add) ==="
# generic/openbsd7 is OpenBSD 7.4, which is EOL: its packages are pruned from
# the default mirrors (the box's installurl 404s). leaseweb still archives
# 7.4 packages, so point pkg_add there explicitly.
export PKG_PATH="https://mirror.leaseweb.com/pub/OpenBSD/7.4/packages/amd64/"
# The headless box ships without the X11 (xenocara) base sets, so cairo/pango
# can't resolve libX11/Xft/fontconfig/freetype. Install the base X sets first
# (xbase has the libs + headers + .pc files) so the FULL build works: one
# binary that runs as wpe (ncurses console) or xwpe (X11), chosen by argv[0].
echo "=== installing X11 base sets (xenocara) ==="
for s in xbase74 xshare74 xfont74; do
  ftp -o "/tmp/$s.tgz" "https://mirror.leaseweb.com/pub/OpenBSD/7.4/amd64/$s.tgz"
  tar -C / -xzphf "/tmp/$s.tgz"
done

# metaauto provides the autoconf/automake version wrappers; pin versions so the
# install is non-interactive. NOTE: OpenBSD has NO ncurses package -- wide-char
# curses lives in the base system, and configure.ac falls back to linking it
# (-lcurses) when no ncursesw.pc is present.
pkg_add -I gmake metaauto autoconf%2.71 automake%1.16 pkgconf \
  cairo pango json-c libvterm

# OpenBSD's auto* wrappers require an explicit version.
AUTOCONF_VERSION=2.71; export AUTOCONF_VERSION
AUTOMAKE_VERSION=1.16; export AUTOMAKE_VERSION
# Base X11 .pc files live under /usr/X11R6.
PKG_CONFIG_PATH=/usr/X11R6/lib/pkgconfig:/usr/local/lib/pkgconfig
export PKG_CONFIG_PATH

# Extracting the X sets by hand does NOT rebuild ld.so's hints cache, so the
# built binary would fail to LOAD /usr/X11R6/lib/* at runtime ('can't load
# library libfreetype.so...'). Register the X and package lib dirs.
ldconfig /usr/X11R6/lib /usr/local/lib

echo "=== fetching main archive ==="
# OpenBSD's tar has no --strip-components, so detect the archive's top dir and
# cd into it instead.
ftp -o /tmp/xwpe-main.tar.gz "$ARCHIVE"
SRCTOP="$(tar tzf /tmp/xwpe-main.tar.gz | head -1 | cut -d/ -f1)"
rm -rf "/tmp/$SRCTOP"
tar xzf /tmp/xwpe-main.tar.gz -C /tmp
cd "/tmp/$SRCTOP"

echo "=== autoreconf + configure (full build: wpe + xwpe; --without-gpm) ==="
# Full build -- both modes. --without-gpm only: GPM is the Linux raw-console
# mouse daemon and does not exist on OpenBSD. The ncurses console mouse (xterm
# reporting, used by 'wpe') is unaffected by this.
autoreconf -fi
./configure --without-gpm

echo "=== build (full log -> /tmp/xwpe-build.log) ==="
gmake -j"$(sysctl -n hw.ncpu)" > /tmp/xwpe-build.log 2>&1 || {
  echo "=== BUILD FAILED -- error/Error lines ==="
  grep -nE "error:|undefined reference|undefined symbol|ld:|\*\*\* .*Error [0-9]" \
    /tmp/xwpe-build.log | head -40
  echo "=== last 30 lines of the build log ==="
  tail -30 /tmp/xwpe-build.log
  echo "(full log in the VM at /tmp/xwpe-build.log)"
  exit 1
}

echo "=== result ==="
if [ -x ./we ]; then echo "OPENBSD BUILD OK:"; file ./we; else echo "FAILED: no ./we"; exit 1; fi
