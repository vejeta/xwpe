#!/bin/sh
# Shared post-build runtime tests for the BSD provisioners -- proves the built
# binary RUNS in both modes, not merely that it links.  Called by each
# platform's provision.sh after a successful build, so the orchestration lives
# in ONE place (the platform files only install deps and build).
#
#   1. gmake check          -- the C unit tests
#   2. sgr_mouse_probe (wpe) -- console mouse over a pty (menu opens on an SGR click)
#   3. xvfb_smoke.sh (xwpe)  -- X11 renders a real UI under Xvfb (via XWPE_X_DUMP)
#
# Usage: run_tests.sh /path/to/build-tree
set -u

DIR="${1:?usage: run_tests.sh /path/to/build-tree}"
cd "$DIR" || { echo "run_tests: cannot cd $DIR"; exit 1; }
RC=0

echo "=== unit tests (gmake check) ==="
if gmake check >/tmp/xwpe-check.log 2>&1; then
  echo "make check: PASS"
else
  echo "make check: FAIL"; grep -iE "FAIL:|error:" /tmp/xwpe-check.log | head; RC=1
fi

echo "=== console-mouse probe (wpe under a pty) ==="
if cc -O2 -o /tmp/probe contrib/bsd-test/sgr_mouse_probe.c -lutil 2>/dev/null \
   && /tmp/probe ./we xterm-256color; then
  echo "wpe mouse probe: PASS"
else
  echo "wpe mouse probe: FAIL"; RC=1
fi

echo "=== headless X11 smoke (xwpe under Xvfb) ==="
sh contrib/bsd-test/xvfb_smoke.sh "$DIR" || RC=1

[ "$RC" = 0 ] && echo "ALL RUNTIME TESTS PASSED" || echo "SOME RUNTIME TESTS FAILED"
exit "$RC"
