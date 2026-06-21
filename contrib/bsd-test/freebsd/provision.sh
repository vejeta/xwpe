#!/bin/sh
# Provisioner: build xwpe from Codeberg git main on FreeBSD.
# Runs inside the Vagrant FreeBSD VM as root.
#
# We build from git main (not the release tarball) so each `vagrant provision`
# picks up the latest portability fixes without re-cutting the release asset.
# For a final RELEASE validation, point this back at the published tarball:
#   fetch .../releases/download/vX.Y.Z/xwpe-X.Y.Z.tar.gz  (no autoreconf needed).
set -eu

ARCHIVE="https://codeberg.org/mendezr/xwpe/archive/main.tar.gz"

echo "=== installing build deps ==="
ASSUME_ALWAYS_YES=yes pkg update
# Aged box images ship an older base package set than the current repo, so a
# fresh `pkg install` pulls new libs (e.g. glib) that need a newer libpcre2
# than the box has -> the built binary then fails to LOAD at runtime
# ('version PCRE2_... not defined'). pkg upgrade brings the whole set
# consistent so xwpe both builds AND runs.
pkg upgrade -y
# Deliberately NOT installing git: same aged-box skew makes pkg's git need a
# newer libpcre2 than base; fetch(1) is in the base system, so we use that.
pkg install -y autoconf automake libtool gmake pkgconf ncurses \
  libX11 libXft cairo pango libvterm json-c texinfo

echo "=== fetching main archive (${ARCHIVE}) ==="
rm -rf /tmp/xwpe-src && mkdir -p /tmp/xwpe-src
fetch -o /tmp/xwpe-main.tar.gz "$ARCHIVE"
tar xzf /tmp/xwpe-main.tar.gz -C /tmp/xwpe-src --strip-components=1
cd /tmp/xwpe-src

echo "=== autoreconf + configure (--without-gpm: GPM is Linux-only) ==="
autoreconf -fi
./configure --without-gpm

echo "=== build ==="
echo "=== build (full log -> /tmp/xwpe-build.log) ==="
gmake -j"$(sysctl -n hw.ncpu)" > /tmp/xwpe-build.log 2>&1 || {
  echo "=== BUILD FAILED -- error: lines only ==="
  grep -nE "error:|undefined reference|ld:" /tmp/xwpe-build.log | head -40
  echo "(full log in the VM at /tmp/xwpe-build.log -- 'vagrant ssh' to read it)"
  exit 1
}

echo "=== result ==="
if [ -x ./we ]; then
  echo "FREEBSD BUILD OK:"
  file ./we
else
  echo "FREEBSD BUILD FAILED: ./we not produced"
  exit 1
fi

# To finish the port afterwards, from inside `vagrant ssh`:
#   cd /tmp/xwpe-src && gmake install        # smoke-test the install
# and copy the four port files into a ports tree to run:
#   make makesum && make stage && make makeplist && poudriere testport
