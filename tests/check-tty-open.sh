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
ALLOW=

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    *) echo "check-tty-open: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

[ -n "$ALLOW" ] || ALLOW=$SRCDIR/tests/tty-open.allow
if [ ! -f "$ALLOW" ]; then
  echo "check-tty-open: no allow file at $ALLOW" >&2
  exit $BAD
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/tty-open.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# The scan reads C as bytes (substr and length count characters in a UTF-8
# locale, which costs the size of the file at every step on some awks).
LC_ALL=C
export LC_ALL
# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# scan FILE... - "file:line: call" for each open() of a terminal that does
# not pass O_NOCTTY.  The blanked lines of a file are kept as an array of
# pieces and walked as an array of characters (blank-c.awk), never joined
# into one string.
scan() {
  awk "$BLANK_C"'
    function report(at,   lo, hi, mid) {
      lo = 1; hi = nl
      while (lo < hi) {
        mid = int((lo + hi + 1) / 2)
        if (start[mid] <= at) lo = mid; else hi = mid - 1
      }
      print file ":" lo ": " fname call
    }
    function opener(name) {
      return (name == "open" || name == "open64" || name == "openat" || name == "openat64" || name == "posix_openpt")
    }
    # after_call(from, n) - the index just past the ")" that closes the "("
    # at from.
    function after_call(from, n,   j, depth, c) {
      j = from + 1
      depth = 1
      while (j <= n && depth > 0) {
        c = ch[j]
        if (c == "(") depth++
        else if (c == ")") depth--
        j++
      }
      return j
    }
    # judge_call(name, from, n) - report the call of NAME whose "(" is at
    # from when it names a terminal and passes no O_NOCTTY; returns the
    # index to go on from.
    function judge_call(name, from, n,   j, low) {   # report() prints the global call
      fname = name
      j = after_call(from, n)
      call = chars_span(ch, from, j - 1)
      low = tolower(call)
      if (call !~ /O_NOCTTY/ && (fname == "posix_openpt" ||
          low ~ /tty|(^|[^a-z])pty|slave|\/dev\/(pts|console|vc)/))
        report(from)
      return j
    }
    function flush(   n, i, j, m, name) {
      if (!mentioned) return
      n = chars_load(seg, nl, ch)
      i = 1
      while (i <= n) {
        if (ch[i] !~ /[A-Za-z0-9_]/) { i++; continue }
        j = i; name = ""
        while (j <= n && ch[j] ~ /[A-Za-z0-9_]/) { name = name ch[j]; j++ }
        m = j
        while (m <= n && ch[m] ~ /[ \t]/) m++
        if (opener(name) && ch[i - 1] != "." && ch[i - 1] != ">" && m <= n && ch[m] == "(") {
          i = judge_call(name, m, n)
          continue
        }
        i = j
      }
    }
    FNR == 1 { if (nl) flush(); delete seg; nl = 0; alln = 0; blank_incom = 0; mentioned = 0; file = FILENAME }
    {
      out = blank_c($0, 1)
      if (index(out, "open")) mentioned = 1
      nl++
      start[nl] = alln + 1
      seg[nl] = out " "
      alln += length(seg[nl])
    }
    END { if (nl) flush() }' "$@"
}

# judge ALLOWFILE FOUND - what is left of the scan's FOUND lines once the
# counted exceptions of ALLOWFILE are taken off: a call the file does not
# list, more calls than it says, fewer than it says (lower or delete the
# line), and a line that matches nothing.  Prints one problem a line.
judge() {
  printf '%s\n' "$2" | awk -F'|' -v allow="$1" '
    function base(s) { sub(/:[0-9]+: .*$/, "", s); sub(/^.*\//, "", s); return s }
    function callname(s,   c) { c = s; sub(/^[^ ]*: /, "", c); sub(/[^A-Za-z0-9_].*$/, "", c); return c }
    BEGIN {
      while ((getline line < allow) > 0) {
        if (line ~ /^[ \t]*(#|$)/) continue
        n = split(line, f, "|")
        if (n < 5 || f[3] !~ /^[0-9]+$/ || f[4] == "" || f[5] == "") { print "bad line in the allow file: " line; continue }
        key = f[1] "|" f[2]
        if (key in al) { print "duplicate allow line for " key; continue }
        al[key] = f[3]; why[key] = f[4]
      }
    }
    NF == 0 { next }
    {
      key = base($0) "|" callname($0)
      seen[key]++
      if (!(key in al)) print "open() of a terminal without O_NOCTTY: " $0
    }
    END {
      for (k in seen) if (k in al) {
        if (seen[k] > al[k] + 0) print "more than allowed: " k " has " seen[k] ", the allow file says " al[k]
        else if (seen[k] < al[k] + 0) print "fewer than the allow file says: " k " has " seen[k] ", lower the line"
      }
      for (k in al) if (!(k in seen)) print "stale allow line, delete it: " k
    }' | sort
}

fail=0
files=$(ls "$SRCDIR"/*.c "$SRCDIR"/*.h 2>/dev/null)
if [ -z "$files" ]; then
  echo "check-tty-open: no sources under $SRCDIR" >&2
  exit $BAD
fi
# shellcheck disable=SC2086
found=$(scan $files)
left=$(judge "$ALLOW" "$found")
if [ -n "$left" ]; then
  echo "check-tty-open: a terminal opened without O_NOCTTY, or an allow line that is off:"
  echo "$left"
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
canary guarded 'int f(void) { return open(spec->tty_path, O_RDWR | O_NOCTTY); }' 0 || blind=1
canary null 'int f(void) { return open("/dev/null", O_RDONLY); }' 0 || blind=1
canary comment 'int f(void) { /* open(tty, O_RDWR) */ return 0; }' 0 || blind=1
canary fopen 'int f(void) { return fopen(tty_name, "r") != 0; }' 0 || blind=1
# The counted exception: exactly the child's own open, found by the scan,
# is silent with its allow line, named without it, and named when the file
# allows fewer or more than it finds.
printf '%s\n' 'int f(const char *slave_name) { return open(slave_name, O_RDWR); }' > "$WORK/we_child.c"
planted=$(scan "$WORK/we_child.c")
printf '%s\n' 'we_child.c|open|1|keep|planted' > "$WORK/allow1"
printf '%s\n' 'we_child.c|open|0|keep|planted' > "$WORK/allow0"
printf '%s\n' 'we_child.c|open|2|keep|planted' > "$WORK/allow2"
printf '%s\n' 'we_other.c|open|1|keep|planted' > "$WORK/allowx"
[ -n "$planted" ] && [ -z "$(judge "$WORK/allow1" "$planted")" ] || { echo "canary allow: a listed call was named or not found"; blind=1; }
[ -n "$(judge "$WORK/allow0" "$planted")" ] || { echo "canary allow: a call above its count was not named"; blind=1; }
[ -n "$(judge "$WORK/allow2" "$planted")" ] || { echo "canary allow: a count above the calls was not named"; blind=1; }
[ -n "$(judge "$WORK/allowx" "$planted")" ] || { echo "canary allow: a call in another file was not named"; blind=1; }
[ -n "$(judge "$WORK/allow1" "")" ] || { echo "canary allow: a stale line was not named"; blind=1; }
if [ $blind -ne 0 ]; then
  echo "check-tty-open: the scanner is blind"
  exit $BAD
fi

[ $fail -eq 0 ] && echo "check-tty-open: clean"
exit $fail
