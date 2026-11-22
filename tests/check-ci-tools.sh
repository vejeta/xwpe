#!/bin/sh
# check-ci-tools.sh -- the Linux job installs what the rows need, and says so.
#
# tests/ci-tools.txt maps each host tool a row needs to its Ubuntu package and
# its kind.  This script (tests/check_ci_tools.py) fails when a tool a manifest
# row declares has no line there, when an installed tool's package is not in
# the job's apt-get list or its "run:" command is missing, or when the job's
# XWPE_TEST_TOOLS_EXPECTED differs from the tools it installs.
#
#   check-ci-tools.sh [--selftest]
#
# Exit: 0 clean, 1 findings or a wrong canary, 77 no python3 or no .github (a
# source tarball does not carry it).

HERE=$(dirname "$0")
SRCDIR=${srcdir:-$HERE/..}
PYTHON=${PYTHON:-python3}
SKIP=77

if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "check-ci-tools: SKIP, no $PYTHON"
  exit $SKIP
fi

exec "$PYTHON" "$HERE/check_ci_tools.py" --srcdir "$SRCDIR" "$@"
