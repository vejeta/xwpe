#!/bin/sh
# check-seen-rows.sh -- a reuse row's SKIP or FAIL is decided in one place.
#
# A row that needs a file system to give a freed inode number back either
# reaches the number (RAN), or the file system never gives that kind of number
# back (SKIP, with what was measured), or it does and the row proved nothing
# (FAIL).  That decision is tests/seen_rows.h (seen_rows_reuse_or_skip, and
# seen_rows_missed for a row that walks by itself).  A row that decides it on
# its own prints its own SKIP or FAIL line and drifts from the others: the
# file rows of FreeBSD ran and the link rows were reported as failures because
# one caller asked about a file where it walked with a link.
#
#   check-seen-rows.sh [--dir TESTSDIR] [--selftest]
#
# Rules (comments are blanked first, string contents kept):
#  1. No tests/*.c or tests/*.h except seen_rows.h holds one of the words the
#     decision prints: "never gives a number back", "gives numbers back",
#     "number was not reached", "did not come back".
#  2. No tests/*.c or tests/*.h except seen_rows.h calls seen_rows_probe(,
#     seen_rows_never_gives_back( or seen_rows_decide_miss( with the aim of
#     deciding a SKIP: a row calls seen_rows_reuse_or_skip or seen_rows_missed.
#     tests/test_seen.c is the one test that measures the probe itself (the
#     row that asks ext4 for each kind, and the canary of the hook).
#
# --selftest feeds fixtures with one offence of each rule and files that must
# pass, and fails unless the scan names each offence and only those.
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
TESTSDIR=
SELFTEST=0

while [ $# -gt 0 ]; do
  case $1 in
    --dir) shift; TESTSDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-seen-rows: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done
[ -n "$TESTSDIR" ] || TESTSDIR=$SRCDIR/tests

. "$HERE/workdir.sh"
work_dir seen-rows 1

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")

# Mode 1 keeps the contents of literals: rule 1 reads them.
SCAN_AWK='
FNR == 1 { blank_incom = 0 }
{
  line = blank_c($0, 1)
  if (line ~ /never gives a number back|gives numbers back|number was not reached|did not come back/)
    print file ":" FNR ": rule 1: the words of the SKIP or FAIL decision belong to tests/seen_rows.h"
  if (rule2 && line ~ /(^|[^A-Za-z0-9_])seen_rows_(probe|never_gives_back|decide_miss)[ \t]*[(]/)
    print file ":" FNR ": rule 2: the probe is asked by tests/seen_rows.h; call seen_rows_reuse_or_skip or seen_rows_missed"
}
'

# scan_file FILE NAME: both rules over FILE (seen_rows.h excepted; rule 2 not
# for the test of the probe itself).
scan_file() {
  case $2 in seen_rows.h) return 0 ;; esac
  rule2=1
  case $2 in test_seen.c) rule2=0 ;; esac
  awk -v file="$2" -v rule2="$rule2" "$BLANK_C""$SCAN_AWK" "$1"
}

scan_dir() {
  n=0
  for f in "$1"/*.c "$1"/*.h; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    scan_file "$f" "${f##*/}"
  done
  echo "$n" > "$WORK/count"
}

selftest_failed=0
selftest_expect() {
  what=$1 out=$2
  shift 2
  ok=1
  for pat in "$@"; do
    printf '%s\n' "$out" | grep -qF -- "$pat" || { ok=0; echo "  output lacks: $pat"; }
  done
  if [ $ok = 1 ]; then
    echo "ok: $what"
  else
    echo "FAIL: $what"
    printf '%s\n' "$out" | sed 's/^/    | /'
    selftest_failed=1
  fi
}

selftest() {
  fx=$WORK/fx
  mkdir -p "$fx"
  printf '%s\n' 'static void f(void) { printf("SKIP: row: this file system never gives a number back\n"); }' > "$fx/test_bad_words.c"
  printf '%s\n' 'static void f(void) { printf("FAIL: row: the number was not reached\n"); }' > "$fx/bad_reached.h"
  printf '%s\n' 'static int f(const char *d, const SeenRowsCase *pc) { return seen_rows_never_gives_back (d, pc); }' > "$fx/test_bad_probe.c"
  printf '%s\n' 'static int f(const char *d, const SeenRowsCase *pc) { return seen_rows_never_gives_back(d, pc); }' > "$fx/test_seen.c"
  printf '%s\n' '/* never gives a number back */' 'static int f(void) { return seen_rows_reuse_or_skip(0, 0, 0, 0); }' > "$fx/test_good.c"
  printf '%s\n' 'static const char *s = "never gives a number back"; static int g(void) { return seen_rows_probe(0, 0, 0); }' > "$fx/seen_rows.h"
  out=$(scan_dir "$fx")
  selftest_expect "the words of the decision outside the owner" "$out" "test_bad_words.c:1: rule 1"
  selftest_expect "another of the words" "$out" "bad_reached.h:1: rule 1"
  selftest_expect "the probe asked outside the owner" "$out" "test_bad_probe.c:1: rule 2"
  for good in test_seen.c test_good.c seen_rows.h; do
    if printf '%s\n' "$out" | grep -q "^$good:"; then
      echo "FAIL: $good must pass, and the scan named it:"
      printf '%s\n' "$out" | grep "^$good:" | sed 's/^/    | /'
      selftest_failed=1
    else
      echo "ok: $good passes"
    fi
  done
  if [ $selftest_failed = 0 ]; then
    echo "check-seen-rows: selftest ok"
    return 0
  fi
  echo "check-seen-rows: FAIL (selftest)"
  return 1
}

if [ $SELFTEST = 1 ]; then
  selftest
  exit $?
fi

out=$(scan_dir "$TESTSDIR")
scanned=$(cat "$WORK/count")
status=0
if [ "${scanned:-0}" -eq 0 ]; then
  echo "check-seen-rows: FAIL: no test file was read under $TESTSDIR (the scan is blind)"
  status=1
fi
if [ -n "$out" ]; then
  echo "check-seen-rows: FAIL: a row decides its own SKIP or FAIL of a reuse:"
  printf '%s\n' "$out" | sed 's/^/  /'
  status=1
fi
selftest_out=$(sh "$0" --selftest 2>&1) \
  || { echo "check-seen-rows: FAIL: canary: the scan is blind (run $0 --selftest)"; printf '%s\n' "$selftest_out" | sed 's/^/  | /'; status=1; }
[ $status = 0 ] && echo "check-seen-rows: ok ($scanned files)"
exit $status
