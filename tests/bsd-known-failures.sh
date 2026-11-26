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
#
# A fifth field "flaky" marks a row of a race that fails on some runs only:
# a flaky row that passes is not STALE, one that fails is a "known flaky
# failure", and both are printed on every run.  The kind is bounded: the
# finding must be in the process-end race class (BT11, BT12, BT13a, BT20) and
# the reason must say "listed YYYY-MM-DD", the label must have 8 characters at
# least, or the line is refused.  A sixth field "needs" lists cause labels
# separated by ";": the row is then known only when one of them fails in the
# same log, so a consequence row that fails alone is NEW.  A label ending in
# "$" matches the whole row text, not just its start.

# The logs hold Latin-1 bytes and labels with accented text; read them as
# bytes so no awk warns about a locale mismatch.
LC_ALL=C
export LC_ALL

# known_failures OS: print the rows listed for OS, one per line.
known_failures() {
  case $1 in
    freebsd)
      # No test is known to fail on FreeBSD.
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
EOF
      ;;
    netbsd)
      cat <<'EOF'
test_bsp_close|the launcher is gone and reaped (launcher that ignores SIGTERM)|loop_until_gone saw the launcher gone at 2006 ms, then process_alive (kill(pid,0), where there is no /proc) said alive; cause unknown, one run (43841645667), evidence pending; listed 2026-11-16|BT20|flaky
test_child|ended on SIGTERM: got |a child's end arrives late or never (run 43792053014; evidence pending); listed 2026-11-16|BT11|flaky
test_child|by SIGTERM, not given up on: got |a child's end arrives late or never (run 43792053014; evidence pending); listed 2026-11-16|BT11|flaky
test_child|by SIGTERM itself|a child's end arrives late or never (run 43792053014; evidence pending); listed 2026-11-16|BT11|flaky
test_child|the start is refused: got |the rows after the late ends find the slots still taken; known only when a cause row fails too; listed 2026-11-16|BT11|flaky|ended on SIGTERM: got ;by SIGTERM itself
test_child|with nothing running: got |the rows after the late ends find the slots still taken; known only when a cause row fails too; listed 2026-11-16|BT11|flaky|ended on SIGTERM: got ;by SIGTERM itself
test_child|and no descriptor: got |the rows after the late ends find the slots still taken; known only when a cause row fails too; listed 2026-11-16|BT11|flaky|ended on SIGTERM: got ;by SIGTERM itself
test_child|none left open: got |the rows after the late ends find the slots still taken; known only when a cause row fails too; listed 2026-11-16|BT11|flaky|ended on SIGTERM: got ;by SIGTERM itself
test_child|the user reads why$|the rows after the late ends find the slots still taken; known only when a cause row fails too; listed 2026-11-16|BT11|flaky|ended on SIGTERM: got ;by SIGTERM itself
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
fx_f|flaky row f1|reason f1 (listed 2026-11-16)|BT11|flaky
fx_g|flaky row g1|reason g1 (listed 2026-11-16)|BT11|flaky
fx_h|flaky cause h|reason h (listed 2026-11-16)|BT11|flaky
fx_h|flaky effect h1|reason h1 (listed 2026-11-16)|BT11|flaky|flaky cause h
fx_h|flaky effect h2$|reason h2 (listed 2026-11-16)|BT11|flaky|flaky cause h;other cause
EOF
      ;;
    selftest_badflaky)
      # Fixture list: a flaky line whose finding is not a process-end race.
      cat <<'EOF'
fx_b|flaky row b|reason b (listed 2026-11-16)|FX6|flaky
EOF
      ;;
    selftest_nodate)
      # Fixture list: a flaky line with no listing date.
      cat <<'EOF'
fx_b|flaky row b|reason b|BT11|flaky
EOF
      ;;
    selftest_notflaky)
      # Fixture list: a fifth field that is not "flaky".
      cat <<'EOF'
fx_b|flaky row b|reason b (listed 2026-11-16)|BT11|flakey
EOF
      ;;
    selftest_toomany)
      # Fixture list: a flaky line with a seventh field.
      cat <<'EOF'
fx_b|flaky row b|reason b (listed 2026-11-16)|BT11|flaky|a cause|extra
EOF
      ;;
    selftest_shortlabel)
      # Fixture list: a flaky line whose label is too short to be bounded.
      cat <<'EOF'
fx_b|row b|reason b (listed 2026-11-16)|BT11|flaky
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

# The one definition of "a label matches a row's text", shared by the awk
# programs below: the label is the start of the text, or, when it ends in "$",
# the whole text.  A "$" label is for a row whose text has no number to stop
# before, so that a longer row with the same start is not covered by it.
LABEL_MATCH_AWK='
  function label_matches(t, l,   n) {
    n = length(l)
    if (n == 0) return 0
    if (substr(l, n, 1) == "$") return t == substr(l, 1, n - 1)
    return index(t, l) == 1
  }
'

