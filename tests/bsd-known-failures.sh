#!/bin/sh
# bsd-known-failures.sh OS LOG [CHECK_RC]
# bsd-known-failures.sh --selftest
#
# Why this exists: the BSD CI job (.github/workflows/bsd.yml) runs `make check`
# on emulated machines, and a job that is red for a reason everybody knows is
# a job nobody reads.  So the failures that are understood and owned are
# listed here, per system, each with its reason and the finding it belongs
# to.  The job then fails only for a failure that is NOT listed, and also
# when a listed failure now PASSES, so the list shrinks and never rots.
#
# The list is by ROW, not by test binary.  A test that fails for a known
# reason must not hide a new failing row inside the same binary, so each
# listed line names one failing row of one test.
#
#   OS   freebsd, openbsd or netbsd
#   LOG  the output of `gmake check` (lines "PASS: name", "FAIL: name", ...).
#        The per-test log of each failing test is read from the same folder:
#        bsd-out/check.log -> bsd-out/<basename of name>.log (the CI copies
#        test_*.log and tests/*.log there).
#   CHECK_EXIT_STATUS  optional, the exit status of that `gmake check`
#   --selftest  run the script on fixture logs and check what it reports
#
# Exit: 0 nothing new, 1 an unlisted failure, a listed one that passes, a
#       listed row that no longer fails, a failing test whose own log is
#       missing, or a log with no "# TOTAL:" summary (nothing ran), 2 bad
#       usage.
#
# What a "row" is, in the per-test log:
#   "FAIL: label"        a row of a C test
#   "FAIL [C]: label"    a row of a test that runs once per locale; the
#                        "[locale]: " tag is dropped, so one listed line
#                        covers every locale
#   "reaches ...", "stale allow line: ...", "canary: ..."
#                        one problem a check script prints in its own words
#                        (the two spaces in front of the first two are dropped)
#   "(exit status: N)"   the harness's last line.  It is a row when the log has
#                        no other row (a check script that ends at the 300 s
#                        deadline, 124), and also when it is anything but 1
#                        in a log that has rows: a test that crashes or runs
#                        out of time after its known rows is not "known"
# A listed label matches when it is the start of the row's text, so a label
# can stop before a number or a process id that changes from run to run.
#
# To change a list: edit known_failures() below.  Each line is
# "name|label|reason|finding", with no "|" in the label.  Remove the line in
# the same change that fixes it.

# The logs hold Latin-1 bytes and labels with accented text; read them as
# bytes so no awk warns about a locale mismatch.
LC_ALL=C
export LC_ALL

# known_failures OS: print the rows listed for OS, one per line.
known_failures() {
  case $1 in
    freebsd)
      cat <<'EOF'
test_childsafe|the folder is named on its terminal|the refusal written to the pty may be lost when the child exits at once (hypothesis, evidence pending)|BT13a
test_rowfind|w ICASE: Find "\w+"|regcomp refuses \w on this system: it has no GNU escapes|BT3
test_rowfind|ba from 1: Find "\ba"|regcomp refuses \b on this system: it has no GNU escapes|BT3
test_rowfind|b boundary at to: Find "b\b"|regcomp refuses \b on this system: it has no GNU escapes|BT3
test_rowfind|differential: Find |the reference has no byte fallback for a text the multibyte engine refuses (generated rows, all one reason)|BT5
test_rowfind_copy|w ICASE: Find "\w+"|regcomp refuses \w on this system: it has no GNU escapes|BT3
test_rowfind_copy|ba from 1: Find "\ba"|regcomp refuses \b on this system: it has no GNU escapes|BT3
test_rowfind_copy|b boundary at to: Find "b\b"|regcomp refuses \b on this system: it has no GNU escapes|BT3
test_rowfind_copy|differential: Find |the reference has no byte fallback for a text the multibyte engine refuses (generated rows, all one reason)|BT5
test_rowfind_reject|w ICASE: Find "\w+"|regcomp refuses \w on this system: it has no GNU escapes|BT3
test_rowfind_reject|ba from 1: Find "\ba"|regcomp refuses \b on this system: it has no GNU escapes|BT3
test_rowfind_reject|b boundary at to: Find "b\b"|regcomp refuses \b on this system: it has no GNU escapes|BT3
test_rowfind_reject|differential: Find |the reference has no byte fallback for a text the multibyte engine refuses (generated rows, all one reason)|BT5
tests/check-wayland-frame.sh|(exit status: 124)|runs past the 300 s deadline: the awk scan is quadratic in the file|BT10
EOF
      ;;
    openbsd)
      cat <<'EOF'
