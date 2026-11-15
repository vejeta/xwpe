#!/bin/sh
# check-child-wait.sh -- waiting for a child has one owner and always a bound.
#
# A wait that can last for ever is how a silent debug adapter hung a test
# run on a Mac for hours with no word of which test it was.  The editor waits
# on its loop (we_fdloop.c), the owner of children (we_child.c) reaps them,
# and a test that waits is bounded by the one hang guard (tests/test_watchdog.h)
# which names what did not finish.  This script fails the build when code
# goes round them.
#
#   check-child-wait.sh [--srcdir DIR] [--allow FILE] [--list]
#
# What is left for this script.  check-child-fds.sh and check-child-exec.sh
# hold how a child is started (one exec, its descriptors, what runs between
# fork and exec), and check-one-owner.sh holds, on the objects, that only the
# owners import poll, waitpid and their kin, for the editor's objects and,
# through CHILD_WAIT_OBJECTS, the sync layers' objects.  None of them reads
# the sync layers' source (every top level we_*_sync.c but the files named in
# NOT_CHILD_WAITS: tests link them, the editor does not) or the tests that
# start children, and none reads how long a wait may last.  That is this
# script's.
#
# Rules, with comments and the text of string literals blanked.  They read the
# sync layers and, for own-wait and own-reap, every tests/*.c that a
# Makefile.am program linking we_child.c names and every tests/*.h; for
# no-alarm, every tests/*.c and tests/*.h.  A call is read by name, not as a
# member (a.poll(), p->poll()):
#  own-wait   poll, ppoll, pselect, select, kevent, epoll_wait, nanosleep,
#             usleep or sleep: a wait of its own beside the loop's.  A test
#             that must pause calls test_sleep_ms of the guard's header.
#  own-reap   waitpid, wait, wait3, wait4 or waitid: a child is reaped by the
#             owner and a test waits through test_wait_child, test_wait_gone
#             or the owner's calls.  Where a test needs the status of a
#             child it has to peek at (WNOWAIT) or reap by hand, an allow line
#             says why.
#  no-exit-fd pidfd (any name with it), kqueue, EVFILT_PROC, in any top level
#             source, header, test and in configure.ac and Makefile.am.  The
#             death of a child reaches the loop as SIGCHLD and the owner's
#             wake pipe on every system; a second mechanism per platform is
#             what the sync layer had and the Mac did not run.  No allow line.
#  no-alarm   alarm, setitimer, timer_create or SIGALRM, in the files above:
#             a test's time limit is the guard's (test_watchdog_arm); a
#             private alarm replaces it and says nothing of what hung.
#  armed      every tests/*.c that a program linking we_child.c names
#             includes test_watchdog.h and calls test_watchdog_arm or
#             test_watchdog_case, so a hang there reports its case.
#  deadline   in the sync layers, a call of wpe_fd_wait_for whose deadline is
#             WPE_WAIT_FOREVER or a negative literal, a call of wpe_fd_poll
#             with a negative literal, and any #define that ends in
#             NO_DEADLINE or FOREVER.  No allow line.
#
# The files tests/child-wait.allow counts the own-wait, own-reap and
# no-alarm calls that are meant, one line per file and call:
#
#   file|call|count|pass|reason
#
# "count" is how many such calls the file has: more or fewer fails, and a
# line that matches nothing is stale.  "keep" is a permanent exception.
#
# How it can go blind, and what it does about it.  A rule reads the file
# list it is given: the list of programs comes from Makefile.am (a canary
# names test_child and test_dap in it, and the sync layers must be read and
# still wait through the loop; a sync layer whose object CHILD_WAIT_OBJECTS
# does not list is named); the scanner reads text, so a call made through a
# macro or a function pointer is not seen (check-one-owner.sh sees a macro,
# but only in the editor's objects and the sync layers' objects, not in the
# tests' own objects).  Canaries plant one offender per rule in
# a file of its own, and calls that are fine (a member, a comment, a string,
# a longer name, the guard's own helpers) beside them; a scanner that does not
# name the first, or names the second, fails the script as blind.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1
ALLOW=
LIST=0

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --list) LIST=1 ;;
    *) echo "check-child-wait: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/child-wait.allow
