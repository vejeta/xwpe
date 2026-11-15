#!/bin/sh
# check-maintainability.sh -- the editor's own C stays readable as it is swept.
#
# The sweep of the 1993 code changes it one function group at a time.  What
# keeps a swept file clean, and what keeps a new feature from adding the old
# habits back, is a measurement the build repeats:
#
#  3. No `f()` declaration (a prototype with no parameter list).
#  4. No `extern` in a .c file of a symbol a header declares.
#  5. No getenv("XWPE_...") outside we_paint.c (the owner of the trace),
#     we_testdump.c, we_ai.c's configuration reader and we_main.c.
#  6. The terminal's mouse symbols stay in we_term.c: mousemask, KEY_MOUSE,
#     getmouse, MEVENT, fk_t_mouse.
#  1-2. Per-file ceilings (longest function, nesting, parameters over six,
#     duplicated blocks, statics shared by two functions) and the tree
#     ceilings of 120 lines and nesting 5.  REPORT ONLY for now: the table is
#     printed and never fails; a later pass turns them strict file by file as
#     the sweep finishes each one.
#
# Rows 3 to 5 count the sites per file in tests/maintainability.allow with
# the pass that removes them; a count may only fall.  A count above its line
# fails in every build.  A line above the measurement is stale and fails only
# in the reference configuration (the allow file's `reference` line, compared
# with the words of tests/build-config.sh): another build compiles less, so
# there it is reported, and a file of a feature the build lacks is reported
# as not built here.  The measurement is made on the
# preprocessor's output with the build's own flags (tests/maintainability.py),
# so a branch the build does not compile is not counted.
#
#   check-maintainability.sh [--strict] [--allow FILE] [--srcdir DIR] [--list]
#                            [--selftest]
#
# --selftest plants one offence per row in a scratch folder and fails unless
# the scan names it (a scanner that sees nothing is not a check).  With no
# argument the script runs the self-test and then the tree.  --list prints the
# allow lines the tree needs today.
#
# Environment (set by `make check`): MAINT_CC, MAINT_FLAGS, MAINT_SOURCES,
# WPE_BUILD_BINARY, WPE_FEATURE_SOURCES, srcdir.  Exit: 0 clean, 1 offenders
# or a blind scanner, 77 no python3 or no compiler, 99 a binary that cannot
# say what it was built with.

HERE=$(dirname "$0")
PYTHON=${PYTHON:-python3}
SKIP=77

if ! command -v "$PYTHON" >/dev/null 2>&1; then
  echo "check-maintainability: SKIP, no $PYTHON"
  exit $SKIP
fi

case ${1:-} in
  --selftest)
    exec "$PYTHON" "$HERE/maintainability.py" "$@"
    ;;
  --list)
    WPE_BUILD_QUIET=1     # the output is an allow file
    ;;
  *)
    "$PYTHON" "$HERE/maintainability.py" --selftest
    status=$?
    [ $status -eq 0 ] || exit $status
    ;;
esac

# What this build contains (tests/build-config.sh asks the binary once):
# a count below its line is stale only in the reference configuration named
# in the allow file, and a file of a feature this build lacks is not built
# here.  Sourcing prints the build's line, and exits 99 when the binary
# cannot tell.
. "$HERE/build-config.sh"
MAINT_BUILT_WITH=$(wpe_built_with)
export MAINT_BUILT_WITH
exec "$PYTHON" "$HERE/maintainability.py" "$@"