# row_is_listed OS NAME TEXT ROWLOG: succeed when a line listed for NAME on OS
# covers the failing row TEXT of ROWLOG.  A line with a "needs" field (its
# sixth) covers it only when one of the cause rows it names also fails in
# ROWLOG: a consequence row without its cause is a new failure.  The text goes
# in through the environment: awk -v would read its backslashes as escapes.
row_is_listed() {
  known_failures "$1" | NAME="$2" TEXT="$3" awk -F'|' "$LABEL_MATCH_AWK"'
    BEGIN { n = ENVIRON["NAME"]; t = ENVIRON["TEXT"] }
    $1 == n && label_matches(t, $2) { print ($6 == "" ? "-" : $6) }
  ' | {
    while IFS= read -r needs; do
      if [ "$needs" = - ] || causes_fail "$4" "$needs"; then
        exit 0
      fi
    done
    exit 1
  }
}

# causes_fail ROWLOG NEEDS: succeed when NEEDS is empty or when some cause
# label of NEEDS (labels separated by ";") is a failing row of ROWLOG.
causes_fail() {
  [ -z "$2" ] && return 0
  printf '%s\n' "$2" | tr ';' '\n' | {
    while IFS= read -r cause; do
      if label_still_fails "$1" "$cause"; then
        exit 0
      fi
    done
    exit 1
  }
}

# label_still_fails ROWLOG LABEL: succeed when some failing row of ROWLOG
# is matched by LABEL.
label_still_fails() {
  failing_rows "$1" | LABEL="$2" awk "$LABEL_MATCH_AWK"'
    BEGIN { l = ENVIRON["LABEL"] }
    label_matches($0, l) { ok = 1 }
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
      row_is_listed "$1" "$name" "$text" "$rowlog" || printf 'NEW FAILURE: %s: %s\n' "$name" "$text"
    done
  done
}

# check_listed OS LOG: print each listed row; report when its test now passes
# or when the row is no longer among the failing rows.
check_listed() {
  known_failures "$1" | while IFS='|' read -r name label reason finding flaky needs; do
    if [ "$flaky" = flaky ]; then
      check_flaky_row "$1" "$2" "$name" "$label" "$reason" "$finding" "$needs"
      continue
    fi
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

# check_flaky_row OS LOG NAME LABEL REASON FINDING NEEDS: print what a flaky
# row did in this run.  A race that sometimes passes is not stale, so a pass
# is only said ("known flaky pass"), never an error; a failure is "known flaky
# failure".  Both lines are printed every run, so the log of each run shows
# how often the row fails and a verifier can retire the line when the owner's
# design has landed.  A failing row whose NEEDS cause rows do not fail is not
# known (check_unlisted reports it as NEW), so it is not printed here.
check_flaky_row() {
  case $(outcome "$2" "$3") in
    FAIL | ERROR)
      rowlog=$(row_log_of "$3" "$2")
      if [ ! -r "$rowlog" ]; then
        : # check_unlisted already said NO ROW LOG
      elif label_still_fails "$rowlog" "$4"; then
        if causes_fail "$rowlog" "$7"; then
          printf 'known flaky failure: %s: %s: %s [%s]\n' "$3" "$4" "$5" "$6"
        fi
      else
        printf 'known flaky pass: %s: %s: the row did not fail this run [%s]\n' "$3" "$4" "$6"
      fi
      ;;
    PASS) printf 'known flaky pass: %s: %s: the test passed this run [%s]\n' "$3" "$4" "$6" ;;
  esac
}

# check_flaky_lines OS: refuse each list line that is flaky without being
# allowed to be.  A flaky line matches by label prefix, so a real regression
# that prints the same label would read as "known flaky" for ever.  The kind
# is therefore bounded: the fifth field must be exactly "flaky", the finding
# must be one of the process-end race class (a child's end that arrives late
# or is lost: BT11, BT12, BT13a, BT20), and the reason must carry the date the
# line was listed ("listed YYYY-MM-DD") so that it can be retired.  The
# optional sixth field names the cause rows (separated by ";") that must fail
# in the same log for a consequence row to count as known; there is no
# seventh.  A label under 8 characters is too short to be bounded.
check_flaky_lines() {
  known_failures "$1" | awk -F'|' '
    NF >= 5 {
      why = ""
      if ($5 != "flaky") why = "the fifth field must be flaky"
      else if (NF > 6) why = "too many fields (name|label|reason|finding|flaky[|needs])"
      else if (length($2) < 8) why = "the label is shorter than 8 characters"
      else if ($4 !~ /^(BT11|BT12|BT13a|BT20)$/) why = "the finding " $4 " is not in the process-end race class (BT11, BT12, BT13a, BT20)"
      else if ($3 !~ /listed [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]/) why = "the reason lacks the listing date (listed YYYY-MM-DD)"
      if (why != "") printf "REFUSED LIST LINE: %s: %s: %s\n", $1, $2, why
    }
  '
}