if [ ! -f "$ALLOW" ]; then
  echo "check-child-wait: no allow file at $ALLOW" >&2
  exit $BAD
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/child-wait.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# The names each rule reads.  Passed to awk through the environment, which
# keeps a backslash whole.
OWN_WAIT_CALLS='poll|ppoll|pselect|select|kevent|epoll_wait|nanosleep|usleep|sleep'
OWN_REAP_CALLS='waitpid|wait|wait3|wait4|waitid'
NO_ALARM_CALLS='alarm|setitimer|timer_create'
ALLOWABLE_CALLS="$OWN_WAIT_CALLS|$OWN_REAP_CALLS|$NO_ALARM_CALLS|SIGALRM"
LOOP_WAIT_CALLS='wpe_fd_wait_for|wpe_fd_poll'
NO_EXIT_FD_RE='[A-Za-z0-9_]*[Pp][Ii][Dd][Ff][Dd][A-Za-z0-9_]*|(^|[^A-Za-z0-9_])kqueue1?[ \t]*[(]|(^|[^A-Za-z0-9_])EVFILT_PROC([^A-Za-z0-9_]|$)'
SIGALRM_RE='(^|[^A-Za-z0-9_])SIGALRM([^A-Za-z0-9_]|$)'
OPEN_ENDED_DEFINE_RE='^[ \t]*#[ \t]*define[ \t]+[A-Za-z0-9_]*(NO_DEADLINE|FOREVER)([^A-Za-z0-9_]|$)'
# Every top level we_*_sync.c is a sync layer that waits for a child, except
# the files named here, which wait for something else.  A new file of that
# name that is not named here is read by the rules, and fails until
# CHILD_WAIT_OBJECTS in Makefile.am lists its object.
NOT_CHILD_WAITS='we_ai_http_sync.c'
# Files that own a wait: never in the scope of the rules above.
OWNERS='we_child.c we_fdloop.c test_watchdog.h'

# The awk that blanks comments, and the text of string and character
# literals unless KEEP is set.  State: incom, across lines.
AWK_BLANK='
function blank(line, keep,   out, n, i, c, d, instr, q) {
  out = ""; n = length(line); i = 1; instr = 0
  while (i <= n) {
    c = substr(line, i, 1); d = substr(line, i, 2)
    if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
    if (instr) {
      if (c == "\\") { if (keep) out = out substr(line, i, 2); i += 2; continue }
      if (c == q) { out = out c; instr = 0 }
      else if (keep) out = out c
      i++; continue
    }
    if (d == "/*") { incom = 1; out = out " "; i += 2; continue }
    if (d == "//") break
    if (c == "\"" || c == "\047") { instr = 1; q = c; out = out c; i++; continue }
    out = out c; i++
  }
  return out
}
'

# scan_calls NAMES FILE... - "file:line: name<TAB>arg<TAB>arg..." for each
# call of one of NAMES (an alternation), arguments without blanks.  A member
# call (after . or >) is not one.  A call may span lines.
scan_calls() {
  names=$1
  shift
  NAMES=$names awk "$AWK_BLANK"'
    function report(at,   lo, hi, mid, args, depth, j, c, cur, n, k, parts) {
      lo = 1; hi = nl
      while (lo < hi) {
        mid = int((lo + hi + 1) / 2)
        if (start[mid] <= at) lo = mid; else hi = mid - 1
      }
      print file ":" lo ": " fname extra
    }
    function flush(   n, i, rest, base, from, j, depth, c, cur, extra2, k, ch) {
      n = length(all)
      i = 1
      while (i <= n) {
        rest = substr(all, i)
        if (!match(rest, re)) break
        base = i + RSTART - 1
        from = base + RLENGTH - 1
        fname = substr(all, base, from - base)
        sub(/^[^A-Za-z_]/, "", fname)
        sub(/[ \t]+$/, "", fname)
        j = from + 1; depth = 1; cur = ""; extra = ""
        while (j <= n && depth > 0) {
          c = substr(all, j, 1)
          if (c == "(") depth++
          else if (c == ")") depth--
          if (depth == 0) break
          if (c == "," && depth == 1) { extra = extra "\t" cur; cur = "" }
          else if (c != " " && c != "\t") cur = cur c
          j++
        }
        if (cur != "" || extra != "") extra = extra "\t" cur
        report(from)
        i = j + 1
      }
    }
    BEGIN { re = "(^|[^A-Za-z0-9_.>])(" ENVIRON["NAMES"] ")[ \t]*[(]" }
    FNR == 1 { if (nl) flush(); all = ""; nl = 0; incom = 0; file = FILENAME }
    {
      nl++
      start[nl] = length(all) + 1
      all = all blank($0, 0) " "
    }
    END { if (nl) flush() }' "$@"
}

