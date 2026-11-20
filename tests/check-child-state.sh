#!/bin/sh
# check-child-state.sh -- a test asks "is this process alive" in one place.
#
# kill(pid, 0) answers "does the pid exist", not "is the process running":
# an unreaped zombie exists.  On NetBSD a launcher that had ended was said to
# be alive until something waited for it, while Linux's /proc said "gone", so
# one row passed on one system and failed on another.  The reader of the
# process table is tests/child_state.h in C and tests/gui_owner.py
# (process_running, over tests/procinfo.py) in Python.  Every question of the
# form "has it ended" or "is it reaped" goes through them.
#
#   check-child-state.sh [--dir TESTSDIR] [--selftest]
#
# Rules (comments and strings are blanked first):
#  1. No tests/*.c or tests/*.h except child_state.h calls kill or killpg
#     with the signal 0.  The call may be spaced out or split over up to three
#     lines; a name that only ends in kill (a wrapper of another name) is not
#     one.
#  2. No tests/*.py or tests/*/*.py except procinfo.py and gui_owner.py calls
#     os.kill or os.killpg with the signal 0 (tests/check_child_state.py reads
#     the tokens, so a docstring or a comment is never mistaken for a call).
#  3. No such file tests a process's /proc folder for existence
#     (os.path.exists("/proc/%d" % pid), isdir, stat, access): on a system
#     with no /proc, which is every BSD, the folder is never there, so the
#     process is "gone" at once.  Reading /proc/<pid>/fd or stat is not
#     flagged: that is a read, and a row that needs it already needs Linux.
#
# A hit is never excused by what it means: at the time of writing every such
# call in tests/ was a liveness or a reaped question.  A use that really needs
# the answer of kill(pid, 0) itself (EPERM, say) goes through a named function
# in child_state.h.  waitpid with WNOHANG is not flagged: it is a reaping wait
# owned by the row that forked.
#
# --selftest feeds fixtures with one offence of each shape and files that must
# pass, and fails unless the scan names each offence and only those (a scanner
# that sees nothing is not a check).
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
TESTSDIR=
SELFTEST=0

while [ $# -gt 0 ]; do
  case $1 in
    --dir) shift; TESTSDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-child-state: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done
[ -n "$TESTSDIR" ] || TESTSDIR=$SRCDIR/tests

. "$HERE/workdir.sh"
work_dir child-state 1

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")

# The scanner of rule 1: each line is blanked once, and a call that starts on a
# line is looked for in that line joined to the next two (a call may be split
# over lines), so the cost stays linear.  Each kill( or killpg( whose second
# argument is 0 is named with the line it starts on.
SCAN_AWK='
FNR == 1 { blank_incom = 0; lines = 0 }
{ text[++lines] = blank_c($0, 0) }
function starts_here(i,   window, head) {
  head = text[i]
  window = head " " text[i + 1] " " text[i + 2]
  if (!match(window, "(^|[^A-Za-z0-9_])(kill|killpg)[ \t]*\\([^,;]+,[ \t]*0[ \t]*\\)"))
    return 0
  if (substr(window, RSTART, 1) ~ /[A-Za-z]/)
    return RSTART <= length(head)
  return RSTART < length(head)
}
END {
  for (i = 1; i <= lines; i++)
    if (starts_here(i))
      print file ":" i ": rule 1: kill or killpg with the signal 0 asks whether a pid exists, not whether it runs; use tests/child_state.h"
}
'

# scan_c FILE NAME: rule 1 over FILE (child_state.h excepted).
scan_c() {
  case $2 in child_state.h) return 0 ;; esac
  awk -v file="$2" "$BLANK_C""$SCAN_AWK" "$1"
}

# scan_py FILE NAME: rule 2 over FILE (the two owners excepted).
scan_py() {
  case $2 in procinfo.py|gui_owner.py) return 0 ;; esac
  [ -n "$PYTHON_OK" ] || return 0
  python3 -I "$HERE/check_child_state.py" "$1" | sed "s|^$1:|$2:|"
}

PYTHON_OK=
command -v python3 >/dev/null 2>&1 && PYTHON_OK=1

