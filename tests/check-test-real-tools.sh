#!/bin/sh
# check-test-real-tools.sh -- no test row reaches a tool of the machine by a
# door the tool farm does not cover.
#
# tests/gui_owner.py gives every process it starts the PATH of a tool farm
# (tests/engine_guard.py).  This check scans every test file for the other
# doors: a program started outside the owner, the editor started outside the
# owner, the live-only opt-out and API-key inheriting in a file that is not
# live, and an engine named by an absolute path.  The sites that remain are
# counted in tests/test-real-tools.allow and a count may only fall.
#
#   check-test-real-tools.sh [--selftest]
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
  echo "check-test-real-tools: SKIP, no $PYTHON"
  exit $SKIP
fi

exec "$PYTHON" "$HERE/check_test_real_tools.py" --srcdir "$SRCDIR" "$@"