# scan_regex RE FILE... - "file:line: text" for each line whose blanked text
# matches RE.  Strings are blanked, comments too.
scan_regex() {
  regex=$1
  shift
  RE=$regex awk "$AWK_BLANK"'
    FNR == 1 { incom = 0 }
    { s = blank($0, 0)
      if (s ~ ENVIRON["RE"]) { t = s; gsub(/^[ \t]+/, "", t); print FILENAME ":" FNR ": " t } }' "$@"
}

# scan_build_regex RE FILE... - the same for configure.ac and Makefile.am:
# a comment starts at # or at dnl, and strings are kept (a probe of a function
# is written in one).
scan_build_regex() {
  regex=$1
  shift
  RE=$regex awk '
    { s = $0
      sub(/#.*/, "", s); sub(/(^|[ \t])dnl([ \t].*)?$/, "", s)
      if (s ~ ENVIRON["RE"]) { t = s; gsub(/^[ \t]+/, "", t); print FILENAME ":" FNR ": " t } }' "$@"
}

# scan_open_ended FILE... - a wait of a sync layer that has no deadline: the
# deadline (second argument of wpe_fd_wait_for, first of wpe_fd_poll) is a
# negative literal or a name with FOREVER, or a define says so.
scan_open_ended() {
  scan_calls "$LOOP_WAIT_CALLS" "$@" | awk -F'\t' '
    {
      name = $1; sub(/^[^ ]*: /, "", name)
      if (name == "wpe_fd_wait_for") a = $3; else a = $2
      gsub(/[()]/, "", a)
      if (a ~ /^-[0-9]+[lL]*$/ || a ~ /FOREVER/) print $1 " (deadline " a ")"
    }'
  OPEN_RE=$OPEN_ENDED_DEFINE_RE scan_define_regex "$@"
}
scan_define_regex() {
  scan_regex "$OPEN_RE" "$@"
}

# top_sources - the C and header files read for the exit-descriptor and alarm
# rules: the top level and tests/, then configure.ac and Makefile.am are read
# apart.
top_sources() {
  ls "$SRCDIR"/*.c "$SRCDIR"/*.h "$SRCDIR"/tests/*.c "$SRCDIR"/tests/*.h 2>/dev/null
}

# child_programs - "program file" for each source tests/NAME.c of a
# Makefile.am program that links we_child.c.  Continued lines are joined.
child_programs() {
  awk '
    { line = $0
      while (line ~ /\\$/ && (getline nxt) > 0) { sub(/\\$/, " ", line); line = line nxt }
      if (line !~ /^[A-Za-z0-9_]+_SOURCES[ \t]*\+?=/) next
      if (line !~ /(^|[ \t])we_child\.c([ \t]|$)/) next
      prog = line; sub(/_SOURCES.*/, "", prog)
      n = split(line, f, /[ \t]+/)
      for (i = 1; i <= n; i++) if (f[i] ~ /^tests\/[A-Za-z0-9_]+\.c$/) print prog, f[i]
    }' "$SRCDIR/Makefile.am"
}

# sync_layers - the sync layers, one path a line: every top level
# we_*_sync.c that is not named in NOT_CHILD_WAITS.
sync_layers() {
  ls "$SRCDIR"/we_*_sync.c 2>/dev/null | while read -r f; do
    case " $NOT_CHILD_WAITS " in *" ${f##*/} "*) continue ;; esac
    echo "$f"
  done
}

# without_owners - the paths on stdin, minus the files that own a wait.
without_owners() {
  while read -r f; do
    case " $OWNERS " in *" ${f##*/} "*) continue ;; esac
    echo "$f"
  done
}

