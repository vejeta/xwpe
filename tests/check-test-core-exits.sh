#!/bin/sh
# check-test-core-exits.sh -- a C test ends a child by a core signal only
# after the child gave up its core file.
#
# abort(), raise() of a core signal, a core signal put back to SIG_DFL: each
# stands in a function that first calls test_child_dies_on_purpose()
# (tests/test_watchdog.h), or is listed in tests/test-core-exits.allow with
# its reason and a count that may only fall.  The scan is
# tests/check_test_core_exits.py.
#
#   check-test-core-exits.sh [--selftest]
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
  echo "check-test-core-exits: SKIP, no $PYTHON"
  exit $SKIP
fi

exec "$PYTHON" "$HERE/check_test_core_exits.py" --srcdir "$SRCDIR" "$@"
