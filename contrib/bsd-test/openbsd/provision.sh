#!/bin/sh
# Build xwpe from Codeberg main on OpenBSD 7.x, then run the shared runtime tests.
# Runs as root in the Vagrant VM.  Box: pygolo/openbsd7 (OpenBSD 7.7).
set -eu

ARCHIVE="https://codeberg.org/mendezr/xwpe/archive/main.tar.gz"

# The box has a default IPv6 route but no working IPv6 connectivity, and the
# mirrors publish AAAA records -- so ftp/pkg_add try IPv6 and hang.  Drop the
# broken IPv6 default route so connects fail-fast and fall back to IPv4.
route -qn delete -inet6 default 2>/dev/null || true

# The VM's NAT reaches the mirrors unreliably (intermittent connect timeouts),
# so retry fetches a few times over IPv4.
fetch4() {  # fetch4 <outfile> <url>
  _i=1
  while [ "$_i" -le 6 ]; do
    ftp -4 -o "$1" "$2" && return 0
    echo "  (fetch retry $_i: $2)"; sleep 5; _i=$((_i + 1))
  done
  return 1
}

REL="$(uname -r)"           # e.g. 7.7
V="$(uname -r | tr -d .)"   # e.g. 77
ARCH="$(uname -m)"          # amd64

# Pick a mirror that still carries this release, because the box's own default
# (ftp.usa.openbsd.org) is often missing packages.  cdn.openbsd.org and
# ftp.openbsd.org keep only supported releases: an EOL one (7.4 and older) is
# pruned from both and survives on the archive mirrors.  The probe is the
# package directory that pkg_add itself reads, and it is a single attempt per
# mirror -- a pruned release answers 404 at once, so only the flaky NAT needs
# fetch4's retries, further down.
echo "=== choosing a mirror for ${REL}/${ARCH} ==="
MIRROR=""
for m in https://cdn.openbsd.org/pub/OpenBSD \
         https://ftp.openbsd.org/pub/OpenBSD \
         https://mirror.leaseweb.com/pub/OpenBSD ; do
  if ftp -4 -o /dev/null "$m/$REL/packages/$ARCH/" >/dev/null 2>&1; then
    MIRROR="$m"; break
  fi
  echo "  (no ${REL} packages at $m)"
done
if [ -z "$MIRROR" ]; then
  echo "FAILED: no mirror carries OpenBSD $REL packages for $ARCH"
  exit 1
fi
echo "mirror: $MIRROR"
echo "$MIRROR" > /etc/installurl

# The box ships without the X11 (xenocara) base sets, so cairo/pango/libXft
# cannot resolve libX11/Xft/fontconfig/freetype.  Install the base X sets first
# (xbase = libs, xshare = headers/.pc/data, xfont = fonts, xserv = X servers incl
# Xvfb for the headless smoke).
echo "=== installing X11 base sets (xenocara ${REL}) ==="
for s in xbase xshare xfont xserv; do
  fetch4 "/tmp/$s$V.tgz" "$MIRROR/${REL}/$ARCH/$s$V.tgz"
  tar -C / -xzphf "/tmp/$s$V.tgz"
done

# metaauto provides the autoconf/automake version wrappers.  OpenBSD has NO
# ncurses package -- wide-char curses is in the base system, and configure.ac
# falls back to -lcurses when no ncursesw.pc is present.  python3 + twm drive the
# headless X11 smoke (best-effort; the smoke skips if a tool is missing).
echo "=== installing build deps (pkg_add) ==="
pkg_add -I gmake metaauto autoconf%2.72 automake%1.16 pkgconf \
  cairo pango json-c libvterm python3 xdotool || \
pkg_add -I gmake metaauto autoconf%2.71 automake%1.16 pkgconf \
  cairo pango json-c libvterm python3 xdotool || true

# The terminal-UI tests (tests/*.py) need pytest and pyte.  OpenBSD spells
# pytest 'py3-test'; there is no py3-pytest, only py3-pytest-subtests.  pyte is
# named separately from wcwidth, but it does not import without it.
echo "=== installing terminal-UI test deps (pytest, pyte) ==="
pkg_add -I py3-test py3-pyte py3-wcwidth || \
pkg_add -I py3-pytest py3-pyte py3-wcwidth || true

# The auto* wrappers require an explicit version; match whatever got installed.
AUTOCONF_VERSION="$(ls /usr/local/bin/autoconf-* 2>/dev/null | sed 's/.*autoconf-//' | sort -V | tail -1)"
AUTOMAKE_VERSION="$(ls -d /usr/local/share/automake-* 2>/dev/null | sed 's/.*automake-//' | sort -V | tail -1)"
export AUTOCONF_VERSION AUTOMAKE_VERSION
# Base X11 .pc files live under /usr/X11R6.
PKG_CONFIG_PATH=/usr/X11R6/lib/pkgconfig:/usr/local/lib/pkgconfig
export PKG_CONFIG_PATH
# Extracting the X sets by hand does not refresh ld.so's hints, so register the
# X and package lib dirs or the built binary fails to load at runtime.
ldconfig /usr/X11R6/lib /usr/local/lib

echo "=== fetching main archive ==="
# OpenBSD's tar has no --strip-components; detect the archive's top dir.
fetch4 /tmp/xwpe-main.tar.gz "$ARCHIVE"
SRCTOP="$(tar tzf /tmp/xwpe-main.tar.gz | head -1 | cut -d/ -f1)"
rm -rf "/tmp/$SRCTOP"
tar xzf /tmp/xwpe-main.tar.gz -C /tmp
cd "/tmp/$SRCTOP"

echo "=== autoreconf + configure (full build: wpe + xwpe; --without-gpm) ==="
autoreconf -fi
# --disable-dependency-tracking: config.status would bootstrap the automake
# .deps fragments with the default (BSD) make, which chokes on GNU-make syntax;
# we build with gmake below, so skip that bootstrap.
./configure --without-gpm --disable-dependency-tracking

echo "=== build (full log -> /tmp/xwpe-build.log) ==="
gmake -j"$(sysctl -n hw.ncpu)" > /tmp/xwpe-build.log 2>&1 || {
  echo "=== BUILD FAILED -- error/Error lines ==="
  grep -nE "error:|undefined reference|undefined symbol|ld:|\*\*\* .*Error [0-9]" \
    /tmp/xwpe-build.log | head -40
  echo "=== last 30 lines ==="; tail -30 /tmp/xwpe-build.log
  exit 1
}

echo "=== result ==="
if [ -x ./we ]; then echo "OPENBSD BUILD OK:"; file ./we; else echo "FAILED: no ./we"; exit 1; fi

# Runtime tests, shared across the BSD provisioners.
sh contrib/bsd-test/run_tests.sh "/tmp/$SRCTOP"
exit $?
