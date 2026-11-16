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
#  6. A mention of comp_sw (the compiler style), in any scanned file.  Run's
#     kind is a field of the language's entry (e_run_kind_of); the style
#     keeps to the build steps, the message format, the dialogs, the project
#     file and the defaults it was made for.  A closed list: every function
#     that still mentions it is in the allow file with its reason, and one
#     that is not fails, whatever it is called.  A function that reads the
#     style to decide what Run starts is how Rust, Java and Go once ran their
#     compiler twice.
#  7. (we_prog.c, we_debug.c, we_opt.c) Who judges a language's value and who
#     picks a toolchain, a closed rule with no allow lines: a language's key
#     is judged only by e_opt_lang_value_from, against the user's own entry
#     for the file; a per-file toolchain pick is applied only by
#     e_prog_switch_apply; a language's value is noted as the user's own only
#     when he types it; and only e_opt_value_from, e_opt_lang_value_from and
#     e_prj_part_from ask whether a value is his.  Its symbol half reads
#     `nm -u` of we_prog.o and we_debug.o as the debugger's does: with no
#     objects named the half is skipped with a message, and an object that is
#     not built fails.
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
# Rule 6: any mention of the compiler style.  A name, not a shape, because the
# style was once read as "the compiler is an interpreter" in two places that
# looked nothing alike.
COMPSW_RE='(^|[^A-Za-z0-9_])comp_sw([^A-Za-z0-9_]|$)'
ALL_RE="$CUT_RE|$SUFFIX_RE|$JOIN_RE|$TABLE_RE|$COMPSW_RE"
# Rule 5, we_prog.c only: a comparison with a time in it ("->" is not a
# comparison), and a call that looks at a file.
TIME_RE='(st_mtime|st_mtim|last_time)[^;]*(<=|>=|==|!=|<|[^-]>)|(<=|>=|==|!=|<|[^-]>)[^;]*(st_mtime|st_mtim|last_time)'
STAT_RE='(^|[^A-Za-z0-9_])(stat|lstat|fstatat|statx|stat64|lstat64|access|faccessat|eaccess|euidaccess)[ \t]*[(]'
WE_PROG_RE="$ALL_RE|$TIME_RE|$STAT_RE"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/program-name.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

# count_by_function FILE PATH PATTERN [SKIP [OK]] - "file function count" for
# each function whose lines match PATTERN, comments blanked, string and
# character literals kept.  A function starts at a line in column 0 that names
# an identifier followed by "(" and does not end with ";".  A #define is its own
# function, named "#define NAME".  "#fn file function" lines list the
# functions seen.  A line that matches SKIP counts nothing (a definition or a
# prototype starts in column 0, a call does not), and what matches OK is cut
# out of a line before it is counted (the call the owner allows).
count_by_function() {
  awk -v file="$1" -v re="$3" -v skip="$4" -v okre="$5" '
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
      if (skip != "" && out ~ skip) s = ""
      else if (okre != "") gsub(okre, "", s)
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
canary R6 we_prog.c e_run_planted 'void e_run_planted(void) { if (e_s_prog.comp_sw & 1) return; }' || blind=1
canary R6 we_debug.c planted 'void planted(const struct e_s_prog *p) { int x = p->comp_sw; (void)x; }' || blind=1
canary R5 we_prog.c planted 'void planted(const char *p) { if (faccessat(0, p, 0, 0)) return; }' || blind=1
canary R5 we_prog.c planted 'void planted(const char *p) { struct stat sb; fstatat(0, p, &sb, 0); }' || blind=1
allowed_canary we_prog.c planted 'void planted(const char *p, struct stat *sb) { e_build_stat(p, sb); (void)sb->st_mtime; }' || blind=1
allowed_canary we_debug.c planted 'void planted(const char *p) { struct stat sb; stat(p, &sb); }' || blind=1

# debug_literals DIR - "file function count" for each function of we_debug.c
# that writes the name or suffix of an output in a literal.  The debugger has
# moved onto the owner, so this is not a count an allow file can hold.
debug_literals() {
  [ -f "$1/we_debug.c" ] || return 0
  count_by_function we_debug.c "$1/we_debug.c" "$SUFFIX_RE" | grep -v '^#fn|'
}

# debug_literal_canary - a literal planted in a copy of we_debug.c must be
# named by debug_literals, or the rule is blind.
debug_literal_canary() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' 'void planted(char *b, const char *d) { snprintf(b, 9, "%s.dbg", d); }' > "$c/we_debug.c"
  if ! debug_literals "$c" | grep -q '^we_debug.c|planted|'; then
    echo "canary R2 (debugger): a planted literal was not found"
    return 1
  fi
  return 0
}
debug_literal_canary || blind=1