# check_unprivileged_skips LOG: a run that says it is unprivileged
# (WPE_TEST_UNPRIVILEGED=1, set by contrib/bsd-test/ci-run.sh) must find file
# modes honoured, so test_write_owner must not skip its two permission rows.
# A probe that always skips would otherwise let those rows pass silently.
#
# This is a backstop for a probe that is bypassed.  The probes of
# tests/testdir.h now print a FAIL themselves, and count it, when the run
# says it is unprivileged, so this check cannot fire for them.  It stays for
# a row that prints the SKIP line by hand instead of going through
# test_premise.
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

# selftest_flaky_lines_refused LIST WHAT PATTERN: the fixture list
# selftest_LIST holds one malformed flaky line; the script must refuse it.
selftest_flaky_lines_refused() {
  selftest_logs "$T/$1"
  out=$(sh "$0" "selftest_$1" "$T/$1/check.log" 1); st=$?
  selftest_expect "flaky: $2 is refused" "$st" 1 "$out" "REFUSED LIST LINE: fx_b:" "$3"
}

# selftest_logs DIR: the fixture logs every case starts from, written to DIR.
# The state is the clean one: every listed row fails and nothing else does.
selftest_logs() {
  d=$1
  mkdir -p "$d"
  printf '%s\n' 'FAIL: fx_a' 'FAIL: fx_c' 'FAIL: tests/fx_s.sh' 'FAIL: tests/fx_k.sh' 'FAIL: fx_f' 'PASS: fx_g' '# TOTAL: 6' > "$d/check.log"
  printf '%s\n' 'FAIL: flaky row f1: x' 'FAIL fx_f (exit status: 1)' > "$d/fx_f.log"
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
  . "$(dirname "$0")/workdir.sh"
  work_dir bsdkf 1
  T=$WORK
  selftest_failed=0
  unset WPE_TEST_UNPRIVILEGED

  selftest_logs "$T/clean"
  out=$(sh "$0" selftest "$T/clean/check.log" 1); st=$?
  selftest_expect "clean: nothing new" "$st" 0 "$out" \
    "bsd-known-failures: nothing new on selftest" "known failure: fx_c: row c2"
  selftest_expect "flaky: a listed flaky row that passes is not STALE and one that fails is known" "$st" 0 "$out" \
    "known flaky failure: fx_f: flaky row f1" "known flaky pass: fx_g: flaky row g1"

  selftest_flaky_lines_refused "badflaky" "a finding that is not a process-end race" "FX6"
  selftest_flaky_lines_refused "nodate" "a reason without the listing date" "lacks the listing date"
  selftest_flaky_lines_refused "notflaky" "a fifth field that is not flaky" "the fifth field must be flaky"
  selftest_flaky_lines_refused "toomany" "a seventh field" "too many fields"
  selftest_flaky_lines_refused "shortlabel" "a label under 8 characters" "label is shorter than"

  for os in freebsd openbsd netbsd; do
    out=$(check_flaky_lines "$os")
    if [ -z "$out" ]; then
      echo "ok: flaky: the $os list has no refused line"
    else
      echo "FAIL: flaky: the $os list has a refused line"
      printf '%s\n' "$out" | sed 's/^/    | /'
      selftest_failed=1
    fi
  done

  selftest_logs "$T/needs"
  printf '%s\n' 'FAIL: fx_h' '# TOTAL: 1' > "$T/needs/check.log"
  printf '%s\n' 'FAIL: flaky effect h1: got 0' 'FAIL fx_h (exit status: 1)' > "$T/needs/fx_h.log"
  out=$(sh "$0" selftest "$T/needs/check.log" 1); st=$?
  selftest_expect "flaky: a consequence row without its cause row is NEW" "$st" 1 "$out" \
    "NEW FAILURE: fx_h: flaky effect h1: got 0"
  printf '%s\n' 'FAIL: flaky cause h: late' 'FAIL: flaky effect h1: got 0' 'FAIL: flaky effect h2' \
    'FAIL fx_h (exit status: 1)' > "$T/needs/fx_h.log"
  out=$(sh "$0" selftest "$T/needs/check.log" 1); st=$?
  selftest_expect "flaky: a consequence row with its cause row is a known flaky failure" "$st" 0 "$out" \
    "known flaky failure: fx_h: flaky effect h1" "known flaky failure: fx_h: flaky effect h2"
  printf '%s\n' 'FAIL: flaky cause h: late' 'FAIL: flaky effect h2 and more' 'FAIL fx_h (exit status: 1)' > "$T/needs/fx_h.log"
  out=$(sh "$0" selftest "$T/needs/check.log" 1); st=$?
  selftest_expect "flaky: a label ending in \$ matches the whole row only" "$st" 1 "$out" \
    "NEW FAILURE: fx_h: flaky effect h2 and more"

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
refused=$(check_flaky_lines "$1")
if [ -n "$refused" ]; then
  printf '%s\n' "$refused"
  status=1
fi
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
