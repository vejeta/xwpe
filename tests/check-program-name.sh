#!/bin/sh
# check-program-name.sh -- the name of what a build makes from a source has
# one owner.
#
# we_progname.[ch] work out the name of a program, an object and a debug
# binary from a source or a project, and where it is put.  A site that cuts a
# file name at a dot, writes a suffix of an output into a string, or joins a
# folder and a name it derived itself, is one more rule for the same name:
# the link, Run and the debugger each had their own and named three files.
#
#   check-program-name.sh [--allow FILE] [--srcdir DIR] [--list] [--strict]
#
# Rules, applied to the sources with comments blanked and string literals
# kept, counted per function:
#
#  1. A cut at a dot: WpeStringCutChar, strrchr or strchr with '.'.
#  2. A suffix of an output inside any string literal (".e", ".o",
#     ".class", ".dbg", ".dbgerr", "a.out", also "%s%s.dbg"), or a #define
#     of one.  Rule 2 reads every top level source except we_progname.[ch],
#     the others only the files that name outputs.
#  3. A folder joined to a name the same function derived: the plan's
#     operand call whose last argument is name, stem, exe, ofile, estr or
#     ostr; and any operand call whose folder is an Exedir, whatever the
#     name is called: what goes under Exedir is an output, placed by the
#     owner alone.
#  4. The end of a name compared with an extension by hand: strlen of a
#     table's .ext/->ext, or "name + n - el" against it.  A compare of
#     wpe_progname_extension() with the field is the owner's way and is not
#     counted.
#  5. (we_prog.c only) A decision about time: a comparison that reads
#     st_mtime, st_mtim or last_time, and any stat, lstat or access call.
#     What the build trusts as current is decided by e_build_current alone
#     (an output counts only when this editor built it here); a site that
#     compares a time stamp or stats a file of its own is how a shipped
#     program once counted as current.  A closed list: every function that
#     still does is in the allow file with its reason, and any other fails,
#     whatever its variables are called.
#
# tests/program-name.allow lists each function that still offends, with its
# count and the pass that moves it onto the owner ("owner") or the reason it
# is not a program's name ("keep").  The count may only fall: more fails,
# fewer fails until the line is lowered, a line that matches nothing fails
# as stale.  --strict ignores the "owner" lines, which is the goal once the
# callers have moved.  --list prints what was found as allow lines.
#
# Canaries: an offender planted for each rule in a copy of a scanned file; a
# rule that does not name it fails the script as blind.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
ALLOW=
LIST=0
STRICT=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    --strict) STRICT=1 ;;
    *) echo "check-program-name: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/program-name.allow

# The files that name what a build makes: all four rules read them.
# One line: the rule 2 sweep below skips these by matching " name ", and a
# newline in the list would scan a file twice.
NAMING_FILES="we_prog.c we_debug.c we_dap.c we_bsp.c we_progn.c we_fl_unix.c we_ai_ui.c we_ai_ws.c"
OWNER_FILES="we_progname.c we_progname.h"

# \047 is a single quote: awk turns it into one when it reads -v.
CUT_RE='(WpeStringCutChar|strrchr|strchr)[ \t]*[(][^;]*[\047][.][\047]'
SUFFIX_RE='[.](e|o|class|dbg|dbgerr)"|["/]a[.]out"|#[ \t]*define[ \t]+[A-Za-z0-9_]*(_SUFFIX|_EXT|_PROJECT_PROG)[ \t]+"[.](e|o|class|dbg|dbgerr)"'
JOIN_RE='wpe_startplan_operand(_in)?[ \t]*[(][^;]*,[ \t]*(name|stem|exe|ofile|estr|ostr)[ \t]*[)]|wpe_startplan_operand(_in)?[ \t]*[(][^,;]*,[^,;]*,[^,;]*[Ee][Xx][Ee][Dd][Ii][Rr][^,;]*,'
TABLE_RE='strlen[ \t]*[(][^;]*[.>]ext[ \t]*[)]|[+][ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*-[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*,[^;]*[.>]ext([^A-Za-z0-9_]|$)'
ALL_RE="$CUT_RE|$SUFFIX_RE|$JOIN_RE|$TABLE_RE"
# Rule 5, we_prog.c only: a comparison with a time in it ("->" is not a
# comparison), and a call that looks at a file.
TIME_RE='(st_mtime|st_mtim|last_time)[^;]*(<=|>=|==|!=|<|[^-]>)|(<=|>=|==|!=|<|[^-]>)[^;]*(st_mtime|st_mtim|last_time)'
STAT_RE='(^|[^A-Za-z0-9_])(stat|lstat|fstatat|statx|stat64|lstat64|access|faccessat|eaccess|euidaccess)[ \t]*[(]'
WE_PROG_RE="$ALL_RE|$TIME_RE|$STAT_RE"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/program-name.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