debug_literals "$SRCDIR" > "$WORK/debug-literals"
while IFS='|' read -r dfile dfn dcount; do
  [ -n "$dfn" ] || continue
  problem "offender: $dfile: $dfn writes the name of an output in a literal ($dcount): the debugger names files through we_progname, and no allow line can say otherwise"
done < "$WORK/debug-literals"

# Rule 7, who judges a language's value and who picks a toolchain.  A closed
# rule with no allow lines, scanned in we_prog.c, we_debug.c and we_opt.c:
#
#  a. e_opt_value_from( whose first argument is not the literal
#     E_OPT_SCOPE_PROGRAMMING.  A language's key is judged against the user's
#     own entry for the file by e_opt_lang_value_from alone.
#  b. e_algol68_apply( outside e_prog_switch_apply: the one place a per-file
#     toolchain pick is applied, so the user's own copy can be given it.
#  c. e_opt_note_own_field( with a first argument other than
#     E_OPT_SCOPE_PROGRAMMING outside e_opt_value_typed_in: a language's
#     value is noted as his only when he types it, never at load, where the
#     note was a second copy of the baseline that could not follow the file.
#  d. e_opt_field_is_his( outside e_opt_value_from, e_opt_lang_value_from and
#     e_prj_part_from: no other site may judge a value without the baseline.
#
# A definition or a prototype starts in column 0 and counts nothing.
R7_FILES="we_prog.c we_debug.c we_opt.c"
R7_SKIP='^[A-Za-z_]'
R7_A_RE='(^|[^A-Za-z0-9_])e_opt_value_from[ \t]*[(]'
R7_A_OK='(^|[^A-Za-z0-9_])e_opt_value_from[ \t]*[(][ \t]*E_OPT_SCOPE_PROGRAMMING[ \t]*,'
R7_B_RE='(^|[^A-Za-z0-9_])e_algol68_apply[ \t]*[(]'
R7_C_RE='(^|[^A-Za-z0-9_])e_opt_note_own_field[ \t]*[(]'
R7_C_OK='(^|[^A-Za-z0-9_])e_opt_note_own_field[ \t]*[(][ \t]*E_OPT_SCOPE_PROGRAMMING[ \t]*,'
R7_D_RE='(^|[^A-Za-z0-9_])e_opt_field_is_his[ \t]*[(]'

# r7_rule TAG DIR RE OK ALLOWED - "TAG|file|function|count" for each function
# of the rule 7 files under DIR that matches RE, except the ALLOWED functions.
r7_rule() {
  for f in $R7_FILES; do
    [ -f "$2/$f" ] || continue
    count_by_function "$f" "$2/$f" "$3" "$R7_SKIP" "$4" | grep -v '^#fn|' |
      awk -F'|' -v tag="$1" -v allowed=" $5 " 'index(allowed, " " $2 " ") == 0 { print tag "|" $0 }'
  done
}