test_child|ended on SIGTERM: got |a child's end arrives late or never (evidence pending, pass E)|BT11
test_child|by SIGTERM, not given up on: got |a child's end arrives late or never (evidence pending, pass E)|BT11
test_child|by SIGTERM itself|a child's end arrives late or never (evidence pending, pass E)|BT11
test_child|the start is refused: got |the rows after the late ends find the slots still taken|BT11
test_child|with nothing running: got |the rows after the late ends find the slots still taken|BT11
test_child|and no descriptor: got |the rows after the late ends find the slots still taken|BT11
test_child|none left open: got |the rows after the late ends find the slots still taken|BT11
test_child|the user reads why|the rows after the late ends find the slots still taken|BT11
test_child|what it started was never signalled|the pid line came late|BT11
test_child|test_child (the owner's start|the 30 s watchdog: a child's end never came|BT11
test_childsafe|a refusal with a lock held by another thread: the child did not end within its bound|the child's end was not delivered within its bound (evidence pending, pass E)|BT12
test_rowfind|w ICASE: Find "\w+"|OpenBSD takes \w as the letter w: no GNU escapes|BT3
test_rowfind|. is a character: Find "."|OpenBSD's regex reads bytes whatever the locale|BT6
test_rowfind|e acute ICASE finds E acute|OpenBSD's regex folds ASCII only|BT6
test_rowfind_copy|w ICASE: Find "\w+"|OpenBSD takes \w as the letter w: no GNU escapes|BT3
test_rowfind_copy|ba from 1: Find "\ba"|OpenBSD takes \b as the letter b: no GNU escapes|BT3
test_rowfind_copy|. is a character: Find "."|OpenBSD's regex reads bytes whatever the locale|BT6
test_rowfind_copy|e acute ICASE finds E acute|OpenBSD's regex folds ASCII only|BT6
test_rowfind_reject|w ICASE: Find "\w+"|OpenBSD takes \w as the letter w: no GNU escapes|BT3
test_rowfind_reject|. is a character: Find "."|OpenBSD's regex reads bytes whatever the locale|BT6
test_rowfind_reject|e acute ICASE finds E acute|OpenBSD's regex folds ASCII only|BT6
tests/check-chain-outcome.sh|(exit status: 124)|runs past the 300 s deadline, then its TERM trap removes its own work directory|BT9
tests/check-wayland-frame.sh|(exit status: 124)|runs past the 300 s deadline: the awk scan is quadratic in the file|BT10
EOF
      ;;
    netbsd)
      cat <<'EOF'
test_childsafe|the folder is named on its terminal|the refusal written to the pty may be lost when the child exits at once (hypothesis, evidence pending)|BT13a
test_rowfind|differential: Find |the reference compiles without the engine's REG_GNU flag (generated rows, all one reason)|BT4
test_rowfind_copy|differential: Find |the reference compiles without the engine's REG_GNU flag (generated rows, all one reason)|BT4
test_rowfind_copy|ba from 1: Find "\ba"|a named row the design did not list: the copy build takes \b as a letter, cause not yet classified (evidence for P5)|unclassified (P5 evidence)
test_rowfind_reject|differential: Find |the reference compiles without the engine's REG_GNU flag (generated rows, all one reason)|BT4
EOF
      ;;
    selftest)
      # Fixture list for --selftest below; not a system.
      cat <<'EOF'