# scan_dir DIR: every offence on stdout; the number of files read is in
# $WORK/count.
scan_dir() {
  n=0
  for f in "$1"/*.c "$1"/*.h; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    scan_c "$f" "${f##*/}"
  done
  for f in "$1"/*.py "$1"/*/*.py; do
    [ -f "$f" ] || continue
    n=$((n + 1))
    scan_py "$f" "${f#"$1"/}"
  done
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
  mkdir -p "$fx/sub"
  printf '%s\n' 'int alive(int p) { return kill(p, 0) == 0; }' > "$fx/test_bad_kill.c"
  printf '%s\n' 'int alive(int p) { return kill (p , 0 ) == 0; }' > "$fx/bad_spaced.h"
  printf '%s\n' 'int alive(int g) {' '  return killpg(g,' '                0) == 0;' '}' > "$fx/bad_split.c"
  printf '%s\n' '#define ALIVE(p) kill(p, 0)' > "$fx/bad_macro.h"
  printf '%s\n' 'int g(int g) { return killpg(g, 0); }' > "$fx/bad_killpg.c"
  printf '%s\n' '/* once kill(pid, 0) */' '// kill(pid, 0)' 'static const char *s = "kill(pid, 0)";' > "$fx/good_words.h"
  printf '%s\n' 'static void f(int p) { kill(p, SIGKILL); kill(p, SIGTERM); wpe_kill(p, 0); }' > "$fx/good_signal.c"
  printf '%s\n' 'static int alive(int p) { return kill(p, 0) == 0; }' > "$fx/child_state.h"
  printf '%s\n' 'import os' 'os.kill(1, 0)' > "$fx/test_bad.py"
  printf '%s\n' 'import os' 'os.killpg(1, 0)' > "$fx/sub/test_bad_sub.py"
  printf '%s\n' 'import os' 'gone = not os.path.exists("/proc/%d" % 7)' > "$fx/test_bad_proc.py"
  printf '%s\n' 'import os' 'ok = os.path.exists("/proc/self/stat")' > "$fx/test_good_proc.py"
  printf '%s\n' '# os.kill(1, 0)' 'x = "os.kill(1, 0)"' > "$fx/test_good_words.py"
  printf '%s\n' 'import os' 'os.kill(1, 0)' > "$fx/gui_owner.py"
  printf '%s\n' 'import os' 'os.killpg(1, 0)' > "$fx/procinfo.py"
  out=$(scan_dir "$fx")
  selftest_expect "kill(p, 0) in C" "$out" "test_bad_kill.c:1: rule 1"
  selftest_expect "kill with blanks round the comma" "$out" "bad_spaced.h:1: rule 1"
  selftest_expect "killpg split over two lines, named at its first" "$out" "bad_split.c:2: rule 1"
  selftest_expect "kill in a macro" "$out" "bad_macro.h:1: rule 1"
  selftest_expect "killpg(g, 0)" "$out" "bad_killpg.c:1: rule 1"
  if [ -n "$PYTHON_OK" ]; then
    selftest_expect "os.kill(1, 0) in Python" "$out" "test_bad.py:2: rule 2"
    selftest_expect "os.killpg(1, 0) in a Python folder" "$out" "sub/test_bad_sub.py:2: rule 2"
    selftest_expect "a test of /proc/<pid> for existence" "$out" "test_bad_proc.py:2: rule 3"
    python3 -I "$HERE/check_child_state.py" --selftest >"$WORK/pyself" 2>&1 \
      || { echo "FAIL: the Python scanner's own selftest"; sed 's/^/    | /' "$WORK/pyself"; selftest_failed=1; }
  else
    echo "note: no python3, the Python half is not run"
  fi
  for good in good_words.h good_signal.c child_state.h test_good_words.py test_good_proc.py gui_owner.py procinfo.py; do
    if printf '%s\n' "$out" | grep -q "^$good:"; then
      echo "FAIL: $good must pass, and the scan named it:"
      printf '%s\n' "$out" | grep "^$good:" | sed 's/^/    | /'
      selftest_failed=1
    else
      echo "ok: $good passes"
    fi
  done
  if [ $selftest_failed = 0 ]; then
    echo "check-child-state: selftest ok"
    return 0
  fi
  echo "check-child-state: FAIL (selftest)"
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
  echo "check-child-state: FAIL: no test file was read under $TESTSDIR (the scan is blind)"
  status=1
fi
if [ -n "$out" ]; then
  echo "check-child-state: FAIL: a test asks kill or killpg with the signal 0 outside the owners of the process table:"
  printf '%s\n' "$out" | sed 's/^/  /'
  status=1
fi
selftest_out=$(sh "$0" --selftest 2>&1) \
  || { echo "check-child-state: FAIL: canary: the scan is blind (run $0 --selftest)"; printf '%s\n' "$selftest_out" | sed 's/^/  | /'; status=1; }
[ $status = 0 ] && echo "check-child-state: ok ($scanned files)"
exit $status