# rule7_scan DIR - every offender of rule 7 under DIR.
rule7_scan() {
  r7_rule 7a "$1" "$R7_A_RE" "$R7_A_OK" ""
  r7_rule 7b "$1" "$R7_B_RE" "" "e_prog_switch_apply"
  r7_rule 7c "$1" "$R7_C_RE" "$R7_C_OK" "e_opt_value_typed_in"
  r7_rule 7d "$1" "$R7_D_RE" "" "e_opt_value_from e_opt_lang_value_from e_prj_part_from"
}

# canary7 TAG FILE FUNCTION CODE - plant CODE in a copy of FILE alone and fail
# unless rule 7 names FUNCTION under TAG.
canary7() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$4" > "$c/$2"
  if ! rule7_scan "$c" | grep -qF "$1|$2|$3|"; then
    echo "canary R7 ($1): a planted offender was not found: $4"
    return 1
  fi
  return 0
}

# allowed_canary7 FILE FUNCTION CODE - the owner's way of doing the same
# thing: rule 7 must NOT name FUNCTION, or the rule is too wide.
allowed_canary7() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$3" > "$c/$1"
  if rule7_scan "$c" | grep -qF "|$1|$2|"; then
    echo "allowed canary R7: the owner's own way was reported: $3"
    return 1
  fi
  return 0
}

# The planted code is laid out as the sources are: the definition in column 0
# and the body indented.
canary7 7a we_prog.c e_run_planted 'static void e_run_planted(const char *x)
{
 e_opt_value_from(e_s_prog.language, "Compiler", x);
}' || blind=1
canary7 7a we_debug.c e_d_planted 'static void e_d_planted(const char *x)
{
 if (e_opt_value_from(
      E_OPT_SCOPE_PROGRAMMING, "Exedir", x))
  return;
}' || blind=1
allowed_canary7 we_prog.c e_run_prog_from 'static int e_run_prog_from(const char *k, const char *x)
{
 return(e_opt_value_from(E_OPT_SCOPE_PROGRAMMING, k, x));
}' || blind=1
canary7 7b we_prog.c planted 'static void planted(void)
{
 e_algol68_apply(&e_s_prog, 1);
}' || blind=1
allowed_canary7 we_prog.c e_prog_switch_apply 'static void e_algol68_apply(struct e_s_prog *cp, int use_ga68);
static void e_prog_switch_apply(struct e_s_prog *entry, int which)
{
 e_algol68_apply(entry, which == 2);
}' || blind=1
canary7 7c we_opt.c planted 'static void planted(struct e_s_prog *p)
{
 e_opt_note_own_field(p->language, "Run", "program");
}' || blind=1
allowed_canary7 we_opt.c e_opt_value_typed_in 'void e_opt_value_typed_in(const char *scope, const char *key, const char *value)
{
 e_opt_note_own_field(scope, key, value);
}' || blind=1
allowed_canary7 we_opt.c e_opt_note_own_prog 'static void e_opt_note_own_prog(void)
{
 e_opt_note_own_field(E_OPT_SCOPE_PROGRAMMING, k, v);
}' || blind=1
canary7 7d we_opt.c planted 'static void planted(const char *l, const char *v)
{
 if (e_opt_field_is_his(l, "Compiler", v, 0))
  return;
}' || blind=1
allowed_canary7 we_prog.c e_prj_part_from 'static int e_prj_part_from(const char *name, const char *value)
{
 return(e_opt_field_is_his(name, name, value, 0));
}' || blind=1
allowed_canary7 we_opt.c e_opt_lang_value_from 'int e_opt_lang_value_from(const struct e_s_prog *base, const char *language,
                          const char *key, const char *value)
{
 if (e_opt_field_is_his(language, key, value, 0))
  return(0);
 return(1);
}' || blind=1

for f in $R7_FILES; do
  [ -f "$SRCDIR/$f" ] || problem "canary: $f not found under $SRCDIR (rule 7 is blind)"
