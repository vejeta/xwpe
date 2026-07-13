#!/bin/sh
# run-tests.sh -- run the xwpe test suite (the local equivalent of the
# Debian autopkgtest).  The Debian autopkgtest is expected to call this same
# script, so the scenarios stay in one place; on the Debian side only the
# test Depends need maintaining.
#
# Usage:
#   tests/run-tests.sh            # C unit + pyte (./wpe) + X11 GUI (./xwpe)
#   tests/run-tests.sh --asan     # build we-asan and run the pyte suite under it
#   tests/run-tests.sh --x11      # ONLY the headless X11 GUI suite (./xwpe)
#   tests/run-tests.sh --wayland  # ONLY the native Wayland GUI suite (./xwpe)
#   tests/run-tests.sh --help
#
# The default run executes everything; its X11 step self-skips when the
# headless X stack is absent, so it stays green on a text-only buildd.
#
# The pyte tests honour $WPE_BIN (default ../wpe), so the same scenarios run
# against the in-tree binary, a sanitizer build, or an installed /usr/bin/wpe.
# The X11 GUI tests honour $XWPE_BIN (default ../xwpe).
#
# pyte/pytest: a system install (python3-pyte, python3-pytest) is used if
# present (e.g. in a Debian autopkgtest with no network); otherwise a local
# venv is bootstrapped under tests/.venv.
#
# The --x11 suite additionally needs a headless X stack -- on Debian:
#   xvfb matchbox-window-manager xdotool x11-utils imagemagick python3-pil
# A window manager (matchbox) is REQUIRED: without one, xwpe's X11 size
# handling oscillates into a resize feedback loop under bare Xvfb.  The suite
# skips cleanly (not fails) if any of these are absent.
set -e

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
cd "$top"

mode=functional
case "${1:-}" in
  ""|--functional) mode=functional ;;
  --asan)          mode=asan ;;
  --x11)           mode=x11 ;;
  --wayland)       mode=wayland ;;
  -h|--help)
    echo "Usage: tests/run-tests.sh [--asan|--x11|--wayland]"
    echo "  (no arg)  C unit (make check) + pyte suite (./wpe) + X11 GUI (./xwpe)"
    echo "  --asan    build we-asan and run the pyte suite under AddressSanitizer"
    echo "  --x11     ONLY the headless X11 GUI suite (./xwpe under Xvfb+matchbox)"
    echo "  --wayland ONLY the native Wayland GUI suite (./xwpe under Xvfb+weston)"
    echo "The pyte tests honour \$WPE_BIN (default ../wpe); X11/Wayland honour \$XWPE_BIN."
    echo "The default X11 step self-skips when the X stack is missing."
    exit 0 ;;
  *) echo "run-tests.sh: unknown option '$1' (try --help)" >&2; exit 2 ;;
esac

# --- pyte/pytest interpreter -------------------------------------------------
if python3 -c "import pyte, pytest" 2>/dev/null; then
  PY=python3
else
  VENV="$here/.venv"
  if [ ! -x "$VENV/bin/python" ]; then
    echo "== bootstrapping tests/.venv (pyte + pytest) =="
    python3 -m venv "$VENV"
    "$VENV/bin/pip" install --quiet pyte==0.8.1 pytest
  fi
  PY="$VENV/bin/python"
fi

# --- run ---------------------------------------------------------------------
case "$mode" in
functional)
  echo "== C unit tests (make check) =="
  make check
  echo "== pyte functional suite (WPE_BIN=./wpe) =="
  [ -x "$top/wpe" ] || make
  WPE_BIN="$top/wpe" "$PY" -m pytest -q "$here" --ignore="$here/x11"
  # X11 GUI suite as part of the default "run everything".  It self-skips when
  # the headless X stack (Xvfb/matchbox/xdotool/xwd/convert) or Pillow is
  # absent, so this stays green on a text-only buildd.  pytest exit 5 = "all
  # skipped / nothing collected" is tolerated; real failures propagate.
  echo "== X11 GUI suite (XWPE_BIN=./xwpe; skips if no X stack) =="
  [ -x "$top/xwpe" ] || make
  XWPE_BIN="$top/xwpe" "$PY" -m pytest -q "$here/x11" \
    || { rc=$?; [ "$rc" = 5 ] && echo "(X11 suite skipped)" || exit "$rc"; }
  ;;

asan)
  # The AddressSanitizer build catches the silent memory bugs (heap overflow,
  # use-after-free) that the functional run cannot see on a normal build.
  # ASan is ~2x slower, which the existing pyte timeouts tolerate (valgrind's
  # ~20-50x does not -- valgrind stays a targeted, custom-timeout tool; see
  # HACKING.md and the autopkgtest plan).
  echo "== building we-asan =="
  make clean >/dev/null
  make CFLAGS="-fsanitize=address -g -O1 -fno-omit-frame-pointer" \
       LDFLAGS="-fsanitize=address" >/dev/null
  cp we we-asan
  echo "== restoring the normal binary =="
  make clean >/dev/null
  make >/dev/null
  echo "== pyte suite under AddressSanitizer (WPE_BIN=./we-asan) =="
  ASAN_OPTIONS="detect_leaks=0:abort_on_error=1:log_path=$top/asan-run" \
    WPE_BIN="$top/we-asan" "$PY" -m pytest -q "$here" --ignore="$here/x11"
  ;;

x11)
  # Headless X11 GUI suite: drives a real ./xwpe under Xvfb + matchbox and
  # asserts on screenshots (see tests/x11/conftest.py).  The suite itself
  # skips cleanly if Xvfb/matchbox/xdotool/xwd/convert are missing, but we
  # still need Pillow in the interpreter to import the test module.
  if ! "$PY" -c "import PIL" 2>/dev/null; then
    case "$PY" in
      */.venv/bin/python) "$here/.venv/bin/pip" install --quiet Pillow ;;
      *) echo "run-tests.sh --x11 needs Pillow (Debian: python3-pil)" >&2; exit 2 ;;
    esac
  fi
  [ -x "$top/xwpe" ] || make
  echo "== X11 GUI suite (XWPE_BIN=./xwpe) =="
  XWPE_BIN="$top/xwpe" "$PY" -m pytest -q "$here/x11"
  ;;

wayland)
  # Headless native Wayland GUI suite: drives a real ./xwpe on a wl_surface
  # under Xvfb + weston (x11-backend, kiosk-shell) and asserts on the frame
  # PPM dumps (see tests/wayland/conftest.py).  The suite self-skips if
  # weston / wl-clipboard are missing, but Pillow must be importable to load
  # the test module.
  if ! "$PY" -c "import PIL" 2>/dev/null; then
    case "$PY" in
      */.venv/bin/python) "$here/.venv/bin/pip" install --quiet Pillow ;;
      *) echo "run-tests.sh --wayland needs Pillow (Debian: python3-pil)" >&2; exit 2 ;;
    esac
  fi
  [ -x "$top/xwpe" ] || make
  echo "== native Wayland GUI suite (XWPE_BIN=./xwpe) =="
  XWPE_BIN="$top/xwpe" "$PY" -m pytest -q "$here/wayland"
  ;;
esac
