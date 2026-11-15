#!/bin/sh
# bsd-known-failures.sh OS LOG [CHECK_RC]
#
# Why this exists: the BSD CI job (.github/workflows/bsd.yml) runs `make check`
# on emulated machines, and a job that is red for a reason everybody knows is
# a job nobody reads.  So the failures that are understood and owned are
# listed here, per system, each with its reason and the finding it belongs
# to.  The job then fails only for a failure that is NOT listed, and also
# when a listed failure now PASSES, so the list shrinks and never rots.
#
#   OS   freebsd, openbsd or netbsd
#   LOG  the output of `gmake check` (lines "PASS: name", "FAIL: name", ...)
#   CHECK_EXIT_STATUS  optional, the exit status of that `gmake check`
#
# Exit: 0 nothing new, 1 an unlisted failure, a listed one that passes, or a
#       log with no "# TOTAL:" summary (nothing ran), 2 bad usage.
#
# To change a list: edit known_failures() below.  Each line is
# "name|reason|finding".  Remove the line in the same change that fixes it.

# known_failures OS: print the rows listed for OS, one per line.
known_failures() {
  case $1 in
    freebsd | openbsd)
      cat <<'EOF'
test_child|the child's controlling terminal and start still fail on OpenBSD although the child-tty and child-fds passes have landed|B8 (EVIDENCE-platform-bsd.md)
test_rowfind|case folding differs from glibc, also ASCII \w and ICASE|B9 (EVIDENCE-platform-bsd.md)
test_rowfind_copy|same as test_rowfind, the copy-match build of the engine|B9 (EVIDENCE-platform-bsd.md)
test_rowfind_reject|same as test_rowfind, the reject build of the engine|B9 (EVIDENCE-platform-bsd.md)
EOF
      ;;
    netbsd) ;;
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

# check_unlisted OS LOG: report each failure that is not on OS's list.
check_unlisted() {
  for name in $(failed_names "$2"); do
    if ! known_failures "$1" | grep -q "^$name|"; then
      echo "NEW FAILURE: $name is not a known failure on $1"
      status=1
    fi
  done
}

# check_listed OS LOG: print each listed row; fail when it now passes.
check_listed() {
  known_failures "$1" | while IFS='|' read -r name reason finding; do
    case $(outcome "$2" "$name") in
      FAIL | ERROR) echo "known failure: $name: $reason [$finding]" ;;
      PASS) echo "STALE ENTRY: $name passes on $1: remove it from tests/bsd-known-failures.sh [$finding]" ;;
    esac
  done
}

[ $# -ge 2 ] && [ $# -le 3 ] && [ -r "$2" ] && known_failures "$1" >/dev/null || {
  echo "usage: bsd-known-failures.sh freebsd|openbsd|netbsd LOG [CHECK_EXIT_STATUS]" >&2
  exit 2
}

# has_summary LOG: automake ends a run that happened with "# TOTAL: n".  A log
# without it is a build that stopped or a check that never started, which has
# no failures to list and must not read as "nothing new".
has_summary() {
  grep '^# TOTAL:' "$1" | grep -qv '^# TOTAL: *0$'
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
check_unlisted "$1" "$2"
listed=$(check_listed "$1" "$2")
[ -n "$listed" ] && echo "$listed"
case $listed in *"STALE ENTRY"*) status=1 ;; esac
[ $status -eq 0 ] && echo "bsd-known-failures: nothing new on $1"
exit $status