# wait_scope - the files the own-wait and own-reap rules read, one a line:
# the sync layers, the tests of programs that link the owner of children and
# every header of tests/, so a wait cannot hide in an include; never an owner.
wait_scope() {
  {
    sync_layers
    child_programs | while read -r prog src; do echo "$SRCDIR/$src"; done
    ls "$SRCDIR"/tests/*.h 2>/dev/null
  } | sort -u | without_owners
}

# alarm_scope - the files the no-alarm rule reads: the sync layers and every
# source and header of tests/, whether or not its program starts a child.
alarm_scope() {
  {
    sync_layers
    ls "$SRCDIR"/tests/*.c "$SRCDIR"/tests/*.h 2>/dev/null
  } | sort -u | without_owners
}

# unlisted_layers - the sync layers whose object CHILD_WAIT_OBJECTS in
# Makefile.am does not name, so check-one-owner.sh would not read it.
unlisted_layers() {
  line=$(grep 'CHILD_WAIT_OBJECTS=' "$SRCDIR/Makefile.am" 2>/dev/null)
  if [ -z "$line" ]; then
    echo "Makefile.am has no CHILD_WAIT_OBJECTS: no sync layer's object is read by check-one-owner.sh"
    return
  fi
  sync_layers | while read -r f; do
    o=${f##*/}
    case $line in
      *"-${o%.c}."*) ;;
      *) echo "${o}: its object is not in CHILD_WAIT_OBJECTS of Makefile.am" ;;
    esac
  done
}

