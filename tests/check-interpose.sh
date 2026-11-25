#!/bin/sh
# check-interpose.sh -- a C test finds the C library's own function behind
# its hook in one place, and its output reaches the log line by line.
#
# A test hooks a C library function (fstat, open, fsync ...) by defining one
# of that name, and passes on to the library's through tests/interpose.h.
# Looking the function up by its C spelling is wrong where the system binds
# the call to another name: on NetBSD the library's "fstat" is an old-ABI
# wrapper that calls __fstat50, which is the hook, and the test ended in a
# stack overflow before it printed a row.  tests/interpose.h asks the binary
# which name the hook was exported under, so no test names a function by
# string.  The same crash lost every line of the test's output, because a
# file is buffered in blocks: tests/linebuf.h makes stdout line-buffered.
#
#   check-interpose.sh [--dir TESTSDIR] [--makefile FILE] [--selftest]
#
# Rules (comments and strings are blanked first):
#  1. No tests/*.c or tests/*.h except interpose.h calls dlsym( or names
#     RTLD_NEXT or RTLD_DEFAULT.
#  2. Every tests/test_*.c that has a main includes linebuf.h, directly or
#     through test_watchdog.h, testdir.h, seen_rows.h, interpose.h or
#     fakechmod.h.
#  3. A program whose test includes interpose.h or fakechmod.h links with
#     TEST_EXPORT_DYNAMIC (NAME_LDFLAGS in Makefile.am), or dladdr cannot see
#     its hooks.
#
# --selftest feeds fixtures with one offence of each rule and files that must
# pass, and fails unless the scan names each offence and only those (a scanner
# that sees nothing is not a check).
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
TESTSDIR=
MAKEFILE=
SELFTEST=0

while [ $# -gt 0 ]; do
  case $1 in
    --dir) shift; TESTSDIR=$1 ;;
    --makefile) shift; MAKEFILE=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-interpose: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done
[ -n "$TESTSDIR" ] || TESTSDIR=$SRCDIR/tests
[ -n "$MAKEFILE" ] || MAKEFILE=$SRCDIR/Makefile.am

. "$(dirname "$0")/workdir.sh"
work_dir interpose 1

BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# The scanner of rule 1: one line per offence, "file:line: rule 1: what".
# Rule 2 is read by scan_rule2 and rule 3 by scan_links.
SCAN_AWK='
FNR == 1 { blank_incom = 0 }
{
  line = blank_c($0, 0)
  if (line ~ /(^|[^A-Za-z0-9_])dlsym[ \t]*[(]/ || line ~ /RTLD_NEXT/ || line ~ /RTLD_DEFAULT/)
    print file ":" FNR ": rule 1: looks a function up by name; use WPE_REAL of tests/interpose.h"
}
'

# scan_rule1 FILE NAME: rule 1 over the lines of FILE (interpose.h excepted).
scan_rule1() {
  case $2 in interpose.h) return 0 ;; esac
  awk -v file="$2" "$BLANK_C""$SCAN_AWK" "$1"
}

# scan_rule2 FILE NAME: a test with a main and no line-buffering header.
scan_rule2() {
  case $2 in test_*.c) ;; *) return 0 ;; esac
  main_at=$(grep -n -E '^(int[[:space:]]+)?main[[:space:]]*\(' "$1" | head -1)
  [ -n "$main_at" ] || return 0
  grep -q -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*"(linebuf|test_watchdog|testdir|seen_rows|interpose|fakechmod)\.h"' "$1" && return 0
  echo "$2:${main_at%%:*}: rule 2: has a main and does not include linebuf.h (stdout is buffered in blocks, a crash loses the rows before it)"
}

# scan_links DIR MAKEFILE: rule 3, one line per program that hooks and does
# not link with the export flag.
scan_links() {
  for f in "$1"/test_*.c; do
    [ -f "$f" ] || continue
    grep -q -E '^[[:space:]]*#[[:space:]]*include[[:space:]]*"(interpose|fakechmod)\.h"' "$f" || continue
    prog=${f##*/}
    prog=${prog%.c}
    grep -q -E "^${prog}_LDFLAGS[[:space:]]*=.*TEST_EXPORT_DYNAMIC" "$2" \
      || echo "${f##*/}:0: rule 3: hooks a function and ${prog}_LDFLAGS does not name TEST_EXPORT_DYNAMIC"
  done
}

# The hooks glibc's _FORTIFY_SOURCE can bypass, each with the twin macro of
# tests/interpose.h that must follow it: "name TWIN_MACRO".
FORTIFIED_HOOKS='read WPE_HOOK_TWIN_READ
readlink WPE_HOOK_TWIN_READLINK
ttyname_r WPE_HOOK_TWIN_TTYNAME_R
open WPE_HOOK_TWIN_OPEN
openat WPE_HOOK_TWIN_OPENAT'

# scan_rule4 FILE NAME: rule 4, a test defines a hook of a name a fortified
# build calls by its checked twin, and does not place that twin.  A fortified
# product (the CI's compiler, a distribution's package build) never reaches
# the plain hook, and the row tests nothing.
scan_rule4() {
  case $2 in test_*.c) ;; *) return 0 ;; esac
  printf '%s\n' "$FORTIFIED_HOOKS" | while read -r hook twin; do
    at=$(grep -n -E "^[a-z_]+[ *]+${hook}[[:space:]]*\(" "$1" | head -1)
    [ -n "$at" ] || continue
    grep -q -E "^[[:space:]]*${twin}[[:space:]]*\$" "$1" && continue
    echo "$2:${at%%:*}: rule 4: hooks ${hook} and does not place ${twin} (a fortified build calls the checked twin and never reaches the hook)"
  done
}

