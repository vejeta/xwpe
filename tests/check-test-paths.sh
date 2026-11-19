#!/bin/sh
# check-test-paths.sh -- a test does not depend on how long its folder's path is.
#
# The folder a test works in is under $TMPDIR, and TMPDIR may be any length: a
# build tree deep in a home folder, a CI runner with a long workspace.  A C
# test that copies that path into a buffer sized for a short one, binds a Unix
# socket by its full path (a socket name holds about 100 bytes), or joins a
# name onto it with a bare snprintf, passes at 24 bytes and fails or, worse,
# works on a cut path in another folder at 120 or 300.  tests/testdir.h is the
# one owner: it makes the folder and refuses a cut template (test_dir_make),
# joins a name or says it does not fit (test_path_join), and plants a socket by
# its name inside its folder (test_plant_socket).  This check fails a test that
# goes around it.  `make check-path-lengths` runs the C tests at 120 and 300
# bytes, which is what the static rules cannot see.
#
#   check-test-paths.sh [--srcdir DIR] [--selftest]
#
# Rules over the C tests (tests/*.c and tests/*.h; comments are blanked, a
# literal keeps its text).  The rules of the C half:
#  T1. sun_path appears only inside the socket helpers of tests/testdir.h
#      (the functions named test_plant_socket*).
#  T2. In a test that includes testdir.h or seen_rows.h, the buffer given to
#      test_dir_make, test_dir_make_under and seen_rows_dir is an array
#      declared in the file with TEST_DIR_PATH_SIZE.
#  T3. In those files, and in testdir.h outside test_path_join, no snprintf or
#      sprintf joins onto dir, other or base with a format that begins "%s/":
#      it goes through test_path_join.  The folder template of testdir.h
#      (".XXXXXX") is the owner's own and is judged by T4.
#  T4. tests/testdir.h reads the result of the snprintf that writes the
#      folder template, so a cut template cannot reach mkdtemp.
#
# Rules over the pytest rows (P1 to P3) are added to this file by their own
# pass; the section PYTEST RULES below is kept for them.
#
# --selftest feeds fixtures with one offence of each rule and files that must
# pass, and fails unless the scan names each offence and only those (a scanner
# that sees nothing is not a check).  The same run happens at the start of
# every run.
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
TESTSDIR=
SELFTEST=0

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-test-paths: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done
TESTSDIR=$SRCDIR/tests

. "$(dirname "$0")/workdir.sh"
work_dir test-paths 1