fx_a|row one|reason a|FX1
fx_c|row c1|reason c1|FX2
fx_c|row c2|reason c2|FX2
tests/fx_s.sh|(exit status: 124)|reason s|FX3
fx_p|row p|reason p|FX4
tests/fx_k.sh|reaches known (from .text.wpe_child_exec): not in the allow file|reason k|FX5
EOF
      ;;
    *) return 1 ;;
  esac
}

# outcome LOG NAME: print the word the log gives NAME (PASS, FAIL, ERROR...).
outcome() {
  awk -v n="$2" '$2 == n && $1 ~ /^[A-Z]+:$/ { sub(/:$/, "", $1); print $1; exit }' "$1"
}

# failed_names LOG: the name of every FAIL or ERROR in LOG.
failed_names() {
  awk '$1 == "FAIL:" || $1 == "ERROR:" { print $2 }' "$1" | sort -u
}

# row_log_of NAME LOG: the per-test log of NAME, next to the check log LOG.
row_log_of() {
  printf '%s/%s.log\n' "$(dirname "$2")" "$(basename "$1")"
}

# failing_rows ROWLOG: the text of every failing row in one per-test log, one
# per line (see "What a row is" above).
failing_rows() {
  awk '
    /^FAIL: / { rows[++n] = substr($0, 7); next }
    /^FAIL \[[^]]*\]: / { t = substr($0, 6); sub(/^\[[^]]*\]: /, "", t); rows[++n] = t; next }
    /^FAIL [^ ]+ \(exit status: [0-9]+\)$/ { sub(/^FAIL [^ ]+ /, ""); last = $0; next }
    /^  (reaches|stale allow line:) / { t = $0; sub(/^  /, "", t); rows[++n] = t; next }
    /^canary: / { rows[++n] = $0; next }
    END {
      # A test that has rows ends with status 1 when it simply failed; any
      # other status (a crash, the deadline) is a row of its own.
      if (last != "" && (n == 0 || last != "(exit status: 1)")) print last
      for (i = 1; i <= n; i++) print rows[i]
    }
  ' "$1"
}

# row_is_listed OS NAME TEXT: succeed when a line listed for NAME on OS has a
# label that starts TEXT.  The text goes in through the environment: awk -v
# would read its backslashes as escapes.
row_is_listed() {
  known_failures "$1" | NAME="$2" TEXT="$3" awk -F'|' '
    BEGIN { n = ENVIRON["NAME"]; t = ENVIRON["TEXT"] }
    $1 == n && length($2) > 0 && index(t, $2) == 1 { ok = 1 }
    END { exit !ok }
  '
}

# label_still_fails ROWLOG LABEL: succeed when some failing row of ROWLOG
# starts with LABEL.
label_still_fails() {
  failing_rows "$1" | LABEL="$2" awk '
    BEGIN { l = ENVIRON["LABEL"] }
    index($0, l) == 1 { ok = 1 }
    END { exit !ok }
  '
}

# check_unlisted OS LOG: report each failing test that is not listed on OS,
# and each failing row of a listed test that is not listed.
check_unlisted() {
  for name in $(failed_names "$2"); do
    if ! known_failures "$1" | awk -F'|' -v n="$name" '$1 == n { ok = 1 } END { exit !ok }'; then
      echo "NEW FAILURE: $name is not a known failure on $1"
      continue
    fi
    rowlog=$(row_log_of "$name" "$2")
    if [ ! -r "$rowlog" ]; then
      echo "NO ROW LOG: $name failed but $rowlog is missing, so its rows cannot be matched"
      continue
    fi
    failing_rows "$rowlog" | while IFS= read -r text; do
      row_is_listed "$1" "$name" "$text" || printf 'NEW FAILURE: %s: %s\n' "$name" "$text"
    done
  done
}

