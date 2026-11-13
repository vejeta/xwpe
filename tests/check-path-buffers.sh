#!/bin/sh
# check-path-buffers.sh -- how long a path the editor holds has one owner, and
# a dialog field's text has one way out.
#
# we_path.h holds the numbers (WPE_PATH_MAX, WPE_NAME_MAX) and we_path.c the
# writers that refuse instead of cutting; we_fieldtake.h and e_opt_field_take
# are the one way a field's text leaves its dialog.  A site that copies a
# name, a folder or a field's text into a buffer with strcpy, strcat or
# sprintf runs past the buffer when the input is longer than the author
# thought, and one that ignores a refusal hands an empty path to chdir,
# fopen or a command.
#
#   check-path-buffers.sh [--srcdir DIR]
#
# Six rules, each with canaries: the script plants an offender in a copy of
# the sources and fails when the rule does not name it, so a scanner gone
# blind fails instead of passing.  This is the memory-safety and the cuts part
# of the fence; the rules that count every path number and copy in the
# editor come with the pass that moves the debugger side.  The compiler holds
# the same line from the other end: warn_unused_result on the owner's
# declarations warns on a refusal ignored where the scan cannot see it, and
# probe_attribute below fails when that attribute is gone.
#
#  1. One definition.  WPE_PATH_MAX and WPE_NAME_MAX are each defined once,
#     in we_path.h, as a number.
#  2. A field's text leaves through the owner.  No strcpy, strcat, strncpy,
#     sprintf, memcpy or memmove takes a field's text (->txt, or a local
#     name it was assigned to) as its source, however the field is reached.
#  3. A refusal is read.  A call of wpe_path_copy, wpe_path_join,
#     wpe_path_cat, wpe_field_take or e_opt_field_take standing alone as a
#     statement, as the body of an if, else, while, for or do, or cast to
#     void, fails.
#  4. The size is asked of the caller.  Every call of e_add_arguments passes
#     the size of its buffer (sizeof, or a variable named size).
#  5. The overflows and the cuts stay gone.  The buffers and numbers the
#     first two passes removed appear in no top level source (comments are
#     not read, blanks inside brackets do not hide them).
#  6. A buffer that holds a path is sized by the owner.  The arrays the
#     passes moved (a picked name, a project path, a saved folder ...) are
#     declared with WPE_PATH_SIZE or WPE_NAME_SIZE, not a number.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-path-buffers: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

OWNER_HEADER=we_path.h

WORK=$(mktemp -d "${TMPDIR:-/tmp}/path-buffers.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# strip_comments FILE - the file's lines with comments blanked, string
# literals kept, one output line per input line.
strip_comments() {
  awk '
    BEGIN { incom = 0 }
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
      print out
    }' "$1"
}

# statements FILE REL - "rel|line|statement" for each statement of the
# (comment stripped) file: the text from the first token after a ';', '{' or
# '}' to the next one, a statement of several lines joined with blanks, the
# line number that of its first token.  A preprocessor line is its own
# statement.
statements() {
  awk -v rel="$2" '
    function emit() {
      if (stmt ~ /[^ \t]/) { s = stmt; sub(/^[ \t]+/, "", s); print rel "|" first "|" s }
      stmt = ""; first = 0
    }
    {
      line = $0
      if (line ~ /^[ \t]*#/) { emit(); s = line; sub(/^[ \t]+/, "", s); print rel "|" NR "|" s; next }
      n = length(line)
      for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (c == ";" || c == "{" || c == "}") { stmt = stmt c; emit(); continue }
        if (stmt == "" && c !~ /[ \t]/) first = NR
        if (stmt != "" || c !~ /[ \t]/) stmt = stmt c
      }
      if (stmt != "") stmt = stmt " "
    }
    END { emit() }' "$1"
}

