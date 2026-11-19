#!/bin/sh
# check-test-threads.sh -- a test launches an editor from the main thread.
#
# The process owner (tests/gui_owner.py) refuses a launch off the main
# thread, because an editor's death signal follows the thread that forked it.
# No test file may start a session from a thread pool or a thread: the scan
# is tests/check_test_threads.py; the rows that prove the refusal are counted
# in tests/test-threads.allow and a count may only fall.
#
#   check-test-threads.sh [--selftest]
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
  echo "check-test-threads: SKIP, no $PYTHON"
  exit $SKIP
fi

exec "$PYTHON" "$HERE/check_test_threads.py" --srcdir "$SRCDIR" "$@"