# scan_dir DIR MAKEFILE: every offence on stdout; the number of files read is
# in $WORK/count.
scan_dir() {
  n=0
  for f in "$1"/*.c "$1"/*.h; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    scan_rule1 "$f" "${f##*/}"
    scan_rule2 "$f" "${f##*/}"
    scan_rule4 "$f" "${f##*/}"
  done
  scan_links "$1" "$2"
  echo "$n" > "$WORK/count"
}

# selftest_expect WHAT OUTPUT PATTERN...: OUTPUT holds every PATTERN.
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
  printf '%s\n' '#include <dlfcn.h>' 'void *f(void) { return dlsym(RTLD_NEXT, "fstat"); }' > "$fx/test_bad_dlsym.c"
  printf '%s\n' '#include <dlfcn.h>' 'void *f(void) { return dlsym (0, "x"); }' > "$fx/bad_spaced.h"
  printf '%s\n' 'static void *f = RTLD_DEFAULT;' > "$fx/bad_default.h"
  printf '%s\n' '/* once found by dlsym(RTLD_NEXT, "x") */' 'static const char *s = "dlsym(";' 'static int g;' > "$fx/good_words.h"
  printf '%s\n' 'static void *f(void) { return dlsym(RTLD_NEXT, "x"); }' > "$fx/interpose.h"
  printf '%s\n' '#include <stdio.h>' 'int main(void) { return 0; }' > "$fx/test_bad_nobuf.c"
  printf '%s\n' '#include <stdio.h>' '#include "linebuf.h"' 'int main(void) { return 0; }' > "$fx/test_good_buf.c"
  printf '%s\n' '#include "test_watchdog.h"' 'int main(void) { return 0; }' > "$fx/test_good_dog.c"
  printf '%s\n' '#include "interpose.h"' '#include "linebuf.h"' 'int main(void) { return 0; }' > "$fx/test_bad_link.c"
  printf '%s\n' '#include "interpose.h"' '#include "linebuf.h"' 'int main(void) { return 0; }' > "$fx/test_good_link.c"
  printf '%s\n' 'static int helper(void) { return 0; }' > "$fx/helper.c"
  printf '%s\n' '#include "linebuf.h"' 'ssize_t read(int fd, void *b, size_t n)' '{ return 0; }' 'int main(void) { return 0; }' > "$fx/test_bad_notwin.c"
  printf '%s\n' '#include "linebuf.h"' 'ssize_t read(int fd, void *b, size_t n)' '{ return 0; }' 'WPE_HOOK_TWIN_READ' 'int main(void) { return 0; }' > "$fx/test_good_twin.c"
  printf '%s\n' '#include "linebuf.h"' 'static ssize_t read_all(int fd)' '{ return 0; }' 'int main(void) { return 0; }' > "$fx/test_good_name.c"
  printf '%s\n' 'test_good_link_LDFLAGS = $(TEST_EXPORT_DYNAMIC)' 'test_bad_link_LDFLAGS =' > "$fx/Makefile.am"
  out=$(scan_dir "$fx" "$fx/Makefile.am")
  selftest_expect "a dlsym of a C spelling" "$out" "test_bad_dlsym.c:2: rule 1"
  selftest_expect "a dlsym with a blank before the paren" "$out" "bad_spaced.h:2: rule 1"
  selftest_expect "RTLD_DEFAULT outside the owner" "$out" "bad_default.h:1: rule 1"
  selftest_expect "a test with a main and no line buffering" "$out" "test_bad_nobuf.c:2: rule 2"
  selftest_expect "a hooking test that does not link the export flag" "$out" \
    "test_bad_link.c:0: rule 3: hooks a function and test_bad_link_LDFLAGS does not name TEST_EXPORT_DYNAMIC"
  selftest_expect "a hook of read with no fortified twin" "$out" \
    "test_bad_notwin.c:2: rule 4: hooks read and does not place WPE_HOOK_TWIN_READ"
  for good in good_words.h interpose.h test_good_buf.c test_good_dog.c test_good_link.c helper.c test_good_twin.c test_good_name.c; do
    if printf '%s\n' "$out" | grep -q "^$good:"; then
      echo "FAIL: $good must pass, and the scan named it:"
      printf '%s\n' "$out" | grep "^$good:" | sed 's/^/    | /'
      selftest_failed=1
    else
      echo "ok: $good passes"
    fi
  done
  if [ $selftest_failed = 0 ]; then
    echo "check-interpose: selftest ok"
    return 0
  fi
  echo "check-interpose: FAIL (selftest)"
  return 1
}

if [ $SELFTEST = 1 ]; then
  selftest
  exit $?
fi

out=$(scan_dir "$TESTSDIR" "$MAKEFILE")
scanned=$(cat "$WORK/count")
status=0
if [ "${scanned:-0}" -eq 0 ]; then
  echo "check-interpose: FAIL: no test file was read under $TESTSDIR (the scan is blind)"
  status=1
fi
if [ -n "$out" ]; then
  echo "check-interpose: FAIL: a test goes round the one owner of its hooks or of its log:"
  printf '%s\n' "$out" | sed 's/^/  /'
  status=1
fi
selftest_out=$(sh "$0" --selftest 2>&1) \
  || { echo "check-interpose: FAIL: canary: the scan is blind (run $0 --selftest)"; printf '%s\n' "$selftest_out" | sed 's/^/  | /'; status=1; }
[ $status = 0 ] && echo "check-interpose: ok ($scanned files)"
exit $status
