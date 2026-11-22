#!/bin/sh
# check-test-waits.sh -- one owner decides how long a wait lasts.
#
# A row's wait is stretched by XWPE_TEST_WAIT_SCALE in one place:
# tests/gui_owner.py reads the variable and tests/wpe_driver.py makes the
# deadline (wait_deadline, wait_screen).  The scan is
# tests/check_test_waits.py: no other file reads the variable, runs a wait
# loop of its own, or hands the owner a time that is already stretched.
#
#   check-test-waits.sh [--selftest]
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
  echo "check-test-waits: SKIP, no $PYTHON"
  exit $SKIP
fi

exec "$PYTHON" "$HERE/check_test_waits.py" --srcdir "$SRCDIR" "$@"