# armed_problems SRCDIR - each test source of a child program that lacks the
# guard's include or its arm.
armed_problems() {
  CHILD_SRC=$1 child_programs_in "$1" | while read -r prog src; do
    f=$1/$src
    if [ ! -f "$f" ]; then
      echo "$src: named by $prog and not there"
      continue
    fi
    inc=$(awk "$AWK_BLANK"'
      FNR == 1 { incom = 0 }
      { s = blank($0, 1) }
      s ~ /^[ \t]*#[ \t]*include[ \t]*"test_watchdog\.h"/ { found = 1 }
      END { print found + 0 }' "$f")
    arm=$(scan_calls 'test_watchdog_arm|test_watchdog_case' "$f")
    [ "$inc" = 1 ] || echo "$src: program $prog does not include test_watchdog.h"
    [ -n "$arm" ] || echo "$src: program $prog never arms the hang guard (test_watchdog_arm or test_watchdog_case)"
  done | sort -u
}
child_programs_in() {
  ( SRCDIR=$1; child_programs )
}

# judge ALLOWFILE FOUND - what is left of FOUND (lines of scan_calls or
# scan_regex) once the counted exceptions of ALLOWFILE are taken off: a call
# the file does not list, more calls than it says, fewer, a line that matches
# nothing, a line that is not well formed or names a call no rule reads.
judge() {
  printf '%s\n' "$2" | CALLS=$ALLOWABLE_CALLS awk -F'|' -v allow="$1" '
    function base(s) { sub(/:[0-9]+: .*$/, "", s); sub(/^.*\//, "", s); return s }
    function callname(s,   c) { c = s; sub(/^[^ ]*: /, "", c); sub(/[^A-Za-z0-9_].*$/, "", c); return c }
    BEGIN {
      ok = "|" ENVIRON["CALLS"] "|"
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        n = split(line, f, "|")
        if (n < 5 || f[3] !~ /^[0-9]+$/ || f[4] == "" || f[5] == "") { print "bad line in the allow file: " line; continue }
        if (index(ok, "|" f[2] "|") == 0) { print "the allow file names a call no rule reads: " line; continue }
        key = f[1] "|" f[2]
        if (key in al) { print "duplicate allow line for " key; continue }
        al[key] = f[3]
      }
    }
    NF == 0 { next }
    {
      key = base($0) "|" callname($0)
      seen[key]++
      if (!(key in al)) print "not in the allow file: " $0
    }
    END {
      for (k in seen) if (k in al) {
        if (seen[k] > al[k] + 0) print "more than allowed: " k " has " seen[k] ", the allow file says " al[k]
        else if (seen[k] < al[k] + 0) print "fewer than the allow file says: " k " has " seen[k] ", lower the line"
      }
      for (k in al) if (!(k in seen)) print "stale allow line, delete it: " k
    }' | sort
}

# wait_calls FILE... - the waits of their own and the reaps.
wait_calls() {
  scan_calls "$OWN_WAIT_CALLS|$OWN_REAP_CALLS" "$@"
}

# alarm_calls FILE... - the alarms (the call forms and SIGALRM).
alarm_calls() {
  scan_calls "$NO_ALARM_CALLS" "$@"
  scan_regex "$SIGALRM_RE" "$@" | sed 's/: .*/: SIGALRM/'
}

# own_calls FILE... - every call the allow file may count.
own_calls() {
  wait_calls "$@"
  alarm_calls "$@"
}

# list_counts FOUND - the counted lines an allow file would start from.
list_counts() {
  printf '%s\n' "$1" | awk '
    NF == 0 { next }
    { f = $0; sub(/:[0-9]+: .*$/, "", f); sub(/^.*\//, "", f)
      c = $0; sub(/^[^ ]*: /, "", c); sub(/[^A-Za-z0-9_].*$/, "", c)
      n[f "|" c]++ }
    END { for (k in n) print k "|" n[k] "|keep|" }' | sort
}

# unlisted_layers_of DIR - unlisted_layers for the tree at DIR.
unlisted_layers_of() {
  ( SRCDIR=$1; unlisted_layers )
}

fail=0

# --- the files each rule reads ------------------------------------------
layers=$(sync_layers)
for need in we_dap_sync.c we_lsp_sync.c we_bsp_sync.c; do
  case " $(echo "$layers" | tr '\n' ' ') " in
    *"/$need "*) ;;
    *) echo "check-child-wait: $need is not among the sync layers read"; exit $BAD ;;
  esac
done
scope=$(wait_scope)
alarms=$(alarm_scope)
if [ -z "$scope" ] || [ -z "$alarms" ]; then
  echo "check-child-wait: no files to read" >&2
  exit $BAD
fi

if [ $LIST -eq 1 ]; then
  # shellcheck disable=SC2086
  list_counts "$(wait_calls $scope; alarm_calls $alarms)"
  exit 0
fi

# own-wait, own-reap, no-alarm: the counted rules.
# shellcheck disable=SC2086
found=$(wait_calls $scope; alarm_calls $alarms)
left=$(judge "$ALLOW" "$found")
if [ -n "$left" ]; then
  echo "check-child-wait: a wait, reap or alarm of its own, or an allow line that is off:"
  echo "$left"
  fail=1
fi

# no-exit-fd: anywhere, no allow line.
# shellcheck disable=SC2046
banned=$(scan_regex "$NO_EXIT_FD_RE" $(top_sources); scan_build_regex "$NO_EXIT_FD_RE" "$SRCDIR/configure.ac" "$SRCDIR/Makefile.am")
if [ -n "$banned" ]; then
  echo "check-child-wait: a second death notice (pidfd, kqueue, EVFILT_PROC) outside comments:"
  echo "$banned"
  fail=1
fi

# armed.
unarmed=$(armed_problems "$SRCDIR")
if [ -n "$unarmed" ]; then
  echo "check-child-wait: a test of the children's owner without the hang guard:"
  echo "$unarmed"
  fail=1
fi

# every layer's object is read by check-one-owner.sh.
unlisted=$(unlisted_layers)
if [ -n "$unlisted" ]; then
  echo "check-child-wait: a sync layer the symbol scan does not read:"
  echo "$unlisted"
  fail=1
fi

# deadline.
# shellcheck disable=SC2086
open_ended=$(scan_open_ended $layers)
if [ -n "$open_ended" ]; then
  echo "check-child-wait: a wait of a sync layer with no deadline:"
  echo "$open_ended"
  fail=1
fi

# --- canaries -------------------------------------------------------------
# canary NAME SCANFUNC CODE EXPECT - plant CODE in a file of its own; EXPECT
# is 1 when SCANFUNC must name something, 0 when it must stay silent.
canary() {
  printf '%s\n' "$3" > "$WORK/planted.c"
  got=$($2 "$WORK/planted.c")
  if [ "$4" = 1 ] && [ -z "$got" ]; then
    echo "canary $1: a planted offender was not found: $3"
    return 1
  fi
  if [ "$4" = 0 ] && [ -n "$got" ]; then
    echo "canary $1: a line that is fine was named: $3 -> $got"
    return 1
  fi
  return 0
}
# Each rule's scanner as one word for canary.
c_calls() { own_calls "$@"; }
c_banned() { scan_regex "$NO_EXIT_FD_RE" "$@"; }
c_open() { scan_open_ended "$@"; }

blind=0
canary poll c_calls 'int f(void) { return poll(fds, 1, 100); }' 1 || blind=1
canary usleep c_calls 'void f(void) { usleep(1000); }' 1 || blind=1
canary select c_calls 'int f(void) { return select(n, &r, 0, 0, &tv); }' 1 || blind=1
canary nanosleep c_calls 'void f(void) { nanosleep(&ts, NULL); }' 1 || blind=1
canary poll-two-lines c_calls 'int f(void) { return poll(fds,
   1, 100); }' 1 || blind=1
