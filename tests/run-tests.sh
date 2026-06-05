#!/bin/sh
# run-tests.sh -- run the xwpe test suite (the local equivalent of the
# Debian autopkgtest).  The Debian autopkgtest is expected to call this same
# script, so the scenarios stay in one place; on the Debian side only the
# test Depends need maintaining.
#
# Usage:
#   tests/run-tests.sh            # C unit tests + pyte functional suite (./wpe)
#   tests/run-tests.sh --asan     # build we-asan and run the pyte suite under it
#   tests/run-tests.sh --help
#
# The pyte tests honour $WPE_BIN (default ../wpe), so the same scenarios run
# against the in-tree binary, a sanitizer build, or an installed /usr/bin/wpe.
#
# pyte/pytest: a system install (python3-pyte, python3-pytest) is used if
# present (e.g. in a Debian autopkgtest with no network); otherwise a local
# venv is bootstrapped under tests/.venv.
set -e

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
cd "$top"

mode=functional
case "${1:-}" in
  ""|--functional) mode=functional ;;
  --asan)          mode=asan ;;
  -h|--help)
    echo "Usage: tests/run-tests.sh [--asan]"
    echo "  (no arg)  C unit tests (make check) + pyte functional suite (./wpe)"
    echo "  --asan    build we-asan and run the pyte suite under AddressSanitizer"
    echo "The pyte tests honour \$WPE_BIN (default ../wpe)."
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
  WPE_BIN="$top/wpe" "$PY" -m pytest -q "$here"
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
    WPE_BIN="$top/we-asan" "$PY" -m pytest -q "$here"
  ;;
esac