done
rule7_scan "$SRCDIR" > "$WORK/rule7"
while IFS='|' read -r r7tag r7file r7fn r7count; do
  [ -n "$r7fn" ] || continue
  case $r7tag in
    7a) r7why="judges a language's value by e_opt_value_from: a language's key goes through e_opt_lang_value_from, against the user's own entry for the file" ;;
    7b) r7why="applies a toolchain pick by itself: e_prog_switch_apply is the one place, so the user's own copy gets the same pick" ;;
    7c) r7why="notes a language's value as the user's own: only a value he types is noted (e_opt_value_typed_in)" ;;
    7d) r7why="judges a value as the user's own without the baseline: only e_opt_value_from, e_opt_lang_value_from and e_prj_part_from may" ;;
  esac
  problem "offender: rule $r7tag: $r7file: $r7fn ($r7count) $r7why, and no allow line can say otherwise"
done < "$WORK/rule7"

# The symbol half: the debugger's object imports the owner's functions, so a
# debugger that names the program or the debug binary by its own rule cannot
# pass by having no literal left.  Read from the objects make check names.
NMTOOL=${NM:-nm}
DEBUG_OBJ=
PROG_OBJ=
for o in $ONE_OWNER_OBJECTS; do
  case $o in *we_debug.o) DEBUG_OBJ=$o ;; *we_prog.o) PROG_OBJ=$o ;; esac
done
SYMBOL_SKIPPED=0
if [ -z "$ONE_OWNER_OBJECTS" ]; then
  echo "check-program-name: symbol half skipped: ONE_OWNER_OBJECTS is empty"
elif [ -z "$DEBUG_OBJ" ]; then
  problem "symbol: ONE_OWNER_OBJECTS names no we_debug.o: the symbol half cannot read the debugger's imports"
elif [ ! -f "$DEBUG_OBJ" ]; then
  problem "symbol: $DEBUG_OBJ is not built: the symbol half cannot read the debugger's imports"
elif [ "$NMTOOL" = ":" ] || ! command -v "$NMTOOL" >/dev/null 2>&1; then
  echo "check-program-name: symbol half skipped: no nm ($NMTOOL)"
  SYMBOL_SKIPPED=1
else
  . "$HERE/objsyms.sh"
  "$NMTOOL" -u "$DEBUG_OBJ" 2>/dev/null | nm_undefined_names > "$WORK/debug-imports"
  for sym in wpe_progname_program wpe_progname_debug_binary wpe_progname_debug_errors wpe_progname_stem wpe_progname_extension; do
    grep -qx "$sym" "$WORK/debug-imports" ||
      problem "symbol: $DEBUG_OBJ does not import $sym: the debugger names a program, a debug binary or a class by a rule of its own"
  done
  grep -qx e_opt_lang_value_from "$WORK/debug-imports" ||
    problem "symbol: $DEBUG_OBJ does not import e_opt_lang_value_from: the debugger judges the language's Executable by a rule of its own"
  # Rule 7, the symbol half: Run, the build, the project and the dialog take
  # the user's own entry for the file, and no caller is left on the old
  # name (which no longer exists, so one fails to link before this runs).
  if [ -z "$PROG_OBJ" ] || [ ! -f "$PROG_OBJ" ]; then
    problem "symbol: ONE_OWNER_OBJECTS names no built we_prog.o: the symbol half cannot read rule 7's imports"
  else
    "$NMTOOL" -u "$PROG_OBJ" 2>/dev/null | nm_undefined_names > "$WORK/prog-imports"
    for sym in e_opt_own_language_for e_opt_lang_value_from; do
      grep -qx "$sym" "$WORK/prog-imports" ||
        problem "symbol: $PROG_OBJ does not import $sym: Run and the build judge a language's value by a rule of their own"
    done
    ! grep -qx e_opt_own_language "$WORK/prog-imports" ||
      problem "symbol: $PROG_OBJ imports e_opt_own_language: the unswitched copy of a language is back"
  fi
fi

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
# The source half passed; with no nm the test is skipped, as the sibling
# scripts do, rather than reported as a pass that read no symbols.
[ "$SYMBOL_SKIPPED" -eq 1 ] && exit 77
exit 0
