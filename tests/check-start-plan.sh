#!/bin/sh
# check-start-plan.sh -- what the editor decides about a program has one owner.
#
# A program the editor starts for the user is described whole by a start
# plan (we_startplan.c) before it starts: its command line with where each
# word came from, its working folder, the paths it writes and removes.  The
# plan is what the user is shown and what the children's owner checks.  A
# site that builds a command line, a working folder or a shell line of its
# own, or hands a folder's program keys (Exedir, Arguments, IncludePath, the
# program's name) to a program by itself, is how a folder's value reached a
# program unseen, one site after another.
#
#   check-start-plan.sh [--allow FILE] [--srcdir DIR] [--list]
#
# The source scan: in the files that start programs, every line that builds
# a start (the patterns below) is counted per function.  tests/start-plan.allow
# lists each function that still does, with its count and the pass that
# moves it into a plan ("plan" for a function that hands the words to
# wpe_startplan_start, which takes only words the judged plan holds).  The
# count may only fall: more than listed fails, fewer fails until the line is
# lowered, a line that matches nothing fails as stale.  A folder value made
# an option (WPE_WORD_OPTION with WPE_FROM_FOLDER on one line) fails with no
# allow line possible.  --list prints what was found, as allow lines.
#
# The symbol half (who imports wpe_child_start and the capture calls) is a
# rule of tests/check-one-owner.sh, with its own allow file.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
ALLOW=
LIST=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    *) echo "check-start-plan: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/start-plan.allow

# The files that start programs, and the one that must not widen the
# assistant's reads with a folder's IncludePath.
FILES="we_prog.c we_debug.c we_bsp.c we_fl_unix.c we_lsp.c we_dap.c we_progn.c
  we_xterm.c we_term.c we_ai_ws.c"

# What builds a start outside the owner: a child spec, a command line or a
# working folder assigned, a shell line, the old argv builders, a folder's
# program keys handed on, a build output's path, the shell entries.
PATTERN='WpeChildSpec|[.]argv[ \t]*=[^=]|[.]cwd[ \t]*=[^=]|"/bin/sh"|e_add_arg[ \t]*[(]|e_make_arg[ \t]*[(]|e_make_library[ \t]*[(]|e_prog[.]exedir|e_prog[.]arguments|e_prog[.]sys_include|e_s_prog[.]exe_name|g_prj[.]ofile|e_u_system|e_system[ \t]*[(]|e_shell_run[ \t]*[(]'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/start-plan.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/found"
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

# count_by_function FILE - "file function count" for each function whose
# lines match PATTERN, comments blanked, string literals kept (a shell's
# path is a literal).  A function starts at a line in column 0 that names
# an identifier followed by "(" and does not end with ";" (a prototype).
count_by_function() {
  awk -v file="$1" -v re="$PATTERN" '
    BEGIN { incom = 0; fn = "(file scope)" }
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
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        if (name != "") { fn = name; seen[fn] = 1 }
      }
      s = out
      while (match(s, re)) { cnt[fn]++; s = substr(s, RSTART + RLENGTH) }
    }
    END {
      for (f in cnt) print file, f, cnt[f]
      for (f in seen) print "#fn", file, f
    }' "$2"
}

for f in $FILES; do
  if [ ! -f "$SRCDIR/$f" ]; then
    problem "canary: $f not found under $SRCDIR (the scan is blind)"
    continue
  fi
  count_by_function "$f" "$SRCDIR/$f" >> "$WORK/scan"
  # A folder value made an option, on one line: never allowed.
  grep -n 'WPE_WORD_OPTION' "$SRCDIR/$f" | grep 'WPE_FROM_FOLDER' | while IFS= read -r hit; do
    echo "a folder value made an option: $f:${hit%%:*}"
  done >> "$WORK/problems"
done
grep -v '^#fn ' "$WORK/scan" | sort > "$WORK/found"
grep '^#fn ' "$WORK/scan" > "$WORK/functions"

# The canaries: a scanner that names no functions, or misses the build's
# own compile, sees nothing.
grep -q '^#fn we_prog.c e_comp_start$' "$WORK/functions" ||
  problem "canary: e_comp_start not seen as a function of we_prog.c (the function scan is blind)"
grep -q '^we_prog.c e_add_arg ' "$WORK/found" ||
  problem "canary: the definition of e_add_arg not counted in we_prog.c (the pattern scan is blind)"

# The refusal of a start with no plan is off only in the children's owner's
# own unit test, whose specs are built by hand: a build of the editor, or any
# other program, that names the switch has the refusal off.
MECHANICS=WPE_CHILD_MECHANICS_TEST
for f in "$SRCDIR"/*.c "$SRCDIR"/*.h "$SRCDIR/configure.ac"; do
  [ -f "$f" ] || continue
  case ${f##*/} in we_child.c) continue ;; esac
  if grep -q "$MECHANICS" "$f"; then
    problem "$MECHANICS named in ${f##*/}: only test_child may build with unplanned starts allowed"
  fi
done
if [ -f "$SRCDIR/Makefile.am" ]; then
  grep -n "$MECHANICS" "$SRCDIR/Makefile.am" | grep -v -e ':test_child_CFLAGS[ \t]*=' -e '^[0-9]*:#' | while IFS= read -r hit; do
    echo "$MECHANICS named in Makefile.am:${hit%%:*} outside test_child_CFLAGS"
  done >> "$WORK/problems"
fi

if [ $LIST -eq 1 ]; then
  awk '{ print $1 "|" $2 "|" $3 "|PASS|reason" }' "$WORK/found"
  awk '{ print "# " $0 }' "$WORK/problems"
  exit 0
fi

: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || $4 == "") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 " " $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print $1, $2, $3, $4
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  problem "no allow file at $ALLOW"
fi

awk '
  FILENAME == ARGV[1] { al[$1 " " $2] = $3; pass[$1 " " $2] = $4; next }
  {
    key = $1 " " $2; got[key] = 1
    if (!(key in al)) {
      print "offender: " $1 ": " $2 " builds a start outside the plan (" $3 " line(s)): put it in a plan, or list it with its pass"; bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 ": " $2 " has " $3 ", the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 ": " $2 " has " $3 ", lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in got)) { print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-start-plan: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
echo "check-start-plan: ok ($(awk '{ n += $3 } END { print n + 0 }' "$WORK/found") line(s) in $(wc -l < "$WORK/found" | tr -d ' ') listed function(s))"
exit 0