# check_listed OS LOG: print each listed row; report when its test now passes
# or when the row is no longer among the failing rows.
check_listed() {
  known_failures "$1" | while IFS='|' read -r name label reason finding; do
    case $(outcome "$2" "$name") in
      FAIL | ERROR)
        rowlog=$(row_log_of "$name" "$2")
        if [ ! -r "$rowlog" ]; then
          : # check_unlisted already said NO ROW LOG
        elif label_still_fails "$rowlog" "$label"; then
          printf 'known failure: %s: %s: %s [%s]\n' "$name" "$label" "$reason" "$finding"
        else
          printf 'STALE ENTRY: %s: the row "%s" no longer fails on %s: remove it from tests/bsd-known-failures.sh [%s]\n' \
            "$name" "$label" "$1" "$finding"
        fi
        ;;
      PASS) echo "STALE ENTRY: $name passes on $1: remove it from tests/bsd-known-failures.sh [$finding]" ;;
    esac
  done
}

# check_unprivileged_skips LOG: a run that says it is unprivileged
# (WPE_TEST_UNPRIVILEGED=1, set by contrib/bsd-test/ci-run.sh) must find file
# modes honoured, so test_write_owner must not skip its two permission rows.
# A probe that always skips would otherwise let those rows pass silently.
check_unprivileged_skips() {
  [ "$WPE_TEST_UNPRIVILEGED" = 1 ] || return 0
  rowlog=$(row_log_of test_write_owner "$1")
  [ -r "$rowlog" ] || return 0
  grep -E '^SKIP: (a backup is not made in a folder that refuses|a failure with no backup says the file may be cut short):' "$rowlog" |
    while IFS= read -r line; do
      printf 'NEW FAILURE: test_write_owner: %s: this run is unprivileged, so its probe must find file modes honoured\n' "$line"
    done
}

# has_summary LOG: automake ends a run that happened with "# TOTAL: n".  A log
# without it is a build that stopped or a check that never started, which has
# no failures to list and must not read as "nothing new".
has_summary() {
  grep '^# TOTAL:' "$1" | grep -qv '^# TOTAL: *0$'
}

