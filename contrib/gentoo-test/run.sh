#!/bin/sh
# Gentoo build/runtime smoke for xwpe, in the official up-to-date stage3 image.
#
# Gentoo is Linux, so (unlike the BSDs) it needs no VM: the official
# gentoo/stage3 container is current (23.0 profile, working binhost), which
# avoids the stale-Vagrant-box profile/binpkg mess.  It builds the package the
# way the ebuild does -- straight from the "make dist" tarball (ships configure,
# no autoreconf) -- console-only, and runs the console mouse probe against it.
#
# Usage:
#   ./run.sh [/path/to/xwpe-<ver>.tar.gz]   # defaults to the build tree's tarball
set -eu

# Default to the tarball matching the configured version (not the lexically- or
# mtime-first of any stale xwpe-*.tar.gz lying in the build tree).
TOP="$(git rev-parse --show-toplevel)"
if [ -n "${1:-}" ]; then
  DIST="${1}"
else
  VER="$(sed -n 's/^#define PACKAGE_VERSION "\(.*\)"/\1/p' "${TOP}/config.h" 2>/dev/null)"
  DIST="${TOP}/xwpe-${VER}.tar.gz"
fi
[ -f "${DIST}" ] || { echo "no make-dist tarball found (looked for ${DIST}); pass one as \$1"; exit 2; }
echo "Testing dist tarball: ${DIST}"

docker run --rm -v "${DIST}":/dist.tar.gz:ro gentoo/stage3:latest bash -eu -c '
  echo "=== sync the ebuild tree and trust the binary host ==="
  emerge-webrsync >/dev/null 2>&1
  getuto

  echo "=== install the console build deps (prefer binary packages) ==="
  emerge --getbinpkg --quiet-build=y --jobs=4 --noreplace \
    dev-libs/json-c dev-libs/libvterm sys-libs/ncurses sys-libs/zlib virtual/pkgconfig

  echo "=== build console-only from the make-dist tarball (as the ebuild does) ==="
  mkdir /b && tar xzf /dist.tar.gz -C /b --strip-components=1
  cd /b
  ./configure --without-x --without-gpm
  make -j4

  echo "=== probe: drive wpe under a pty and click the File menu ==="
  cc -O2 -o /tmp/probe contrib/bsd-test/sgr_mouse_probe.c -lutil
  if /tmp/probe ./we xterm-256color; then
    echo "GENTOO RUNTIME OK: console build links and the menu responds"
  else
    echo "GENTOO PROBE FAILED"; exit 1
  fi
'
