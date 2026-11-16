#!/bin/sh
# check-sh-idiom.sh -- the awk inside the check scripts is linear in the size
# of what it reads.
#
# The check scripts read whole C files in awk.  A scan that walks a file one
# character at a time through `substr(all, i, 1)`, or asks `length(all)` at
# every step, costs the size of the file for each step on an awk whose
# substr and length count characters, not bytes (gawk in a UTF-8 locale, the
# one-true-awk of FreeBSD 14 and OpenBSD 7.7): the check that took 7 s under
# gawk here ran past the 300 s deadline on two BSDs.  The idiom that stays
# linear on every awk: blank comments and literals one line at a time with
# the one blanker (tests/blank-c.awk, `blank_c`), and where a whole-file
# string cannot be avoided, `split(all, ch, "")` once, `n = length(all)` once,
# and index the array.
#
#   check-sh-idiom.sh [--srcdir DIR] [--selftest]
#
# Rule 2, over every tests/check-*.sh:
#  a. substr(V, e, 1) where V is a string the script builds up across lines
#     (a line of the form `V = V ...`): a character walk over a whole file.
#  b. X = substr(V, e) on the same V: a copy of the rest of the file for
#     every step of a scan.
#  c. length( inside the condition of a for or while: the length is taken
#     again at every step.  Take it once, before the loop.
# A line that carries `# linear:` and a reason (the string is one line, or
# one statement, and short) is not named.
#
# (Rule 1, that every check script sources one shared library for its work
# directory and traps, comes with that library.)
#
# Canaries (--selftest, and at the start of every run): an offender per
# clause, on one line, and the same line with the marker and with the
# linear idiom; a scanner that does not name every offender, or names a
# clean line, fails the script as blind.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
SELFTEST=0
while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-sh-idiom: unknown argument $1" >&2; exit 1 ;;
  esac
  shift
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/sh-idiom.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# scan FILE... - "file:line: clause: text" for each line of rule 2.
scan() {
  for f in "$@"; do
    awk -v file="$f" '
      function code_of(s) {
        sub(/^[ \t]+/, "", s)
        return s
      }
      function note_builder(line,   rest, name) {
        rest = line
        while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]=[ \t][A-Za-z_][A-Za-z0-9_]*/)) {
          name = substr(rest, RSTART, RLENGTH)
          split(name, parts, /[ \t]=[ \t]/)
          if (parts[1] == parts[2]) built[parts[1]] = 1
          rest = substr(rest, RSTART + RLENGTH)
        }
      }
      FNR == NR { note_builder($0); next }
      {
        text = code_of($0)
        if (text ~ /^#/ || $0 ~ /#[ \t]*linear:/) next
        for (v in built) {
          if ($0 ~ ("substr\\([ \t]*" v "[ \t]*,[^,]*,[ \t]*1[ \t]*\\)"))
            print file ":" FNR ": a. character walk over " v ": " text
          else if ($0 ~ ("=[ \t]*substr\\([ \t]*" v "[ \t]*,[^,)]*\\)"))
            print file ":" FNR ": b. copy of the rest of " v ": " text
        }
        if ($0 ~ /(while|for)[ \t]*\(.*length\(/)
          print file ":" FNR ": c. length( in a loop condition: " text
      }' "$f" "$f"
  done
}

# canary NAME CODE EXPECT - CODE is a line of awk inside a script that builds
# V by appending; EXPECT is the clause letter the scan must name, or "none".
canary() {
  printf '%s\n' '    { all = all $0 " " }' "$2" > "$WORK/plant.sh"
  got=$(scan "$WORK/plant.sh")
  if [ "$3" = none ]; then
    [ -z "$got" ] || { echo "canary $1: a clean line was named: $got"; return 1; }
    return 0
  fi
  case $got in
    *": $3. "*) return 0 ;;
  esac
  echo "canary $1: the offender was not named (wanted clause $3, got: $got)"
  return 1
}

blind=0
canary walk       '      for (i = 1; i <= n; i++) { c = substr(all, i, 1)' a || blind=1
canary walk-sp    '      c = substr(all , j, 1)' a || blind=1
canary rest       '        rest = substr(all, pos)' b || blind=1
canary rest-expr  '        rest = substr(all, at + plen)' b || blind=1
canary loop-for   '      for (i = from; i <= length(s); i++) {' c || blind=1
canary loop-while '      while (pos <= length(body)) {' c || blind=1
canary marked     '      c = substr(all, i, 1)  # linear: one line' none || blind=1
canary marked-len '      while (i <= length(s)) {  # linear: one line' none || blind=1
canary index-ok   '      c = ch[i]' none || blind=1
canary once-ok    '      n = length(all); for (i = 1; i <= n; i++) { c = ch[i] }' none || blind=1
canary slice-ok   '      after = substr(all, pos, 160)' none || blind=1
canary line-ok    '      c = substr(line, i, 1)' none || blind=1
canary comment-ok '      # for (i = 1; i <= length(all); i++) c = substr(all, i, 1)' none || blind=1
if [ $blind -ne 0 ]; then
  echo "check-sh-idiom: the scanner is blind"
  exit 1
fi
if [ $SELFTEST -eq 1 ]; then
  echo "check-sh-idiom: canaries ok"
  exit 0
fi

# This script holds the offenders of its canaries as text, so it is not read.
files=$(ls "$SRCDIR"/tests/check-*.sh 2>/dev/null | grep -v '/check-sh-idiom\.sh$')
if [ -z "$files" ]; then
  echo "check-sh-idiom: no check scripts under $SRCDIR/tests" >&2
  exit 1
fi
# shellcheck disable=SC2086
found=$(scan $files)
if [ -n "$found" ]; then
  echo "check-sh-idiom: awk that is not linear (blank per line with blank_c, split once, index the array):"
  printf '%s\n' "$found"
  exit 1
fi
echo "check-sh-idiom: clean"
exit 0
