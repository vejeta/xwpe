#!/bin/sh
# check-test-processes.sh -- a test learns about a process through procinfo.
#
# No test file builds a ps(1) argument vector of its own: tests/procinfo.py
# is the one place that reads the process table (Linux from /proc, elsewhere
# from ps), so a row means the same on every system.  The scan is
# tests/check_test_processes.py; the sites that remain are counted in
# tests/test-processes.allow and a count may only fall.
#
#   check-test-processes.sh [--selftest]
#
# The script runs the scanner's self-test (a planted offence must be named,
# a clean file must not) and then the tree.
#
# Exit: 0 clean, 1 offenders or a blind scanner, 77 no python3.

HERE=$(dirname "$0")
SRCDIR=${srcdir:-$HERE/..}
PYTHON=${PYTHON:-python3}
SKIP=77

if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "check-test-processes: SKIP, no $PYTHON"
  exit $SKIP
fi

exec "$PYTHON" "$HERE/check_test_processes.py" --srcdir "$SRCDIR" "$@"
