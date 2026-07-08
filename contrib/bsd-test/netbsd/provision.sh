#!/bin/sh
# Build xwpe from Codeberg main on NetBSD 10, then run the shared runtime tests.
# Runs as root in the Vagrant VM.  Box: pygolo/netbsd10 (NetBSD 10.0).
set -eu

ARCHIVE="https://codeberg.org/mendezr/xwpe/archive/main.tar.gz"

# The box has a default IPv6 route but no working IPv6 connectivity, and the
# NetBSD mirrors publish AAAA records -- so ftp/pkgin try IPv6 and hang/fail on
# "No route to host" without falling back.  Drop the broken IPv6 default route so
# connects fail-fast and fall back to IPv4.
route -qn delete -inet6 default 2>/dev/null || true
# pkgin's libfetch ignores the route tweak and still tries the mirror's AAAA
# ("No route to host") without falling back to IPv4.  Pin ftp.NetBSD.org to its
# IPv4 in /etc/hosts so getaddrinfo returns only the A record.  Its .tgz files
# 302-redirect within the same host (10.0 -> 10.0_<quarter>), so this covers the
# redirects too.  (If this IP ever changes, refresh it -- it is a stable NetBSD
# project host.)
grep -q "ftp.NetBSD.org" /etc/hosts || echo "199.233.217.201 ftp.NetBSD.org" >> /etc/hosts

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

# pkgsrc tools install under /usr/pkg; X11 is in the base system under /usr/X11R7.
PATH=/usr/pkg/bin:/usr/pkg/sbin:$PATH; export PATH
# /usr/pkg FIRST: prefer pkgsrc's newer .pc files over the base X set's, so
# 'pkg-config cairo' resolves its Requires chain.
PKG_CONFIG_PATH=/usr/pkg/lib/pkgconfig:/usr/X11R7/lib/pkgconfig:/usr/lib/pkgconfig
export PKG_CONFIG_PATH

# The box ships without the X11 base sets, so cairo/pango/libXft can't resolve
# libX11/Xft/fontconfig.  Install them: xbase = runtime X libs, xcomp = X headers
# and .pc files.  Fetched dynamically for whatever release this box is.
echo "=== installing X11 base sets (xbase/xcomp) ==="
REL="$(uname -r | cut -d_ -f1)"
ARCH="$(uname -m)"
SETS="https://ftp.NetBSD.org/pub/NetBSD/NetBSD-${REL}/${ARCH}/binary/sets"
for s in xbase xcomp; do
  fetch4 "/tmp/$s.tar.xz" "$SETS/$s.tar.xz"
  tar -C / -xJpf "/tmp/$s.tar.xz"
done

echo "=== installing build deps (pkgin) ==="
# Use ftp.NetBSD.org (the project's own server) rather than the Fastly CDN, which
# this VM's NAT reaches unreliably.  The pkgsrc binary path uses the MACHINE_ARCH
# (uname -p = x86_64), not MACHINE (uname -m = amd64).
echo "https://ftp.NetBSD.org/pub/pkgsrc/packages/NetBSD/$(uname -p)/${REL}/All" \
  > /usr/pkg/etc/pkgin/repositories.conf
pkgin -y update || true
# NetBSD 10 pkgsrc names: libvterm is 'libvterm01' (was libvterm03 on NetBSD 9),
# wide-char ncurses is 'ncurses'.  python311 + twm drive the headless X11 smoke
# (best-effort: the smoke skips cleanly if any of Xvfb/twm/python3 is absent).
pkgin -y install gmake autoconf automake pkgconf cairo pango json-c \
  libvterm01 ncurses python311 twm xdotool || true

echo "=== fetching main archive ==="
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
gmake -j"$(/sbin/sysctl -n hw.ncpu 2>/dev/null || echo 2)" > /tmp/xwpe-build.log 2>&1 || {
  echo "=== BUILD FAILED -- error: lines only ==="
  grep -nE "error:|undefined reference|undefined symbol|ld:" /tmp/xwpe-build.log | head -40
  echo "(full log in the VM at /tmp/xwpe-build.log)"
  exit 1
}

echo "=== result ==="
if [ -x ./we ]; then echo "NETBSD BUILD OK:"; file ./we; else echo "FAILED: no ./we"; exit 1; fi

# Runtime tests, shared across the BSD provisioners (unit tests + wpe console
# mouse + xwpe X11 Xvfb smoke).
sh contrib/bsd-test/run_tests.sh "/tmp/$SRCTOP"
exit $?
