#!/bin/sh
# check-tty-open.sh -- the editor never takes a terminal as its own by opening it.
#
# Opening a terminal device without O_NOCTTY makes it the controlling
# terminal of a session leader that has none, which is what the editor is
# when a desktop launcher starts it.  The terminal the editor opens is the
# debugger's or a program's, which it hands to a child: the child then cannot
# take it (jdb's VM fails with "cannot give P a terminal"), and closing the
# other end sends the editor a hangup.
#
#   check-tty-open.sh [--srcdir DIR]
#
# Rule: in the top level sources, with comments blanked and string literals
# kept, every call of open(), open64(), openat() or openat64() whose
# arguments name a terminal (a name or string with "tty", "slave", or "pty"
# not inside a word such as "empty", or /dev/pts, /dev/console, /dev/vc)
# passes O_NOCTTY, and so does every call of posix_openpt().  A call of
# open() on /dev/null or a pipe is not read, nor a member called open.
# A terminal reached through a name that says none of this (a parameter
# called p) is not seen: the debugger rows that start the editor without a
# controlling terminal are the guard for that.
#
# Canaries: an offender planted in a file of its own, on one line and on
# two; a scanner that does not name it fails the script as blind, and so
# does one that names a call that passes the flag.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-tty-open: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/tty-open.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# scan FILE... - "file:line: call" for each open() of a terminal that does
# not pass O_NOCTTY.
scan() {
  awk '
    function report(at,   lo, hi, mid) {
      lo = 1; hi = nl
      while (lo < hi) {
        mid = int((lo + hi + 1) / 2)
        if (start[mid] <= at) lo = mid; else hi = mid - 1
      }
      print file ":" lo ": " fname call
    }
    function flush(   n, i, c, depth, j, low, rest, base, from) {
      n = length(all)
      i = 1
      while (i <= n) {
        rest = substr(all, i)
        if (!match(rest, /(^|[^A-Za-z0-9_.>])(open|open64|openat|openat64|posix_openpt)[ \t]*\(/)) break
        base = i + RSTART - 1
        from = base + RLENGTH - 1
        fname = substr(all, base, from - base)
        sub(/^[^A-Za-z_]/, "", fname)
        sub(/[ \t]+$/, "", fname)
        j = from + 1
        depth = 1
        while (j <= n && depth > 0) {
          c = substr(all, j, 1)
          if (c == "(") depth++
          else if (c == ")") depth--
          j++
        }
        call = substr(all, from, j - from)
        low = tolower(call)
        if (call !~ /O_NOCTTY/ && (fname == "posix_openpt" ||
            low ~ /tty|(^|[^a-z])pty|slave|\/dev\/(pts|console|vc)/))
          report(from)
        i = j
      }
    }
    FNR == 1 { if (nl) flush(); all = ""; nl = 0; incom = 0; file = FILENAME }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
          if (c == "\"") instr = 0
          i++; continue
        }
        if (d == "/*") { incom = 1; i += 2; continue }
        if (d == "//") break
        if (c == "\"") instr = 1
        out = out c; i++
      }
      nl++
      start[nl] = length(all) + 1
      all = all out " "
    }
    END { if (nl) flush() }' "$@"
}

fail=0
files=$(ls "$SRCDIR"/*.c "$SRCDIR"/*.h 2>/dev/null)
if [ -z "$files" ]; then
  echo "check-tty-open: no sources under $SRCDIR" >&2
  exit $BAD
fi
# shellcheck disable=SC2086
found=$(scan $files)
if [ -n "$found" ]; then
  echo "check-tty-open: open() of a terminal without O_NOCTTY:"
  echo "$found"
  fail=1
fi

# canary NAME CODE EXPECT - plant CODE in a file of its own; EXPECT is 1
# when the scanner must name it, 0 when it must stay silent.
canary() {
  printf '%s\n' "$2" > "$WORK/planted.c"
  got=$(scan "$WORK/planted.c")
  if [ "$3" = 1 ] && [ -z "$got" ]; then
    echo "canary $1: a planted offender was not found: $2"
    return 1
  fi
  if [ "$3" = 0 ] && [ -n "$got" ]; then
    echo "canary $1: a call that is fine was named: $2"
    return 1
  fi
  return 0
}

blind=0
canary one-line 'int f(void) { return open(e_d_tty, O_RDONLY | O_CLOEXEC); }' 1 || blind=1
canary slave 'int f(char *slave_name) { return open(slave_name, O_RDWR); }' 1 || blind=1
canary path 'int f(void) { return open("/dev/pts/3", O_WRONLY); }' 1 || blind=1
canary two-lines 'int f(void) { if ((a = open(e_d_tty, O_RDONLY | O_CLOEXEC)) < 0 ||
   (b = open(e_d_tty, O_WRONLY | O_CLOEXEC | O_NOCTTY)) < 0) return 1; }' 1 || blind=1
canary openat 'int f(void) { return openat(AT_FDCWD, e_d_pty_slave_name, O_RDWR); }' 1 || blind=1
canary open64 'int f(void) { return open64(tty, O_RDWR); }' 1 || blind=1
canary openpt 'int f(void) { return posix_openpt(O_RDWR); }' 1 || blind=1
canary openpt-guarded 'int f(void) { return posix_openpt(O_RDWR | O_NOCTTY); }' 0 || blind=1
canary empty 'int f(void) { return open(empty_file, O_RDONLY); }' 0 || blind=1
canary member 'int f(void) { return box->open(tty) + w.open(tty); }' 0 || blind=1
canary guarded'int f(void) { return open(spec->tty_path, O_RDWR | O_NOCTTY); }' 0 || blind=1
canary null 'int f(void) { return open("/dev/null", O_RDONLY); }' 0 || blind=1
canary comment 'int f(void) { /* open(tty, O_RDWR) */ return 0; }' 0 || blind=1
canary fopen 'int f(void) { return fopen(tty_name, "r") != 0; }' 0 || blind=1
if [ $blind -ne 0 ]; then
  echo "check-tty-open: the scanner is blind"
  exit $BAD
fi

[ $fail -eq 0 ] && echo "check-tty-open: clean"
exit $fail
