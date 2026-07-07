#!/bin/sh
# Build xwpe from Codeberg main on NetBSD. Runs as root in the Vagrant VM.
#
# FIRST-RUN CAVEAT: pkgin package names / pkg-config paths may need a tweak
# round -- packaging, separate from any real code-portability error. Iterate.
set -eu

ARCHIVE="https://codeberg.org/mendezr/xwpe/archive/main.tar.gz"

# pkgsrc tools install under /usr/pkg; X11 is in the base system under /usr/X11R7.
PATH=/usr/pkg/bin:/usr/pkg/sbin:$PATH; export PATH
# /usr/pkg FIRST: the base X sets ship an older fontconfig.pc that fails cairo's
# Requires; pkgsrc's newer fontconfig.pc must win so 'pkg-config cairo' resolves.
PKG_CONFIG_PATH=/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig:/usr/lib/pkgconfig
export PKG_CONFIG_PATH

# This minimal box ships without the X11 base sets, so the pkgsrc cairo/pango/
# libXft have unmet requirements (libX11.so.7 etc. absent under /usr/X11R7).
# Install the X sets first: xbase = runtime X libs, xcomp = X headers + .pc.
echo "=== installing X11 base sets (xbase/xcomp) ==="
REL="$(uname -r | cut -d_ -f1)"
ARCH="$(uname -m)"
SETS="https://cdn.netbsd.org/pub/NetBSD/NetBSD-${REL}/${ARCH}/binary/sets"
for s in xbase xcomp; do
  ftp -o "/tmp/$s.tar.xz" "$SETS/$s.tar.xz"
  tar -C / -xJpf "/tmp/$s.tar.xz"
done

echo "=== installing build deps (pkgin) ==="
pkgin -y update || true
# Full build (one binary: wpe ncurses console + xwpe X11). pkgsrc names differ:
# libvterm 0.3.x is 'libvterm03', and wide-char ncurses is just 'ncurses'.
pkgin -y install gmake autoconf automake pkgconf cairo pango json-c libvterm03 ncurses

echo "=== fetching main archive ==="
ftp -o /tmp/xwpe-main.tar.gz "$ARCHIVE"
SRCTOP="$(tar tzf /tmp/xwpe-main.tar.gz | head -1 | cut -d/ -f1)"
rm -rf "/tmp/$SRCTOP"
tar xzf /tmp/xwpe-main.tar.gz -C /tmp
cd "/tmp/$SRCTOP"

echo "=== autoreconf + configure (full build: wpe + xwpe; --without-gpm) ==="
# --without-gpm only: GPM is the Linux raw-console mouse daemon, absent on the
# BSDs. The ncurses console mouse (xterm reporting, used by 'wpe') is unaffected.
autoreconf -fi
# --disable-dependency-tracking: config.status would bootstrap the automake
# .deps fragments with the default (BSD) make, which chokes on GNU-make syntax;
# we build with gmake below, so skip that bootstrap.
./configure --without-gpm --disable-dependency-tracking

echo "=== build (full log -> /tmp/xwpe-build.log) ==="
gmake -j"$(sysctl -n hw.ncpu)" > /tmp/xwpe-build.log 2>&1 || {
  echo "=== BUILD FAILED -- error: lines only ==="
  grep -nE "error:|undefined reference|undefined symbol|ld:" /tmp/xwpe-build.log | head -40
  echo "(full log in the VM at /tmp/xwpe-build.log)"
  exit 1
}

echo "=== result ==="
if [ -x ./we ]; then echo "NETBSD BUILD OK:"; file ./we; else echo "FAILED: no ./we"; exit 1; fi