canary waitpid c_calls 'int f(void) { return waitpid(pid, &st, 0); }' 1 || blind=1
canary wait c_calls 'void f(void) { wait(NULL); }' 1 || blind=1
canary waitid c_calls 'void f(void) { waitid(P_PID, pid, &si, WEXITED | WNOWAIT); }' 1 || blind=1
canary alarm c_calls 'void f(void) { alarm(5); }' 1 || blind=1
canary setitimer c_calls 'void f(void) { setitimer(ITIMER_REAL, &it, 0); }' 1 || blind=1
canary timer_create c_calls 'void f(void) { timer_create(CLOCK_MONOTONIC, 0, &t); }' 1 || blind=1
canary sigalrm c_calls 'void f(void) { signal(SIGALRM, h); }' 1 || blind=1
canary member c_calls 'int f(void) { return d->poll(1) + d.wait(2) + p->sleep(3); }' 0 || blind=1
canary longer-name c_calls 'int f(void) { return test_wait_child(p, 1) + test_sleep_ms(2) + wpe_fd_poll(3) + my_alarm(4) + pollfd_new(5); }' 0 || blind=1
canary comment c_calls 'int f(void) { /* poll(x) waitpid(y) alarm(1) SIGALRM */ return 0; } // sleep(1)' 0 || blind=1
canary string c_calls 'int f(void) { return puts("std::thread::sleep(1); poll(-1); waitpid(); SIGALRM"); }' 0 || blind=1
canary guard c_calls 'void f(void) { test_watchdog_arm("x", 5); test_wait_child(p, 100); test_wait_gone(p, 100); }' 0 || blind=1
canary pidfd c_banned 'int f(void) { return syscall(SYS_pidfd_open, pid, 0); }' 1 || blind=1
canary pidfd-name c_banned 'int f(void) { return pidfd_open(pid, 0); }' 1 || blind=1
canary kqueue c_banned 'int f(void) { return kqueue(); }' 1 || blind=1
canary evfilt c_banned 'void f(void) { EV_SET(&e, pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, 0); }' 1 || blind=1
canary no-pidfd c_banned '#ifdef DAP_NO_PIDFD
#endif' 1 || blind=1
canary pidfd-comment c_banned 'int f(void) { /* no pidfd, no kqueue(), EVFILT_PROC */ return 0; }' 0 || blind=1
canary kqueue-word c_banned 'int f(void) { return kqueues + a_kqueue; }' 0 || blind=1
canary forever c_open 'void f(void) { wpe_fd_wait_for(pred, WPE_WAIT_FOREVER, "x"); }' 1 || blind=1
canary minus-one c_open 'void f(void) { wpe_fd_wait_for(pred, -1L, "x"); }' 1 || blind=1
canary minus-one-paren c_open 'void f(void) { wpe_fd_wait_for(pred, (-1), "x"); }' 1 || blind=1
canary poll-forever c_open 'void f(void) { wpe_fd_poll(-1); }' 1 || blind=1
canary define-no-deadline c_open '#define SYNC_NO_DEADLINE (-1)' 1 || blind=1
canary define-forever c_open '#define SYNC_WAIT_FOREVER 0' 1 || blind=1
canary bounded c_open 'void f(void) { wpe_fd_wait_for(sync_pred, 30000L, "x"); wpe_fd_wait_for(sync_pred, left, what); wpe_fd_poll((int)left); }' 0 || blind=1
canary no-fd-define c_open '#define SYNC_NO_FD (-1)' 0 || blind=1
canary bounded-nested c_open 'void f(void) { wpe_fd_wait_for(pick(a, -1), deadline(b, c), "x, -1"); }' 0 || blind=1

