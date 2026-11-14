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

# The same IPv6 problem, for pkgin: ftp.NetBSD.org is pinned to a literal
# address above, any other mirror is pinned from DNS so libfetch gets an A
# record only.  Best-effort -- without an IPv4 the install just fails.
pin4() {  # pin4 <hostname>
  _ip=$(host -4 -t a "$1" 2>/dev/null | awk '/has address/ {print $NF; exit}')
  if [ -n "$_ip" ]; then
    grep -q "$1" /etc/hosts || echo "$_ip $1" >> /etc/hosts
  else
    echo "  (no IPv4 for $1; pkgin may fail on IPv6)"
  fi
}

# One name pkgin cannot find aborts the whole transaction, so the packages whose
# names move between releases are installed one at a time; first match wins.
pkgin_any() {  # pkgin_any <name> [<name>...]
  for _n in "$@"; do
    echo "  trying $_n"
    if pkgin -y install "$_n"; then return 0; fi
  done
  echo "  (the repository has none of: $*)"
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

echo "=== choosing a pkgsrc binary repository ==="
# The pkgsrc binary path uses MACHINE_ARCH (uname -p = x86_64), not MACHINE
# (uname -m = amd64).  ftp.NetBSD.org (the project's own server) and its CDN
# carry the releases pkgsrc still builds for; an EOL release is pruned from both
# and its packages survive on the archive host, so each candidate is tried with
# the real thing -- `pkgin update` -- and the winner is reported.  Set
# XWPE_PKG_REPO to skip the search when the right directory is already known,
# for instance a quarterly one:
#   XWPE_PKG_REPO=https://cdn.NetBSD.org/pub/pkgsrc/packages/NetBSD/x86_64/9.0_2024Q4/All
CONF=/usr/pkg/etc/pkgin/repositories.conf
REPO="${XWPE_PKG_REPO:-}"
if [ -n "$REPO" ]; then
  echo "$REPO" > "$CONF"
  pkgin -y update || true
else
  for u in \
      "https://ftp.NetBSD.org/pub/pkgsrc/packages/NetBSD/$(uname -p)/${REL}/All" \
      "https://cdn.NetBSD.org/pub/pkgsrc/packages/NetBSD/$(uname -p)/${REL}/All" \
      "https://archive.netbsd.org/pub/pkgsrc/packages/NetBSD/$(uname -p)/${REL}/All" \
      "https://archive.netbsd.org/pub/pkgsrc-archive/packages/NetBSD/$(uname -p)/${REL}/All" ; do
    echo "  trying $u"
    _h=${u#https://}
    pin4 "${_h%%/*}"
    echo "$u" > "$CONF"
    if pkgin -y update; then REPO="$u"; break; fi
  done
fi
echo "repository: ${REPO:-none answered}"
[ -n "$REPO" ] || echo "  (the installs below will fail; set XWPE_PKG_REPO)"

# Names that pkgsrc keeps across releases go in one transaction; the ones it
# renames go through pkgin_any below.  Wide-char ncurses is 'ncurses'.
pkgin -y install gmake autoconf automake pkgconf cairo pango json-c ncurses \
  twm xdotool || true
# libvterm is 'libvterm01' on NetBSD 10 and was 'libvterm03' on NetBSD 9.
pkgin_any libvterm01 libvterm03 || true
# python3 + twm drive the headless X11 smoke (best-effort: the smoke skips
# cleanly if any of Xvfb/twm/python3 is absent).
pkgin_any python313 python312 python311 python310 || true
# The terminal-UI tests (tests/*.py) need pytest and pyte, which pkgsrc spells
# after the interpreter they are built for: py313-test, py313-pyte.  That
# version moves with the release, so try the recent ones newest first, and name
# wcwidth explicitly because pyte does not import without it.
for v in 313 312 311 310 39; do
  echo "  trying py$v-test py$v-pyte py$v-wcwidth"
  if pkgin -y install "py$v-test" "py$v-pyte" "py$v-wcwidth"; then break; fi
done

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
