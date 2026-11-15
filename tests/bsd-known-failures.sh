#!/bin/sh
# bsd-known-failures.sh OS LOG
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
#
# Exit: 0 nothing new, 1 an unlisted failure or a listed one that passes,
#       2 bad usage.
#
# To change a list: edit known_failures() below.  Each line is
# "name|reason|finding".  Remove the line in the same change that fixes it.

# known_failures OS: print the rows listed for OS, one per line.
known_failures() {
  case $1 in
    freebsd | openbsd)
      cat <<'EOF'
test_child|the child's controlling terminal: TIOCSCTTY ended with ENOTTY on the last BSD run, which predates the child-tty and child-fds passes; not yet run again|B8 (EVIDENCE-platform-bsd.md)
test_rowfind|libc regex folds case differently on bytes 0x80 and above than glibc|B9 (EVIDENCE-platform-bsd.md)
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

[ $# -eq 2 ] && [ -r "$2" ] && known_failures "$1" >/dev/null || {
  echo "usage: bsd-known-failures.sh freebsd|openbsd|netbsd LOG" >&2
  exit 2
}
status=0
check_unlisted "$1" "$2"
listed=$(check_listed "$1" "$2")
[ -n "$listed" ] && echo "$listed"
case $listed in *"STALE ENTRY"*) status=1 ;; esac
[ $status -eq 0 ] && echo "bsd-known-failures: nothing new on $1"
exit $status
