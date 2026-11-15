#!/bin/sh
# check-bsd-known-failures.sh -- the BSD CI's known-failure list does its job.
#
# Runs tests/bsd-known-failures.sh --selftest, which feeds the script fixture
# logs and checks that it reports a new failure, a stale entry, a failing
# test with no log of its own, a log with no summary and a failed check, and
# that the CI body copies the per-test logs before it calls the script.
# Run on Linux so a change to the list or its logic is proven before a BSD run.
#
# Exit: 0 pass, 1 a case failed.

SRCDIR=${srcdir:-.}
exec sh "$SRCDIR/tests/bsd-known-failures.sh" --selftest
