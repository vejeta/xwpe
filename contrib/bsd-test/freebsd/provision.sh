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
# Runtime-test tooling: Xvfb + a tiny WM + xdpyinfo drive the headless X11 smoke
# (contrib/bsd-test/xvfb_smoke.sh); python3 checks the captured frame.
pkg install -y xorg-vfbserver matchbox-window-manager xdpyinfo python3

# FreeBSD's freetype2.pc has "Requires.private: ... bzip2 ...", but base bzip2
# (libbz2 + bzlib.h are in the base system) ships no bzip2.pc, so pkg-config
# cannot resolve xft/cairo/pango/fontconfig -> configure reports "Xft: no" and
# the X11 build degrades. A real port build (poudriere) has this via the ports
# framework; for this manual VM, drop a minimal bzip2.pc pointing at base libbz2.
if ! pkg-config --exists bzip2 2>/dev/null; then
  cat > /usr/local/libdata/pkgconfig/bzip2.pc <<'PC'
Name: bzip2
Description: bzip2 compression library (base system)
Version: 1.0.8
Libs: -lbz2
Cflags:
PC
fi

echo "=== fetching main archive (${ARCHIVE}) ==="
rm -rf /tmp/xwpe-src && mkdir -p /tmp/xwpe-src
fetch -o /tmp/xwpe-main.tar.gz "$ARCHIVE"
tar xzf /tmp/xwpe-main.tar.gz -C /tmp/xwpe-src --strip-components=1
cd /tmp/xwpe-src

echo "=== autoreconf + configure (--without-gpm: GPM is Linux-only) ==="
autoreconf -fi
# --disable-dependency-tracking: config.status bootstraps the automake .deps
# fragments by running the DEFAULT make, which on the BSDs is BSD make -- it
# chokes on the GNU-make dependency syntax ("Something went wrong bootstrapping
# makefile fragments").  We build with gmake below; disabling dep-tracking skips
# that bootstrap entirely (fine for a one-shot from-scratch build).
./configure --without-gpm --disable-dependency-tracking

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

# --- Runtime tests: prove the binary RUNS, not just links. ---
RC=0

echo "=== unit tests (gmake check) ==="
if gmake check >/tmp/xwpe-check.log 2>&1; then
  echo "make check: PASS"
else
  echo "make check: FAIL"; grep -iE "FAIL:|error:" /tmp/xwpe-check.log | head; RC=1
fi

echo "=== console-mouse probe (wpe under a pty) ==="
cc -O2 -o /tmp/probe contrib/bsd-test/sgr_mouse_probe.c -lutil
if /tmp/probe ./we xterm-256color; then
  echo "wpe mouse probe: PASS"
else
  echo "wpe mouse probe: FAIL"; RC=1
fi

echo "=== headless X11 smoke (xwpe under Xvfb) ==="
sh contrib/bsd-test/xvfb_smoke.sh /tmp/xwpe-src || RC=1

[ "$RC" = 0 ] && echo "ALL FREEBSD RUNTIME TESTS PASSED" \
             || echo "SOME FREEBSD RUNTIME TESTS FAILED"
exit "$RC"

# To finish the port afterwards, from inside `vagrant ssh`:
#   cd /tmp/xwpe-src && gmake install        # smoke-test the install
# and copy the four port files into a ports tree to run:
#   make makesum && make stage && make makeplist && poudriere testport