# defines_of FILE REL - "rel|line|name|value" for each #define of the file.
defines_of() {
  awk -v rel="$2" '
    /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/ {
      s = $0; sub(/^[ \t]*#[ \t]*define[ \t]+/, "", s)
      name = s; sub(/[^A-Za-z0-9_].*$/, "", name)
      value = substr(s, length(name) + 1); sub(/^[ \t]+/, "", value)
      print rel "|" NR "|" name "|" value
    }' "$1"
}

# The awk functions the statement rules share.  A statement line is
# "rel|line|text" and the text may itself hold '|' (a "||"), so it is cut at
# the first two bars, never split on all of them.
AWK_LIB='
function cut_stmt(rec,    p, rest, q) {
  p = index(rec, "|"); REL = substr(rec, 1, p - 1); rest = substr(rec, p + 1)
  q = index(rest, "|"); LN = substr(rest, 1, q - 1); S = substr(rest, q + 1)
}
# arg_split(s, from) - the top level arguments of the call whose "(" comes
# just before position @from, in ARGS[1..n]; n, or 0 when the call is open.
function arg_split(s, from,    i, n, depth, c, cur, instr, q) {
  n = 0; depth = 1; cur = ""; instr = 0
  for (i = from; i <= length(s); i++) {
    c = substr(s, i, 1)
    if (instr) {
      cur = cur c
      if (c == "\\") { i++; cur = cur substr(s, i, 1) } else if (c == q) instr = 0
      continue
    }
    if (c == "\"" || c == "\047") { instr = 1; q = c; cur = cur c; continue }
    if (c == "(" || c == "[") depth++
    else if (c == ")" || c == "]") {
      depth--
      if (depth == 0) { ARGS[++n] = cur; return n }
    }
    if (c == "," && depth == 1) { ARGS[++n] = cur; cur = ""; continue }
    cur = cur c
  }
  return 0
}
# strip_heads(s) - @s without the "else", "do", "if (...)", "while (...)"
# and "for (...)" that only decide whether or how often the statement after
# them runs.
function strip_heads(s,    done, i, depth, c) {
  done = 0
  while (!done) {
    done = 1
    sub(/^[ \t]+/, "", s)
    if (s ~ /^(else|do)([ \t]|$)/) { sub(/^(else|do)[ \t]*/, "", s); done = 0; continue }
    if (s ~ /^(if|while|for)[ \t]*\(/) {
      depth = 0
      for (i = index(s, "("); i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "(") depth++
        else if (c == ")") { depth--; if (depth == 0) break }
      }
      if (depth == 0) { s = substr(s, i + 1); done = 0 }
    }
  }
  return s
}
# tight(l) - @l with the blanks inside and before brackets removed, so a
# declaration spelled "filen [ 128 ]" reads as "filen[128]".
function tight(l) {
  gsub(/[ \t]+\[/, "[", l); gsub(/\[[ \t]+/, "[", l); gsub(/[ \t]+\]/, "]", l)
  return l
}
'

# scan_tree DIR OUT - run the rules on the top level sources under DIR; the
# findings go to OUT, one per line, each starting with its rule.
scan_tree() {
  dir=$1; out=$2
  t=$(mktemp -d "$WORK/scan.XXXXXX")
  mkdir "$t/s"
  : > "$out"; : > "$t/defines"; : > "$t/stmts"
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    strip_comments "$f" > "$t/s/$rel"
    defines_of "$t/s/$rel" "$rel" >> "$t/defines"
    statements "$t/s/$rel" "$rel" >> "$t/stmts"
  done

  # Rule 1.
  for name in WPE_PATH_MAX WPE_NAME_MAX; do
    count=$(awk -F'|' -v n="$name" '$3 == n' "$t/defines" | wc -l)
    if [ "$count" -ne 1 ]; then
      echo "R1: $name is defined $count times, not once" >> "$out"
    fi
    awk -F'|' -v n="$name" -v owner="$OWNER_HEADER" '
      $3 == n && $1 != owner { print "R1: " n " is defined in " $1 ":" $2 ", not in " owner }
      $3 == n && $1 == owner && $4 !~ /^[0-9]+([ \t].*)?$/ { print "R1: " n " in " owner " is not a number: " $4 }' \
      "$t/defines" >> "$out"
  done
  [ -f "$dir/$OWNER_HEADER" ] ||
    echo "R1: $OWNER_HEADER is missing (the scan is blind or the owner is gone)" >> "$out"

  # Rule 2: a copy function with a field's text as its source.  A field's text
  # is "->txt", or a name a statement of the same file set to one.
  awk "$AWK_LIB"'
    BEGIN {
      fn = "(^|[^A-Za-z0-9_>.])(strcpy|strcat|strncpy|strncat|sprintf|memcpy|memmove|stpcpy)[ \t]*\\("
    }
    {
      if (index($0, "->txt") == 0 && index($0, "str") == 0 && index($0, "mem") == 0 && index($0, "sprintf") == 0 && index($0, "stpcpy") == 0)
        next
      cut_stmt($0)
      s = S
      gsub(/[ \t]*->[ \t]*/, "->", s)   # "w -> txt" is the same field
      if (REL != lastrel) { naliases = 0; lastrel = REL }
      # a name set from a field: "char *p = o->wstr[0]->txt;" or "p = w->txt;"
      if (s ~ /^[^;(]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=[^=].*->txt[^;]*;$/ && s !~ /^(if|while|for|return)[ \t(]/) {
        eq = index(s, "="); lhs = substr(s, 1, eq - 1); sub(/[ \t]+$/, "", lhs)
        if (match(lhs, /[A-Za-z_][A-Za-z0-9_]*$/)) alias[++naliases] = substr(lhs, RSTART)
      }
      rest = s
      while (match(rest, fn)) {
        start = RSTART + RLENGTH
        n = arg_split(rest, start)
        for (k = 2; k <= n; k++) {
          hit = (ARGS[k] ~ /->txt([^A-Za-z0-9_]|$)/)
          for (a = 1; !hit && a <= naliases; a++)
            hit = (ARGS[k] ~ ("(^|[^A-Za-z0-9_>.])" alias[a] "([^A-Za-z0-9_]|$)"))
          if (hit) {
            print "R2: " REL ":" LN ": a copy function reads a field\047s text; use e_opt_field_take"
            break
          }
        }
        rest = substr(rest, start)
      }
    }' "$t/stmts" >> "$out"

  # Rule 3: the result of a refusing writer is dropped.
  awk "$AWK_LIB"'
    BEGIN { names = "(wpe_path_copy|wpe_path_join|wpe_path_cat|wpe_field_take|e_opt_field_take)" }
    {
      cut_stmt($0)
      s = strip_heads(S)
      alone = "^" names "[ \t]*\\("
      voided = "^\\([ \t]*void[ \t]*\\)[ \t]*" names "[ \t]*\\("
      # the body of a for: the header is cut at its semicolons, so the
      # body follows the ")" that closes it, with no "(" before
      forbody = "^[^(]*\\)[ \t]*" names "[ \t]*\\("
      if (s ~ alone || s ~ forbody) print "R3: " REL ":" LN ": the result of a call that can refuse is not read"
      else if (s ~ voided) print "R3: " REL ":" LN ": the result of a call that can refuse is cast away"
    }' "$t/stmts" >> "$out"

  # Rule 4: a call of e_add_arguments gives its buffer's size.
  awk "$AWK_LIB"'
    {
      cut_stmt($0)
      s = S
      if (s ~ /^(static[ \t]+)?int[ \t]+e_add_arguments/) next
      if (s ~ /e_add_arguments[ \t]*\(/ && s !~ /e_add_arguments[ \t]*\([^,]*,[ \t]*(sizeof|size[ \t]*,)/)
        print "R4: " REL ":" LN ": e_add_arguments is called without the size of its buffer"
    }' "$t/stmts" >> "$out"

  # Rules 5 and 6, line by line.
  for f in "$t"/s/*.c "$t"/s/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    awk -v rel="$rel" "$AWK_LIB"'
      BEGIN {
        # the sizes a moved path buffer may be declared with
        okname = "(WPE_PATH_SIZE|WPE_NAME_SIZE|WPE_BUILD_PATH_MAX)"
        nmoved = split("filen prj_path cwd_saved saved_cwd xdgfile legacy libpath dirtmp exe", moved, " ")
      }
      {
        if (index($0, "[") == 0 && index($0, "MALLOC") == 0 && index($0, "WPE_PATHMAX") == 0 &&
            index($0, "strcpy") == 0 && index($0, "strncpy") == 0 && index($0, "E_X_SYSTEM") == 0)
          next
        l = tight($0)
        if (l ~ /(^|[^A-Za-z0-9_])filen\[128\]/)
          print "R5: " rel ":" NR ": a file name picked in the manager is kept in 128 bytes"
        if (l ~ /(^|[^A-Za-z0-9_])(search|replace)\[80\]/)
          print "R5: " rel ":" NR ": a Find text is kept in 80 bytes under a field of 128"
        if (l ~ /\(strcpy\)[ \t]*\(/)
          print "R5: " rel ":" NR ": a history entry is copied into a field without its capacity"
        if (l ~ /(^|[^A-Za-z0-9_])library\[80\]/)
          print "R5: " rel ":" NR ": the project library is kept in 80 bytes"
        if (l ~ /e_run_src_(dirct|datnam)\[/ || l ~ /strncpy\(e_run_src/)
          print "R5: " rel ":" NR ": the source Run keeps is held in a fixed array or cut"
        if (l ~ /MALLOC\(129\)/)
          print "R5: " rel ":" NR ": a name offered for Move is kept in 129 bytes"
        if (l ~ /(^|[^A-Za-z0-9_])WPE_PATHMAX([^A-Za-z0-9_]|$)/ || l ~ /E_X_SYSTEM_PATH_MAX/)
          print "R5: " rel ":" NR ": a private path number is back"
        if (l ~ /strncpy\(dirtmp/)
          print "R5: " rel ":" NR ": the Directory field is cut by strncpy"
        for (m = 1; m <= nmoved; m++) {
          if (match(l, "(^|[^A-Za-z0-9_])char[^;(=]*[^A-Za-z0-9_]" moved[m] "\\[[^]]*\\]")) {
            decl = substr(l, RSTART, RLENGTH)
            if (decl !~ okname)
              print "R6: " rel ":" NR ": " moved[m] " holds a path in a buffer the owner did not size"
          }
        }
      }' "$f" >> "$out"
  done
}

# canary RULE CODE - plant CODE in a tree of its own (the owner's header and
# one source), scan, and fail unless RULE names it.  The rules look at
# statements, never at the rest of the tree, so a small tree proves the same
# and keeps the scan fast.
canary() {
  rule=$1; code=$2
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  cp "$SRCDIR/$OWNER_HEADER" "$c"/ 2>/dev/null
  printf '%s\n' "$code" > "$c/we_planted.c"
  scan_tree "$c" "$c/out"
  if ! grep -q "^$rule:" "$c/out"; then
    echo "canary $rule: a planted offender was not found: $code"
    return 1
  fi
  return 0
}

# probe_attribute - the compiler is the other half of rule 3.  With
# warn_unused_result on the owner's declarations a refusal ignored anywhere
# is a diagnostic; the C library declares many functions the same way, so the
# build cannot make the warning an error (a distribution builds with
# _FORTIFY_SOURCE).  This proves the declarations still carry the attribute:
# a program that ignores a refusal must fail under -Werror=unused-result and
# the same program reading it must compile.  No compiler, or one that does
# not know the flag, skips the probe.
probe_attribute() {
  cc=${CC:-cc}
  command -v "$cc" >/dev/null 2>&1 || return 0
  good=$WORK/probe_good.c
  printf '#include <string.h>\n#include "we_path.h"\n#include "we_fieldtake.h"\nint planted(char *a) { return wpe_path_copy(a, 4, "x") + wpe_path_join(a, 4, "x", "y") + wpe_path_cat(a, 4, "x") + wpe_field_take("x", a, 4); }\n' > "$good"
  "$cc" -I"$SRCDIR" -c -o /dev/null -Werror=unused-result "$good" 2>/dev/null || return 0
  for call in 'wpe_path_copy(a, 4, "x")' 'wpe_path_join(a, 4, "x", "y")' \
              'wpe_path_cat(a, 4, "x")' 'wpe_field_take("x", a, 4)'; do
    bad=$WORK/probe_bad.c
    printf '#include <string.h>\n#include "we_path.h"\n#include "we_fieldtake.h"\nvoid planted(char *a, int c) { if (c) %s; }\n' "$call" > "$bad"
    if "$cc" -I"$SRCDIR" -c -o /dev/null -Werror=unused-result "$bad" 2>/dev/null; then
      echo "canary attribute: an ignored $call compiles; warn_unused_result is gone from we_path.h or we_fieldtake.h"
      return 1
    fi
  done
  return 0
}

blind=0
probe_attribute || blind=1
canary R1 '#define WPE_PATH_MAX 1024' || blind=1
canary R2 'void planted(char *buf, W_OPTSTR *o) { strcpy(buf, o->wstr[0]->txt); }' || blind=1
canary R2 'void planted(char *buf, W_O_WRSTR *w) { sprintf(buf, "%s", w->txt); }' || blind=1
canary R2 'void planted(char *buf, W_OPTSTR *o) { char *p = o->wstr[0]->txt; strcpy(buf, p); }' || blind=1
canary R2 'void planted(char *buf, W_OPTSTR *o) { memcpy(buf, o->wstr[1]->txt, 400); }' || blind=1
canary R2 'void planted(char *buf, W_O_WRSTR *w) { strncpy(buf, w->txt, 99); }' || blind=1
canary R2 'void planted(char *buf, W_O_WRSTR *w) { strcpy(buf, w -> txt); }' || blind=1
canary R3 'void planted(char *a, const char *b) { wpe_path_copy(a, 4, b); }' || blind=1
canary R3 'void planted(char *a, const char *b) { (void)wpe_path_join(a, 4, b, b); }' || blind=1
canary R3 'void planted(char *a, const char *b) { ( void ) wpe_path_cat(a, 4, b); }' || blind=1
canary R3 'void planted(char *a, const char *b, int c) { if (c) wpe_path_copy(a, 4, b); }' || blind=1
canary R3 'void planted(char *a, const char *b, int c) { if (c) b = 0; else wpe_path_copy(a, 4, b); }' || blind=1
canary R3 'void planted(char *a, const char *b, int c) { if (c > (1 + 2)) if (b) e_opt_field_take(0, 0, a, 4); }' || blind=1
canary R3 'void planted(char *a, const char *b, int i) { for (i = 0; i < 2; i++) wpe_path_copy(a, 4, b); }' || blind=1
canary R3 'void planted(char *a, const char *b) { do wpe_path_cat(a, 4, b); while (0); }' || blind=1
canary R4 'void planted(FENSTER *f) { char s[9]; e_add_arguments(s, "x", f, 0, 0, NULL); }' || blind=1
canary R5 'void planted(void) { char filen[128]; }' || blind=1
canary R5 'void planted(void) { char filen [ 128 ]; }' || blind=1
canary R5 'void planted(void) { char a, library[80]; }' || blind=1
canary R5 'void planted(char *s) { strncpy(e_run_src_dirct, s, 255); }' || blind=1
canary R5 'void planted(void) { char *p = MALLOC(129); }' || blind=1
canary R5 '#define WPE_PATHMAX 2048' || blind=1
canary R6 'void planted(void) { char prj_path[2048]; }' || blind=1
canary R6 'void planted(void) { char a, filen [ 256 ]; }' || blind=1
canary R6 'void planted(void) { char cwd_saved[WPE_PATH_SIZE / 4]; char saved_cwd[1024]; }' || blind=1

scan_tree "$SRCDIR" "$WORK/problems"
if [ -s "$WORK/problems" ]; then
  sort "$WORK/problems"
  echo "check-path-buffers: $(wc -l < "$WORK/problems") offender(s)"
  exit $BAD
fi
if [ "$blind" -ne 0 ]; then
  exit $BAD
fi
echo "check-path-buffers: clean"
exit 0