# The build files: a comment of # or dnl is not read, a probe is.
printf '%s\n' 'AC_CHECK_FUNCS([pidfd_open kqueue])' > "$WORK/planted.ac"
[ -n "$(scan_build_regex "$NO_EXIT_FD_RE" "$WORK/planted.ac")" ] || { echo "canary configure: a probe of pidfd_open was not found"; blind=1; }
printf '%s\n' '# no pidfd' 'dnl no kqueue(' 'AC_CHECK_FUNCS([poll])' > "$WORK/planted.ac"
[ -z "$(scan_build_regex "$NO_EXIT_FD_RE" "$WORK/planted.ac")" ] || { echo "canary configure: a comment was named"; blind=1; }

# The scope: the owners are never read, a test of a child program is.
case " $(echo "$scope" | tr '\n' ' ') " in
  *"/we_child.c "*|*"/we_fdloop.c "*|*"/test_watchdog.h "*)
    echo "canary scope: an owner of a wait is in the files read"; blind=1 ;;
esac
for need in we_dap_sync.c tests/test_child.c tests/test_dap.c; do
  case " $(echo "$scope" | tr '\n' ' ') " in
    *"/$need "*) ;;
    *) echo "canary scope: $need is not among the files read (Makefile.am list unreadable?)"; blind=1 ;;
  esac
done
# The sync layers still wait through the loop, so the deadline scan has calls
# to read; a layer that stopped would leave that scan reading nothing.
[ -n "$(scan_calls wpe_fd_wait_for "$SRCDIR/we_dap_sync.c")" ] || { echo "canary deadline: no call of wpe_fd_wait_for in we_dap_sync.c"; blind=1; }
[ -n "$(scan_calls wpe_fd_poll "$SRCDIR/we_lsp_sync.c")" ] || { echo "canary deadline: no call of wpe_fd_poll in we_lsp_sync.c"; blind=1; }
[ -n "$(scan_calls wpe_fd_poll "$SRCDIR/we_bsp_sync.c")" ] || { echo "canary deadline: no call of wpe_fd_poll in we_bsp_sync.c"; blind=1; }

# armed: a planted tree with a program without the guard, one with it, and
# one whose source is missing.
mkdir -p "$WORK/tree/tests"
printf '%s\n' 'check_PROGRAMS += test_bare test_ok test_ghost test_loose' \
  'test_bare_SOURCES = tests/test_bare.c we_child.c we_fdloop.c' \
  'test_ok_SOURCES = tests/test_ok.c \' \
  '  we_child.c' \
  'test_ghost_SOURCES = tests/test_ghost.c we_child.c' \
  'test_loose_SOURCES = tests/test_loose.c we_child.c' \
  'test_other_SOURCES = tests/test_other.c we_fdloop.c' > "$WORK/tree/Makefile.am"
printf '%s\n' '#include "edit.h"' 'int main(void) { return 0; }' > "$WORK/tree/tests/test_bare.c"
printf '%s\n' '#include "test_watchdog.h"' 'int main(void) { test_watchdog_arm("t", 5); return 0; }' > "$WORK/tree/tests/test_ok.c"
printf '%s\n' '/* #include "test_watchdog.h" */' '#include "edit.h"' 'int main(void) { /* test_watchdog_arm("t", 5); */ return 0; }' > "$WORK/tree/tests/test_loose.c"
got=$(armed_problems "$WORK/tree")
case $got in *test_bare.c*) ;; *) echo "canary armed: a test with no include and no arm was not named"; blind=1 ;; esac
case $got in *test_ghost.c*) ;; *) echo "canary armed: a missing test source was not named"; blind=1 ;; esac
case $got in *test_loose.c*) ;; *) echo "canary armed: a guard in comments only was not named"; blind=1 ;; esac
case $got in *test_ok.c*) echo "canary armed: a test with the guard was named"; blind=1 ;; esac
case $got in *test_other.c*) echo "canary armed: a program that does not link we_child.c was named"; blind=1 ;; esac