# selftest_expect WHAT STATUS WANT_STATUS OUTPUT PATTERN...: one assertion
# line of --selftest; every PATTERN must be in OUTPUT.
selftest_expect() {
  what=$1 got=$2 want=$3 out=$4
  shift 4
  ok=1
  [ "$got" = "$want" ] || { ok=0; echo "  exit status $got, wanted $want"; }
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

# selftest_logs DIR: the fixture logs every case starts from, written to DIR.
# The state is the clean one: every listed row fails and nothing else does.
selftest_logs() {
  d=$1
  mkdir -p "$d"
  printf '%s\n' 'FAIL: fx_a' 'FAIL: fx_c' 'FAIL: tests/fx_s.sh' 'FAIL: tests/fx_k.sh' '# TOTAL: 4' > "$d/check.log"
  printf '%s\n' 'check-fx: the problems' '  reaches known (from .text.wpe_child_exec): not in the allow file' \
    'note: something' 'FAIL tests/fx_k.sh (exit status: 1)' > "$d/fx_k.sh.log"
  printf '%s\n' 'FAIL: row one: got 3 want 4' 'FAIL fx_a (exit status: 1)' > "$d/fx_a.log"
  printf '%s\n' 'FAIL [C]: row c1: x' 'FAIL [C.UTF-8]: row c2: y' 'FAIL fx_c (exit status: 1)' > "$d/fx_c.log"
  printf '%s\n' 'TIMEOUT: ./tests/fx_s.sh after 300 s' 'FAIL tests/fx_s.sh (exit status: 124)' > "$d/fx_s.sh.log"
}

# selftest_ci_copies_logs_before_the_check: the per-test logs are read from
# the folder of check.log, which the CI fills with a copy.  That copy must run
# before this script is called, or every failing test reads as NO ROW LOG.
# Skipped when the CI body is not next to this script (a stripped tarball).
selftest_ci_copies_logs_before_the_check() {
  body=$(dirname "$0")/../contrib/bsd-test/ci-run.sh
  if [ ! -r "$body" ]; then
    echo "ok: the CI body is not here, the copy-before-check order is not checked"
    return
  fi
  copy=$(grep -n '^cp test-suite.log test_\*.log tests/\*.log bsd-out/' "$body" | head -1 | cut -d: -f1)
  call=$(grep -n '^sh tests/bsd-known-failures.sh' "$body" | head -1 | cut -d: -f1)
  if [ -n "$copy" ] && [ -n "$call" ] && [ "$copy" -lt "$call" ]; then
    echo "ok: ci-run.sh copies the per-test logs (line $copy) before the check (line $call)"
  else
    echo "FAIL: ci-run.sh must copy the per-test logs into bsd-out before it calls this script (copy line '$copy', call line '$call')"
    selftest_failed=1
  fi
}

# selftest: run this script on fixture logs for each thing it must report:
# nothing new, NEW, STALE, NO ROW LOG, NO SUMMARY and CHECK FAILED.
selftest() {
  T=$(mktemp -d "${TMPDIR:-/tmp}/bsdkf.XXXXXX") || return 1
  selftest_failed=0
  unset WPE_TEST_UNPRIVILEGED

  selftest_logs "$T/clean"
  out=$(sh "$0" selftest "$T/clean/check.log" 1); st=$?
  selftest_expect "clean: nothing new" "$st" 0 "$out" \
    "bsd-known-failures: nothing new on selftest" "known failure: fx_c: row c2"

  selftest_logs "$T/new"
  printf '%s\n' 'FAIL: fx_a' 'FAIL: fx_c' 'FAIL: tests/fx_s.sh' 'FAIL: fx_x' '# TOTAL: 4' > "$T/new/check.log"
  printf '%s\n' 'FAIL: row one: got 3 want 4' 'FAIL: row two: a new row' 'FAIL fx_a (exit status: 1)' > "$T/new/fx_a.log"
  printf '%s\n' 'FAIL tests/fx_s.sh (exit status: 1)' > "$T/new/fx_s.sh.log"
  out=$(sh "$0" selftest "$T/new/check.log" 1); st=$?
  selftest_expect "NEW: an unlisted test, an unlisted row, a changed script status" "$st" 1 "$out" \
    "NEW FAILURE: fx_x is not a known failure on selftest" \
    "NEW FAILURE: fx_a: row two: a new row" \
    "NEW FAILURE: tests/fx_s.sh: (exit status: 1)" \
    "STALE ENTRY: tests/fx_s.sh: the row \"(exit status: 124)\" no longer fails"

  selftest_logs "$T/stale"
  printf '%s\n' 'PASS: fx_a' 'FAIL: fx_c' 'FAIL: tests/fx_s.sh' '# TOTAL: 3' > "$T/stale/check.log"
  printf '%s\n' 'FAIL [C]: row c1: x' 'FAIL fx_c (exit status: 1)' > "$T/stale/fx_c.log"
  out=$(sh "$0" selftest "$T/stale/check.log" 1); st=$?
  selftest_expect "STALE: a test that passes, a row that no longer fails" "$st" 1 "$out" \
    "STALE ENTRY: fx_a passes on selftest" \
    "STALE ENTRY: fx_c: the row \"row c2\" no longer fails"

  selftest_logs "$T/norow"
  rm -f "$T/norow/fx_c.log"
  out=$(sh "$0" selftest "$T/norow/check.log" 1); st=$?
  selftest_expect "NO ROW LOG: a failing test without its own log" "$st" 1 "$out" \
    "NO ROW LOG: fx_c failed but"

  selftest_logs "$T/nosum"
  printf '%s\n' 'FAIL: fx_a' > "$T/nosum/check.log"
  out=$(sh "$0" selftest "$T/nosum/check.log" 2); st=$?
  selftest_expect "NO SUMMARY: nothing ran" "$st" 1 "$out" "NO SUMMARY:"

  selftest_logs "$T/chk"
  printf '%s\n' 'PASS: fx_a' '# TOTAL: 1' > "$T/chk/check.log"
  out=$(sh "$0" selftest "$T/chk/check.log" 2); st=$?
  selftest_expect "CHECK FAILED: a non-zero status and no failing test" "$st" 1 "$out" \
    "CHECK FAILED: gmake check exited 2"

  selftest_logs "$T/crash"
  printf '%s\n' 'FAIL: row one: got 3 want 4' 'FAIL fx_a (exit status: 139)' > "$T/crash/fx_a.log"
  out=$(sh "$0" selftest "$T/crash/check.log" 1); st=$?
  selftest_expect "NEW: a listed test that crashed after its known rows (139)" "$st" 1 "$out" \
    "NEW FAILURE: fx_a: (exit status: 139)"
  printf '%s\n' 'FAIL: row one: got 3 want 4' 'FAIL fx_a (exit status: 124)' > "$T/crash/fx_a.log"
  out=$(sh "$0" selftest "$T/crash/check.log" 1); st=$?
  selftest_expect "NEW: a listed test that ran out of time after its known rows (124)" "$st" 1 "$out" \
    "NEW FAILURE: fx_a: (exit status: 124)"

  selftest_logs "$T/script"
  printf '%s\n' 'check-fx: the problems' '  reaches known (from .text.wpe_child_exec): not in the allow file' \
    '  reaches system (from .text.wpe_child_exec): not in the allow file' \
    'FAIL tests/fx_k.sh (exit status: 1)' > "$T/script/fx_k.sh.log"
  out=$(sh "$0" selftest "$T/script/check.log" 1); st=$?
  selftest_expect "NEW: a new problem line inside a listed check script" "$st" 1 "$out" \
    "NEW FAILURE: tests/fx_k.sh: reaches system (from .text.wpe_child_exec): not in the allow file"

  selftest_logs "$T/priv"
  printf '%s\n' 'PASS: test_write_owner' '# TOTAL: 1' > "$T/priv/check.log"
  printf '%s\n' 'SKIP: a backup is not made in a folder that refuses: this user can create files' \
    'SKIP: text onto /dev/full: this system has no /dev/full device' > "$T/priv/test_write_owner.log"
  out=$(WPE_TEST_UNPRIVILEGED=1 sh "$0" selftest "$T/priv/check.log" 0); st=$?
  selftest_expect "NEW: a permission row skipped in an unprivileged run" "$st" 1 "$out" \
    "NEW FAILURE: test_write_owner: SKIP: a backup is not made in a folder that refuses"
  out=$(sh "$0" selftest "$T/priv/check.log" 0); st=$?
  selftest_expect "a permission row skipped in a run that does not say it is unprivileged is fine" "$st" 0 "$out" \
    "nothing new on selftest"

  selftest_ci_copies_logs_before_the_check

  rm -rf "$T"
  if [ $selftest_failed = 0 ]; then
    echo "bsd-known-failures --selftest: all cases ok"
    return 0
  fi
  echo "bsd-known-failures --selftest: FAILED"
  return 1
}

if [ "$1" = "--selftest" ]; then
  selftest
  exit $?
fi

[ $# -ge 2 ] && [ $# -le 3 ] && [ -r "$2" ] && known_failures "$1" >/dev/null || {
  echo "usage: bsd-known-failures.sh freebsd|openbsd|netbsd LOG [CHECK_EXIT_STATUS]" >&2
  echo "       bsd-known-failures.sh --selftest" >&2
  exit 2
}

if ! has_summary "$2"; then
  echo "NO SUMMARY: $2 has no '# TOTAL:' line, or it says 0, so no test ran (the build or check stopped early)"
  exit 1
fi
status=0
# The exit status of `gmake check`, when the caller passes it: non-zero with
# no FAIL or ERROR line in the log means the check broke for a reason the
# list cannot name (a make error, a killed run), so it fails here.
if [ -n "$3" ] && [ "$3" != 0 ] && [ -z "$(failed_names "$2")" ]; then
  echo "CHECK FAILED: gmake check exited $3 and the log lists no failing test"
  status=1
fi
unlisted=$(check_unlisted "$1" "$2"; check_unprivileged_skips "$2")
if [ -n "$unlisted" ]; then
  printf '%s\n' "$unlisted"
  status=1
fi
listed=$(check_listed "$1" "$2")
[ -n "$listed" ] && printf '%s\n' "$listed"
case $listed in *"STALE ENTRY"*) status=1 ;; esac
[ $status -eq 0 ] && echo "bsd-known-failures: nothing new on $1"
exit $status