# count_by_function FILE PATH PATTERN - "file function count" for each
# function whose lines match PATTERN, comments blanked, string and character
# literals kept.  A function starts at a line in column 0 that names an
# identifier followed by "(" and does not end with ";".  A #define is its own
# function, named "#define NAME".  "#fn file function" lines list the
# functions seen.
count_by_function() {
  awk -v file="$1" -v re="$3" '
    BEGIN { incom = 0; fn = "(file scope)" }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0; inchr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
          if (c == "\"") instr = 0
          i++; continue
        }
        if (inchr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
          if (c == "\047") inchr = 0
          i++; continue
        }
        if (d == "/*") { incom = 1; i += 2; continue }
        if (d == "//") break
        if (c == "\"") instr = 1
        if (c == "\047") inchr = 1
        out = out c; i++
      }
      here = fn
      if (out ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/) {
        nm = out; sub(/^[ \t]*#[ \t]*define[ \t]+/, "", nm); sub(/[^A-Za-z0-9_].*$/, "", nm)
        here = "#define " nm
      } else if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        if (name != "") { fn = name; here = fn; seen[fn] = 1 }
      }
      s = out
      while (match(s, re)) { cnt[here]++; s = substr(s, RSTART + RLENGTH) }
    }
    END {
      for (f in cnt) print file "|" f "|" cnt[f]
      for (f in seen) print "#fn|" file "|" f
    }' "$2"
}

# scan_tree DIR OUT - what the scan finds under DIR, "file function count".
# Names the files it could not find.
scan_tree() {
  dir=$1; out=$2
  : > "$out"; : > "$out.fn"
  for f in $NAMING_FILES; do
    if [ ! -f "$dir/$f" ]; then
      [ "$QUIET" = 1 ] || problem "canary: $f not found under $dir (the scan is blind)"
      continue
    fi
    re=$ALL_RE
    [ "$f" = we_prog.c ] && re=$WE_PROG_RE
    count_by_function "$f" "$dir/$f" "$re" >> "$out.raw"
  done
  for p in "$dir"/*.c "$dir"/*.h; do
    [ -f "$p" ] || continue
    f=${p##*/}
    case " $NAMING_FILES $OWNER_FILES config.h " in *" $f "*) continue ;; esac
    count_by_function "$f" "$p" "$SUFFIX_RE" >> "$out.raw"
  done
  [ -f "$out.raw" ] || : > "$out.raw"
  grep -v '^#fn|' "$out.raw" | sort > "$out"
  grep '^#fn|' "$out.raw" > "$out.fn"
  rm -f "$out.raw"
}

# canary RULE FILE FUNCTION CODE - plant CODE in a copy of FILE alone and
# fail unless the scan reports FUNCTION in it.
canary() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$4" > "$c/$2"
  QUIET=1 scan_tree "$c" "$c/out"
  if ! grep -qF "$2|$3|" "$c/out"; then
    echo "canary $1: a planted offender was not found: $4"
    return 1
  fi
  return 0
}

# allowed_canary FILE FUNCTION CODE - the owner's way of doing the same
# thing: the scan must NOT report FUNCTION, or the rule is too wide.
allowed_canary() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$3" > "$c/$1"
  QUIET=1 scan_tree "$c" "$c/out"
  if grep -qF "$1|$2|" "$c/out"; then
    echo "allowed canary: the owner's own way was reported: $3"
    return 1
  fi
  return 0
}