# The files read, on a planted tree: a new sync layer is read and, with its
# object not in CHILD_WAIT_OBJECTS, named; the one named in NOT_CHILD_WAITS is
# not; a header of tests/ is read by the wait rules and every source of tests/
# by the alarm rule; the owner's header is read by neither.
mkdir -p "$WORK/tree2/tests"
for f in we_foo_sync.c we_dap_sync.c we_ai_http_sync.c; do : > "$WORK/tree2/$f"; done
for f in test_sneaky.h test_watchdog.h test_plain.c; do : > "$WORK/tree2/tests/$f"; done
printf '%s\n' "  CHILD_WAIT_OBJECTS='test_dap-we_dap_sync.\$(OBJEXT)'; export CHILD_WAIT_OBJECTS; \\" \
  'test_plain_SOURCES = tests/test_plain.c' > "$WORK/tree2/Makefile.am"
got=$( (SRCDIR=$WORK/tree2; sync_layers; echo --; wait_scope; echo --; alarm_scope; echo --; unlisted_layers) )
case $got in *we_foo_sync.c*) ;; *) echo "canary layers: a new we_*_sync.c was not read"; blind=1 ;; esac
case $(echo "$got" | sed -n '1,/^--$/p') in *we_ai_http_sync.c*) echo "canary layers: a file named in NOT_CHILD_WAITS was read"; blind=1 ;; esac
case $(echo "$got" | sed -n '/^--$/,$p') in *test_sneaky.h*) ;; *) echo "canary scope: a header of tests/ was not read"; blind=1 ;; esac
case $got in *test_plain.c*) ;; *) echo "canary scope: a test that starts no child was not read for alarms"; blind=1 ;; esac
case $got in *test_watchdog.h*) echo "canary scope: the guard's header was read"; blind=1 ;; esac
case $(unlisted_layers_of "$WORK/tree2") in
  *we_foo_sync.c*) ;;
  *) echo "canary layers: a layer missing from CHILD_WAIT_OBJECTS was not named"; blind=1 ;;
esac
case $(unlisted_layers_of "$WORK/tree2") in
  *we_dap_sync.c*) echo "canary layers: a layer listed with \$(OBJEXT) was named"; blind=1 ;;
esac

# The counted exception: found by the scan, silent with its allow line,
# named without it and when the file allows fewer or more.
printf '%s\n' 'int f(void) { return waitpid(p, &st, WNOHANG); }' > "$WORK/test_x.c"
planted=$(own_calls "$WORK/test_x.c")
printf '%s\n' 'test_x.c|waitpid|1|keep|planted' > "$WORK/allow1"
printf '%s\n' 'test_x.c|waitpid|0|keep|planted' > "$WORK/allow0"
printf '%s\n' 'test_x.c|waitpid|2|keep|planted' > "$WORK/allow2"
printf '%s\n' 'test_y.c|waitpid|1|keep|planted' > "$WORK/allowx"
printf '%s\n' 'test_x.c|fork|1|keep|planted' > "$WORK/allowf"
[ -n "$planted" ] && [ -z "$(judge "$WORK/allow1" "$planted")" ] || { echo "canary allow: a listed call was named or not found"; blind=1; }
[ -n "$(judge "$WORK/allow0" "$planted")" ] || { echo "canary allow: a call above its count was not named"; blind=1; }
[ -n "$(judge "$WORK/allow2" "$planted")" ] || { echo "canary allow: a count above the calls was not named"; blind=1; }
[ -n "$(judge "$WORK/allowx" "$planted")" ] || { echo "canary allow: a call in another file was not named"; blind=1; }
[ -n "$(judge "$WORK/allowf" "$planted")" ] || { echo "canary allow: a line for a call no rule reads was not named"; blind=1; }
[ -n "$(judge "$WORK/allow1" "")" ] || { echo "canary allow: a stale line was not named"; blind=1; }

if [ $blind -ne 0 ]; then
  echo "check-child-wait: the scanner is blind"
  exit $BAD
fi

[ $fail -eq 0 ] && echo "check-child-wait: clean"
exit $fail