# The one blanker of comments (tests/blank-c.awk).
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# The scanner for one C file.  -v file=NAME (as shown), -v uses=1 when the file
# includes testdir.h or seen_rows.h (or is one of them).  Output: one line per
# offence, "file:line: rule: what".
C_AWK='
function say(rule, msg) { print file ":" FNR ": " rule ": " msg; bad++ }
function trim(s) { gsub(/^[ \t]+/, "", s); gsub(/[ \t]+$/, "", s); return s }
# The N-th argument (from 1) of the call whose text after "(" is REST.
function nth_arg(rest, want,   i, n, depth, c, k, start) {
  depth = 0; k = 1; start = 1; n = length(rest)
  for (i = 1; i <= n; i++) {
    c = substr(rest, i, 1)
    if (c == "(") depth++
    else if (c == ")") {
      if (depth == 0) break
      depth--
    } else if (c == "," && depth == 0) {
      if (k == want) break
      k++; start = i + 1
    }
  }
  if (k != want) return ""
  return trim(substr(rest, start, i - start))
}
# Note the buffer of one call of NAME at this line, taking its N-th argument.
function note_call(line, re, want,   rest) {
  if (match(line, re)) {
    rest = substr(line, RSTART + RLENGTH)
    calls++
    call_arg[calls] = nth_arg(rest, want)
    call_line[calls] = FNR
  }
}
BEGIN {
  blank_incom = 0
  base = file; sub(/.*\//, "", base)
  is_owner = (base == "testdir.h")
  is_rows = (base == "seen_rows.h")
}
{
  line = blank_c($0, 1)
  if (is_owner) {
    if (line ~ /^static[^(]*test_plant_socket[A-Za-z_]*[ \t]*\(/) in_plant = 1
    if (line ~ /^static[^(]*test_path_join[ \t]*\(/) in_join = 1
  }
  if (index(line, "sun_path") && !in_plant)
    say("T1", "sun_path outside the socket helpers of testdir.h: plant a socket with test_plant_socket")
  # T3 reads a whole statement: from the line that opens a printf call to the
  # line that holds its ";", joined with blanks, so a call split over lines is
  # judged as one.
  if (uses && stmt == "" && !in_join && line ~ /(^|[^A-Za-z0-9_])s?n?printf[ \t]*[(]/) {
    stmt_line = FNR
    stmt_lines = 0
  }
  if (stmt_line) {
    stmt = stmt " " line
    stmt_lines++
    if (index(line, ";") || stmt_lines > 12) {
      if (stmt ~ /(^|[^A-Za-z0-9_])s?n?printf[ \t]*[(][^"]*"%s\/[^"]*"[ \t]*,[ \t]*(dir|other|base)[ \t]*[,)]/ && !(is_owner && index(stmt, ".XXXXXX")))
        print file ":" stmt_line ": T3: a join onto dir, other or base with a bare printf: use test_path_join"
      if (stmt ~ /(^|[^A-Za-z0-9_])s?n?printf[ \t]*[(][^"]*"%s\/[^"]*"[ \t]*,[ \t]*(dir|other|base)[ \t]*[,)]/ && !(is_owner && index(stmt, ".XXXXXX"))) bad++
      stmt = ""; stmt_line = 0
    }
  }
  if (uses && !is_owner && !is_rows) {
    note_call(line, "(^|[^A-Za-z0-9_])test_dir_make[ \t]*[(]", 1)
    note_call(line, "(^|[^A-Za-z0-9_])test_dir_make_under[ \t]*[(]", 2)
    note_call(line, "(^|[^A-Za-z0-9_])seen_rows_dir[ \t]*[(]", 1)
    t = line
    while (match(t, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\[[ \t]*TEST_DIR_PATH_SIZE[ \t]*\]/)) {
      d = substr(t, RSTART, RLENGTH); sub(/[ \t]*\[.*/, "", d)
      decl[d] = 1
      t = substr(t, RSTART + RLENGTH)
    }
  }
  if (is_owner && index(line, ".XXXXXX") && line ~ /printf[ \t]*\(/ && line !~ /=[ \t]*s?n?printf[ \t]*\(/)
    say("T4", "the folder template is written without reading snprintf'"'"'s result: a cut template reaches mkdtemp")
  if (line ~ /^}/) { in_plant = 0; in_join = 0 }
}
END {
  for (i = 1; i <= calls; i++) {
    a = call_arg[i]
    if (a !~ /^[A-Za-z_][A-Za-z0-9_]*$/)
      print file ":" call_line[i] ": T2: the folder buffer is not a plain array name: " a
    else if (!(a in decl))
      print file ":" call_line[i] ": T2: " a " is not declared with TEST_DIR_PATH_SIZE"
    else
      continue
    bad++
  }
  exit bad ? 1 : 0
}
'

# ---- PYTEST RULES (P1 to P3): the pass that adds them puts them here. ----

# scan_dir DIR: every C file of the rules, offences on stdout; the number of
# files read goes to $WORK/count.
scan_dir() {
  n=0
  for f in "$1"/*.c "$1"/*.h; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    name=${f#"$1"/}
    uses=0
    case $name in
      testdir.h|seen_rows.h) uses=1 ;;
      *) grep -Eq '^[ 	]*#[ 	]*include[ 	]*"(testdir|seen_rows)\.h"' "$f" && uses=1 ;;
    esac
    awk -v file="$name" -v uses="$uses" "$BLANK_C
$C_AWK" "$f"
  done
  echo "$n" > "$WORK/count"
}

selftest_failed=0
# selftest_expect WHAT OUTPUT PATTERN...: OUTPUT holds every PATTERN.
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
    printf '%s\n' "$out" | while IFS= read -r l; do echo "    | $l"; done
    selftest_failed=1
  fi
}

selftest() {
  fx=$WORK/fx
  mkdir -p "$fx"
  printf '%s\n' '#include <sys/un.h>' 'static void f(struct sockaddr_un *a) { strcpy(a->sun_path, "x"); }' > "$fx/bad_sun.c"
  printf '%s\n' '#include "testdir.h"' 'static char dir[256];' \
    'static void m(void) { test_dir_make(dir, sizeof dir, "x"); }' > "$fx/bad_buf.c"
  printf '%s\n' '#include "testdir.h"' 'static char dir[TEST_DIR_PATH_SIZE];' \
    'static char other[512];' \
    'static void m(void) { test_dir_make_under("/dev/shm", other, sizeof other, "x"); }' > "$fx/bad_under.c"
  printf '%s\n' '#include "testdir.h"' \
    'static void m(char *p, const char *dir) { snprintf(p, 9, "%s/x", dir); }' > "$fx/bad_join.c"
  printf '%s\n' '#include "testdir.h"' \
    'static void m(char *p, const char *other)' '{' \
    ' snprintf(p,' '          9,' '          "%s/x",' '          other);' '}' > "$fx/bad_multi.c"
  printf '%s\n' '#include "testdir.h"' 'static char dir[TEST_DIR_PATH_SIZE];' \
    'static char folder[64];' \
    '/* a socket name lives in sun_path; a join "%s/x" is snprintf(p, 9, "%s/x", dir) */' \
    'static void m(char *p, const char *name) {' \
    ' test_dir_make(dir, sizeof dir, "x");' \
    ' test_path_join(p, 9, dir, name);' \
    ' snprintf(p, 9, "%s/x", folder);' \
    ' snprintf(p, 9, "%s/%s", name, name); }' > "$fx/good.c"
  printf '%s\n' '#include <stdio.h>' 'static void m(char *p, const char *dir) { snprintf(p, 9, "%s/x", dir); }' > "$fx/good_unrelated.c"
  printf '%s\n' \
    'static int test_path_join(char *o, size_t n, const char *dir, const char *name)' \
    '{' \
    ' return snprintf(o, n, "%s/%s", dir, name);' \
    '}' \
    'static int test_plant_socket_named(const char *name)' \
    '{' \
    ' memcpy(a.sun_path, name, 3);' \
    '}' \
    'static int other(void)' \
    '{' \
    ' memcpy(a.sun_path, "x", 1);' \
    ' snprintf(dir, n, "%s/%s.XXXXXX", base, prefix);' \
    ' snprintf(out, n, "%s/%s", dir, name);' \
    '}' > "$fx/testdir.h"
  out=$(scan_dir "$fx")
  selftest_expect "a sun_path in a test" "$out" "bad_sun.c:2: T1"
  selftest_expect "a folder buffer of a number" "$out" "bad_buf.c:3: T2: dir is not declared with TEST_DIR_PATH_SIZE"
  selftest_expect "a folder buffer of a number given to test_dir_make_under" "$out" \
    "bad_under.c:4: T2: other is not declared with TEST_DIR_PATH_SIZE"
  selftest_expect "a bare join onto dir" "$out" "bad_join.c:2: T3"
  selftest_expect "a bare join split over lines" "$out" "bad_multi.c:4: T3"
  selftest_expect "a sun_path outside the socket helpers of the owner" "$out" "testdir.h:11: T1"
  selftest_expect "a folder template written unread" "$out" "testdir.h:12: T4"
  selftest_expect "a bare join onto dir in the owner" "$out" "testdir.h:13: T3"
  for good in good.c good_unrelated.c; do
    if printf '%s\n' "$out" | grep -q "^$good:"; then
      echo "FAIL: $good must pass, and the scan named it:"
      printf '%s\n' "$out" | grep "^$good:" | while IFS= read -r l; do echo "    | $l"; done
      selftest_failed=1
    else
      echo "ok: $good passes"
    fi
  done
  for exact in "testdir.h:3:" "testdir.h:7:" "testdir.h:12: T3"; do
    if printf '%s\n' "$out" | grep -q "^$exact"; then
      echo "FAIL: the owner's own line $exact must pass, and the scan named it"
      selftest_failed=1
    else
      echo "ok: the owner's own line $exact passes"
    fi
  done
  if [ $selftest_failed = 0 ]; then
    echo "check-test-paths: selftest ok"
    return 0
  fi
  echo "check-test-paths: FAIL (selftest)"
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
  echo "check-test-paths: FAIL: no test file was read under $TESTSDIR (the scan is blind)"
  status=1
fi
if [ -n "$out" ]; then
  echo "check-test-paths: FAIL: a test depends on the length of its folder's path:"
  printf '%s\n' "$out" | while IFS= read -r l; do echo "  $l"; done
  status=1
fi
selftest_out=$(sh "$0" --selftest 2>&1) \
  || { echo "check-test-paths: FAIL: canary: the scan is blind (run $0 --selftest); it said:"; printf '%s\n' "$selftest_out" | sed 's/^/  | /'; status=1; }
[ $status = 0 ] && echo "check-test-paths: ok ($scanned files)"
exit $status