blind=0
canary R1 we_prog.c planted "void planted(char *s) { WpeStringCutChar(s, '.'); }" || blind=1
canary R1 we_debug.c planted "void planted(const char *s) { const char *p = strrchr(s, '.'); (void)p; }" || blind=1
canary R2 we_debug.c planted 'void planted(char *b, const char *d, const char *s) { snprintf(b, 9, "%s%s.dbg", d, s); }' || blind=1
canary R2 we_prog.c '#define E_X_SUFFIX' '#define E_X_SUFFIX ".e"' || blind=1
canary R3 we_prog.c planted 'void planted(char *o, const char *dir, const char *stem) { wpe_startplan_operand(o, 9, dir, stem); }' || blind=1
canary R3 we_prog.c planted 'void planted(char *o, FENSTER *f) { wpe_startplan_operand(o, 9, e_prog.exedir, f->datnam); }' || blind=1
allowed_canary we_prog.c planted 'void planted(char *o, FENSTER *f) { wpe_startplan_operand(o, 9, f->dirct, f->datnam); }' || blind=1
canary R4 we_debug.c planted 'void planted(int i, const char *n, int len) { int el = strlen(T[i].ext); if (!strcmp(n + len - el, T[i].ext)) return; }' || blind=1
canary R4 we_debug.c planted 'void planted(const char *n, int len, const E *p) { if (!strcmp(n + len - el, p->ext)) return; }' || blind=1
allowed_canary we_debug.c planted 'void planted(int i, const char *n) { if (!strcmp(wpe_progname_extension(n), T[i].ext)) return; }' || blind=1
canary R5 we_prog.c planted 'void planted(struct stat *sb, struct stat *zz) { if (sb->st_mtime >= zz->st_mtime) return; }' || blind=1
canary R5 we_prog.c planted 'void planted(M_TIME qq) { if (qq < last_time) return; }' || blind=1
canary R5 we_prog.c planted 'void planted(const char *p) { struct stat sb; stat(p, &sb); }' || blind=1
canary R5 we_prog.c planted 'void planted(const char *p) { if (access(p, 0)) return; }' || blind=1
canary R5 we_prog.c planted 'void planted(const char *p) { if (faccessat(0, p, 0, 0)) return; }' || blind=1
canary R5 we_prog.c planted 'void planted(const char *p) { struct stat sb; fstatat(0, p, &sb, 0); }' || blind=1
allowed_canary we_prog.c planted 'void planted(const char *p, struct stat *sb) { e_build_stat(p, sb); (void)sb->st_mtime; }' || blind=1
allowed_canary we_debug.c planted 'void planted(const char *p) { struct stat sb; stat(p, &sb); }' || blind=1

scan_tree "$SRCDIR" "$WORK/found"
grep -q '^#fn|we_prog.c|e_comp_operands$' "$WORK/found.fn" ||
  problem "canary: e_comp_operands not seen as a function of we_prog.c (the function scan is blind)"

if [ $LIST -eq 1 ]; then
  awk -F'|' '{ print $1 "|" $2 "|" $3 "|PASS|reason" }' "$WORK/found"
  awk '{ print "# " $0 }' "$WORK/problems"
  exit 0
fi

: > "$WORK/allowed"
if [ -r "$ALLOW" ]; then
  awk -F'|' -v strict="$STRICT" '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || ($4 != "owner" && $4 != "keep")) { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 " " $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      if (strict && $4 == "owner") next
      print $1 "|" $2 "|" $3 "|" $4
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  problem "no allow file at $ALLOW"
fi

awk -F'|' '
  FILENAME == ARGV[1] { al[$1 "|" $2] = $3; pass[$1 "|" $2] = $4; next }
  {
    key = $1 "|" $2; got[key] = 1
    if (!(key in al)) {
      print "offender: " $1 ": " $2 " names an output by a rule of its own (" $3 " line(s)): move it onto we_progname, or list it with its pass"; bad = 1
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

if [ "$blind" -ne 0 ] || [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-program-name: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
echo "check-program-name: ok ($(awk -F'|' '{ n += $3 } END { print n + 0 }' "$WORK/found") line(s) in $(wc -l < "$WORK/found" | tr -d ' ') listed function(s))"
exit 0
