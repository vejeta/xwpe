#!/bin/sh
# check-number-fields.sh -- a number the user or a file gives the editor has
# one owner.
#
# we_number.[ch] hold the type of each number (its range, its default, its
# key), the one parser that reads a text whole, the one writer of the five
# editor numbers (maxcol, tabn, maxchg, numundo, autoindent) and the one way
# a dialog field's digits become its value.  The editor divides by Tabstops,
# sizes every row with Max. Columns and reallocates with Auto Ind. Col., so a
# number outside its range is a crash or lost text.  A site that parses
# digits itself, writes one of the five numbers, or guards a value the owner
# already keeps in range, is one more rule for the same number.
#
#   check-number-fields.sh [--allow FILE] [--srcdir DIR] [--list] [--strict]
#
# Rules, on the top level sources with comments and literals blanked:
#
#  1. One parser.  The undefined symbols of the objects of we_e_aus.c,
#     we_hfkt.c, we_progn.c, we_block.c, we_main.c, WeString.c and we_opt.c
#     (nm -u, as linked: glibc links sscanf as __isoc99_sscanf and inlines
#     atoi into strtol) name none of atoi, atol, atoll, strtol, strtoul,
#     strtoll, strtoull, strtoimax, strtoumax, sscanf; only we_number.o may.
#     The source form is counted too, so a macro cannot hide one.  A file
#     with an allow line (the option file reader, until it moves onto the
#     owner) is counted in source form instead of by nm, and the count may
#     only fall.
#  2. One writer.  No source but we_number.c assigns, increments, compounds
#     or takes the address of the field maxcol, tabn, maxchg, numundo or
#     autoindent, whatever the pointer is called, and none but we_number.c
#     assigns the number of a number field; the field's accept loop of
#     we_opt.c assigns no ->num at all.
#  3. Every field has a type.  Every call of e_add_numstr and e_num_kst
#     passes a WPE_NUM( argument; WPE_NUM_TYPES is defined once, in
#     we_number.c, with one initialiser per kind; e_schreib_zif takes a
#     char *, never an int *.
#  4. No guard at a site.  A comparison of tabn, maxcol or autoindent with 0
#     or 1, or its negation, outside we_number.c, is counted per function;
#     the function that keeps a fallback for a buffer built without an
#     editor (buffer_tab_width) is the one allowed line.
#  5. The deleted symbol.  Nothing defines or calls WpeStringToNumber.
#  6. A refusal is read.  A call of wpe_number_parse, e_number_store or
#     e_number_field_accept standing alone as a statement, as the body of an
#     if, else or loop, or cast to void, fails; and the declarations carry
#     warn_unused_result.
#
# tests/number-fields.allow lists what the file reader still does by itself
# and the one keep line, each with its count and the pass that moves it onto
# the owner ("P2", "P4") or the reason it is not a number the owner keeps
# ("keep"); the counts may only fall: more fails, fewer fails until the line
# is lowered, a line that matches nothing fails as stale.  --strict ignores
# every line but the keep ones, which is the goal.  --list prints what was
# found as allow lines.
#
# Canaries: an offender planted for each rule in a tree of its own, and for
# rule 1 an atoi and a sscanf compiled and read with nm as linked; a rule
# that does not name it fails the script as blind.
#
# Environment (set by make check): NM, ONE_OWNER_OBJECTS, CC, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
ALLOW=
LIST=0
STRICT=0
BAD=1
NMTOOL=${NM:-nm}
OBJECTS=$ONE_OWNER_OBJECTS

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    --strict) STRICT=1 ;;
    *) echo "check-number-fields: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/number-fields.allow

OWNER=we_number.c
OWNER_HEADER=we_number.h
# The files whose object may not import a parser (rule 1).
PARSER_FILES="we_e_aus.c we_hfkt.c we_progn.c we_block.c we_main.c WeString.c we_opt.c"
PARSER_RE='(__isoc[0-9]+_)?(atoi|atol|atoll|strtol|strtoul|strtoll|strtoull|strtoimax|strtoumax|sscanf)'
FIELDS='maxcol|tabn|maxchg|numundo|autoindent'
GUARDED='tabn|maxcol|autoindent'
# Rule 2 as written against the field: an assignment, a compound assignment,
# an increment or decrement, an address handed on.
WRITE_RE="->($FIELDS)[ \t]*(([-+*/%|&^]|<<|>>)?=[^=]|[+][+]|--)|([+][+]|--)[ \t]*[A-Za-z_>.-]*->($FIELDS)|&[A-Za-z_>.-]*->($FIELDS)([^A-Za-z0-9_]|\$)"
GUARD_RE="(^|[^A-Za-z0-9_])($GUARDED)[ \t]*(>=|<=|==|!=|>|<)[ \t]*[01]([^0-9]|\$)|[^0-9][01][ \t]*[<>]=?[ \t]*[A-Za-z_>.-]*($GUARDED)([^A-Za-z0-9_]|\$)|![ \t]*[A-Za-z_>.-]*->($GUARDED)([^A-Za-z0-9_]|\$)"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/number-fields.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# strip_c FILE - the file's lines with comments, strings and characters
# blanked, one output line per input line.
strip_c() {
  awk -v sq="'" '
    BEGIN { incom = 0 }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          if (c == "\\") i += 2
          else { if (c == q) instr = 0; i++ }
          continue
        }
        if (d == "/*") { incom = 1; out = out " "; i += 2; continue }
        if (d == "//") break
        if (c == "\"" || c == sq) { instr = 1; q = c; out = out " "; i++; continue }
        out = out c; i++
      }
      print out
    }' "$1"
}

# statements FILE REL - "rel|line|statement" for each statement of the
# (stripped) file: the text from the first token after a ';', '{' or '}' to
# the next one, a statement of several lines joined with blanks, the line
# number that of its first token.  A preprocessor line is its own statement.
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

# count_by_function FILE REL REGEX - "rel|function|count" for each function
# whose lines match REGEX.  A function starts at a line in column 0 that
# names an identifier followed by "(" and does not start a preprocessor
# line; what comes before the first one is "(file scope)".
count_by_function() {
  awk -v rel="$2" -v re="$3" '
    BEGIN { fn = "(file scope)" }
    /^[A-Za-z_]/ && /[(]/ { fn = $0; sub(/[ \t]*[(].*/, "", fn); sub(/.*[ \t*]/, "", fn) }
    {
      s = $0
      while (match(s, re)) { cnt[fn]++; s = substr(s, RSTART + (RLENGTH > 0 ? RLENGTH : 1)) }
    }
    END { for (f in cnt) print rel "|" f "|" cnt[f] }' "$1"
}

# The awk functions the statement rules share (a statement line is
# "rel|line|text", and the text may hold '|', so it is cut at two bars).
AWK_LIB='
function cut_stmt(rec,    p, rest, q) {
  p = index(rec, "|"); REL = substr(rec, 1, p - 1); rest = substr(rec, p + 1)
  q = index(rest, "|"); LN = substr(rest, 1, q - 1); S = substr(rest, q + 1)
}
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
'

# scan_tree DIR OUT - run the source rules on the top level sources under
# DIR.  OUT gets one line per finding: "rule|file|function|count|text".
scan_tree() {
  dir=$1; out=$2
  t=$(mktemp -d "$WORK/scan.XXXXXX")
  mkdir "$t/s"
  : > "$out"; : > "$t/stmts"
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    strip_c "$f" > "$t/s/$rel"
    statements "$t/s/$rel" "$rel" >> "$t/stmts"
  done

  # Rule 1, source form: a parser called in one of the files that may not.
  for rel in $PARSER_FILES; do
    [ -f "$t/s/$rel" ] || continue
    count_by_function "$t/s/$rel" "$rel" \
      "(^|[^A-Za-z0-9_])(atoi|atol|atoll|strtol|strtoul|strtoll|strtoull|strtoimax|strtoumax|sscanf)[ \t]*[(]" |
      awk -F'|' '{ n += $3 } END { if (n) print "R1|" $1 "|(file)|" n "|a parser is called" }' >> "$out"
  done

  # Rule 2: a write of one of the five numbers, or of a number field's value.
  for f in "$t"/s/*.c "$t"/s/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    [ "$rel" = "$OWNER" ] && continue
    count_by_function "$f" "$rel" "$WRITE_RE" |
      awk -F'|' '{ print "R2|" $1 "|" $2 "|" $3 "|one of the five numbers is written outside the owner" }' >> "$out"
    count_by_function "$f" "$rel" 'nstr[[][^]]*[]]->num[ \t]*=[^=]' |
      awk -F'|' '{ print "R2|" $1 "|" $2 "|" $3 "|a number field is given its value outside the owner" }' >> "$out"
    if [ "$rel" = we_opt.c ]; then
      count_by_function "$f" "$rel" '->num[ \t]*=[^=]' |
        awk -F'|' '$2 ~ /^e_opt_(run_num_field|num_field_edit|num_field_resize_take)$/ { print "R2|" $1 "|" $2 "|" $3 "|the accept loop of a number field assigns a value" }' >> "$out"
    fi
  done

  # Rule 3: every field has a type.
  awk "$AWK_LIB"'
    {
      cut_stmt($0)
      s = S
      if (s ~ /^(static[ \t]+)?(int[ \t]+|W_O_NUMSTR[ \t]*[*][*][ \t]*)(e_add_numstr|e_num_kst)[ \t]*[(]/) next
      # a WPE_NUM( argument, or the dialog helper handing on its own typed parameter
      if (s ~ /(^|[^A-Za-z0-9_])(e_add_numstr|e_num_kst)[ \t]*[(]/ && s !~ /WPE_NUM[ \t]*[(]/ && s !~ /,[ \t]*type[ \t]*[,)]/)
        print "R3|" REL "|" LN "|1|a number field is added without a WPE_NUM( type"
      if (s ~ /(^|[^A-Za-z0-9_])e_schreib_zif[ \t]*[(][ \t]*int[ \t]*[*]/)
        print "R3|" REL "|" LN "|1|e_schreib_zif takes an int *, not the field text"
    }' "$t/stmts" >> "$out"
  defs=$(cat "$t"/s/*.c "$t"/s/*.h 2>/dev/null | grep -c 'WPE_NUM_TYPES[ 	]*[[][^]]*[]][ 	]*=')
  if [ -f "$t/s/$OWNER" ] && [ "$defs" -ne 1 ]; then
    echo "R3|$OWNER|(file)|1|WPE_NUM_TYPES is defined $defs times, not once" >> "$out"
  fi
  if [ -f "$t/s/$OWNER" ] && [ -f "$t/s/$OWNER_HEADER" ]; then
    kinds=$(awk '/typedef enum/ { on = 1 } on { print } /WpeNumberKind/ { on = 0 }' "$t/s/$OWNER_HEADER" |
            grep -o 'WPE_NUM_[A-Z]*' | grep -vc 'WPE_NUM_KINDS')
    inits=$(awk '/WPE_NUM_TYPES[ \t]*\[/ { on = 1; next } on && /^[ \t]*[}][ \t]*;/ { on = 0 } on { print }' "$t/s/$OWNER" |
            grep -c '^[ \t]*[{][ \t]*WPE_NUM_[A-Z]*[ \t]*,')
    if [ "$kinds" -ne "$inits" ]; then
      echo "R3|$OWNER|(file)|1|WPE_NUM_TYPES has $inits initialisers for $kinds kinds" >> "$out"
    fi
  fi

  # Rule 4: a guard at a site, counted per function.
  for f in "$t"/s/*.c "$t"/s/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    [ "$rel" = "$OWNER" ] && continue
    count_by_function "$f" "$rel" "$GUARD_RE" |
      awk -F'|' '{ print "R4|" $1 "|" $2 "|" $3 "|a value the owner keeps in range is guarded at a site" }' >> "$out"
  done

  # Rule 5: the deleted symbol.
  for f in "$t"/s/*.c "$t"/s/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    count_by_function "$f" "$rel" "(^|[^A-Za-z0-9_])WpeStringToNumber([^A-Za-z0-9_]|\$)" |
      awk -F'|' '{ print "R5|" $1 "|" $2 "|" $3 "|WpeStringToNumber is back" }' >> "$out"
  done

  # Rule 6: the result of a refusing call is dropped.
  awk "$AWK_LIB"'
    BEGIN { names = "(wpe_number_parse|e_number_store|e_number_field_accept)" }
    {
      cut_stmt($0)
      s = strip_heads(S)
      alone = "^" names "[ \t]*\\("
      voided = "^\\([ \t]*void[ \t]*\\)[ \t]*" names "[ \t]*\\("
      forbody = "^[^(]*\\)[ \t]*" names "[ \t]*\\("
      if (s ~ /^(static[ \t]+)?(int|void)[ \t]/) next
      if (s ~ alone || s ~ forbody) print "R6|" REL "|" LN "|1|the result of a call that can refuse is not read"
      else if (s ~ voided) print "R6|" REL "|" LN "|1|the result of a call that can refuse is cast away"
    }' "$t/stmts" >> "$out"
}

# canary RULE CODE [FILE] - plant CODE in a tree of its own, as FILE (default
# we_planted.c), with the owner's two files, scan, and fail unless RULE names
# it.
canary() {
  rule=$1; code=$2; name=${3:-we_planted.c}
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  cp "$SRCDIR/$OWNER" "$SRCDIR/$OWNER_HEADER" "$c"/ 2>/dev/null
  printf '%s\n' "$code" > "$c/$name"
  scan_tree "$c" "$c/out"
  if ! grep -q "^$rule|" "$c/out"; then
    echo "canary $rule: a planted offender was not found: $code"
    return 1
  fi
  return 0
}

# canary_fine CODE - code that is fine must name nothing.
canary_fine() {
  c=$(mktemp -d "$WORK/fine.XXXXXX")
  cp "$SRCDIR/$OWNER" "$SRCDIR/$OWNER_HEADER" "$c"/ 2>/dev/null
  printf '%s\n' "$1" > "$c/we_planted.c"
  scan_tree "$c" "$c/out"
  if [ -s "$c/out" ]; then
    echo "canary: code that is fine was named: $1"
    sed 's/^/  /' "$c/out"
    return 1
  fi
  return 0
}

# parser_linked CODE - the undefined symbols of CODE compiled, one per line.
parser_linked() {
  cc=${CC:-cc}
  command -v "$cc" >/dev/null 2>&1 || return 1
  printf '%s\n' "$1" > "$WORK/linked.c"
  "$cc" -O2 -c -o "$WORK/linked.o" "$WORK/linked.c" 2>/dev/null || return 1
  "$NMTOOL" -u "$WORK/linked.o" 2>/dev/null | awk 'NF >= 2 && $(NF-1) == "U" { n = $NF; sub(/@.*/, "", n); print n }'
}

# probe_attribute - the compiler is the other half of rule 6: a program that
# ignores the parser's answer must fail under -Werror=unused-result and the
# same program reading it must compile.  The other two calls are declared in
# edit.h, which needs the editor's configuration to compile; they are read
# for the same macro.
probe_attribute() {
  cc=${CC:-cc}
  command -v "$cc" >/dev/null 2>&1 || return 0
  printf '#include "we_number.h"\nint planted(const char *t) { int v; return wpe_number_parse(t, WPE_NUM(WPE_NUM_TAB), &v); }\n' > "$WORK/probe_good.c"
  "$cc" -I"$SRCDIR" -c -o /dev/null -Werror=unused-result "$WORK/probe_good.c" 2>/dev/null || return 0
  printf '#include "we_number.h"\nvoid planted(const char *t, int c) { int v; if (c) wpe_number_parse(t, WPE_NUM(WPE_NUM_TAB), &v); }\n' > "$WORK/probe_bad.c"
  if "$cc" -I"$SRCDIR" -c -o /dev/null -Werror=unused-result "$WORK/probe_bad.c" 2>/dev/null; then
    echo "canary attribute: an ignored wpe_number_parse compiles; warn_unused_result is gone from we_number.h"
    return 1
  fi
  for call in e_number_store e_number_field_accept; do
    if ! grep -E "(^|[^A-Za-z0-9_])$call[ \t]*[(][^;]*WPE_NUMBER_RESULT[ \t]*;" "$SRCDIR/edit.h" >/dev/null; then
      echo "canary attribute: $call is declared in edit.h without WPE_NUMBER_RESULT"
      return 1
    fi
  done
  return 0
}

# object_of FILE - the object of a source among OBJECTS, by its name.
object_of() {
  base=${1%.c}.o
  for o in $OBJECTS; do
    case $(basename "$o") in
      "$base"|*-"$base") [ -f "$o" ] && { echo "$o"; return 0; } ;;
    esac
  done
  return 1
}

# nm_names OBJ - the undefined symbols of an object that are a parser.
nm_names() {
  "$NMTOOL" -u "$1" 2>/dev/null | awk -v re="^$PARSER_RE\$" '
    NF >= 2 && $(NF-1) == "U" { n = $NF; sub(/@.*/, "", n); if (n ~ re) print n }'
}

# --- the scan of the tree
scan_tree "$SRCDIR" "$WORK/found"

# Rule 1, as linked, for the objects of the editor that are given.  A file
# with an allow line is exempt by name here: its remaining parsers are
# counted in source form above.
nm_checked=0
if [ -n "$OBJECTS" ] && command -v "$NMTOOL" >/dev/null 2>&1 && [ "$NMTOOL" != ":" ]; then
  for rel in $PARSER_FILES; do
    obj=$(object_of "$rel") || continue
    nm_checked=$((nm_checked + 1))
    if [ "$STRICT" -eq 0 ] && grep -q "^R1|$rel|" "$ALLOW" 2>/dev/null; then
      continue
    fi
    for sym in $(nm_names "$obj"); do
      echo "R1|$rel|(linked)|1|the object imports $sym" >> "$WORK/found"
    done
  done
  # Rule 5, as linked: no object defines the deleted symbol; the owner's
  # symbols are seen, so a scanner that sees nothing fails.
  for o in $OBJECTS; do
    [ -f "$o" ] || continue
    if "$NMTOOL" -g --defined-only "$o" 2>/dev/null | awk '$NF == "WpeStringToNumber" { f = 1 } END { exit !f }'; then
      echo "R5|$(basename "$o")|(linked)|1|the object defines WpeStringToNumber" >> "$WORK/found"
    fi
  done
fi

# --- the allow file
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

# allow lines: rule|file|function|count|pass|reason
if [ -f "$ALLOW" ]; then
  grep -v '^[ \t]*#' "$ALLOW" | grep -v '^[ \t]*$' > "$WORK/allow" || true
else
  : > "$WORK/allow"
fi
if [ "$STRICT" -eq 1 ]; then
  awk -F'|' '$5 == "keep"' "$WORK/allow" > "$WORK/allow.strict"
  mv "$WORK/allow.strict" "$WORK/allow"
fi

if [ "$LIST" -eq 1 ]; then
  awk -F'|' '$1 ~ /^R[0-9]$/ && $4 + 0 > 0 { print $1 "|" $2 "|" $3 "|" $4 "|P?|" $5 }' "$WORK/found" | sort
  exit 0
fi

# findings are summed per rule, file and function (the linked ones are
# informational lines of their own)
awk -F'|' '{ k = $1 "|" $2 "|" $3; n[k] += $4; why[k] = $5 } END { for (k in n) print k "|" n[k] "|" why[k] }' \
  "$WORK/found" | sort > "$WORK/sum"

while IFS='|' read -r rule file fn count why; do
  allowed=$(awk -F'|' -v r="$rule" -v f="$file" -v fn="$fn" '$1 == r && $2 == f && $3 == fn { print $4 }' "$WORK/allow")
  if [ -z "$allowed" ]; then
    problem "$rule $file $fn: $count ($why)"
  elif [ "$count" -gt "$allowed" ]; then
    problem "$rule $file $fn: $count, the allow file says $allowed (more than allowed)"
  elif [ "$count" -lt "$allowed" ]; then
    problem "$rule $file $fn: $count, the allow file says $allowed (lower the line)"
  fi
done < "$WORK/sum"

# an allow line that matches nothing is stale
while IFS='|' read -r rule file fn count pass reason; do
  [ -n "$rule" ] || continue
  grep -q "^$rule|$file|$fn|" "$WORK/sum" ||
    problem "stale allow line: $rule $file $fn matches nothing"
done < "$WORK/allow"

# --- the canaries: a scanner that sees nothing must fail, not pass
blind=0
probe_attribute || blind=1

# rule 1, source form and as linked
canary R1 'int planted(const char *s) { return atoi(s); }' we_e_aus.c || blind=1
canary R1 'int planted(const char *s) { int v = 0; sscanf(s, "%d", &v); return v; }' we_hfkt.c || blind=1
canary R1 'long planted(const char *s) { return strtol(s, 0, 10); }' WeString.c || blind=1
# compiled and read as linked: a scanner that only knew the source names would
# be blind to these, which is what glibc makes of atoi and sscanf
if linked=$(parser_linked '#include <stdio.h>
#include <stdlib.h>
int planted(const char *s) { int v = 0; sscanf(s, "%d", &v); return atoi(s) + v; }'); then
  seen=$(printf '%s\n' "$linked" | awk -v re="^$PARSER_RE\$" '$0 ~ re { n++ } END { print n + 0 }')
  [ "$seen" -ge 2 ] || { echo "canary R1: a compiled atoi and sscanf show $seen parser names as linked, not 2 ($(echo $linked))"; blind=1; }
else
  echo "check-number-fields: no compiler, the compiled canaries are skipped" >&2
fi
if [ "$nm_checked" -gt 0 ]; then
  n=$(nm_names "$(object_of we_number.c 2>/dev/null)" 2>/dev/null | wc -l)
  [ "$n" -ge 1 ] || { echo "canary R1: the owner's object imports no parser as nm sees it (LTO objects or a wrong nm)"; blind=1; }
fi

# rule 2
canary R2 'void planted(ECNT *e) { e->tabn = 0; }' || blind=1
canary R2 'void planted(FENSTER *f) { f->ed->maxcol -= 1; }' || blind=1
canary R2 'void planted(BUFFER *b) { b->cn->maxcol += 1; }' || blind=1
canary R2 'void planted(ECNT *cn) { cn->autoindent++; }' || blind=1
canary R2 'void planted(ECNT *cn) { --cn->numundo; }' || blind=1
canary R2 'void planted(ECNT *cn, char *v) { sscanf(v, "%d", &cn->tabn); }' || blind=1
canary R2 'void planted(ECNT *cn) { cn->maxchg <<= 1; }' || blind=1
canary R2 'void planted(W_OPTSTR *o) { o->nstr[0]->num = 3; }' || blind=1
canary R2 'static void e_opt_run_num_field(W_OPTSTR *o)
{
 W_O_NUMSTR *n = o->nstr[0];
 n->num = 3;
}' we_opt.c || blind=1
canary_fine 'int planted(ECNT *cn, EShift shift) { shift.tabn = cn->tabn; return cn->tabn == cn->maxcol || cn->tabn <= cn->maxchg; }' || blind=1

# rule 3
canary R3 'void planted(W_OPTSTR *o) { e_add_numstr(3, 9, 20, 9, 2, 100, 0, AltT, "x", 4, o); }' || blind=1
canary R3 'void planted(FENSTER *f) { e_num_kst("Goto", 1, 100, f, 0, AltG); }' || blind=1
canary R3 'int e_schreib_zif(int *num, int x, int y, int max, int ft, int fs) { return 0; }' || blind=1
canary_fine 'void planted(W_OPTSTR *o) { e_add_numstr(3, 9, 20, 9,
   WPE_NUM(WPE_NUM_TAB), 0, AltT, "x", 4, o); }' || blind=1

# rule 4
canary R4 'int planted(FENSTER *f) { if (f->ed->tabn > 0) return 1; return 0; }' || blind=1
canary R4 'int planted(BUFFER *b) { if (!b->cn->tabn) return 1; return 0; }' || blind=1
canary R4 'int planted(int tabn) { if (0 < tabn) return 1; return 0; }' || blind=1
canary R4 'int planted(ECNT *cn) { return cn->maxcol >= 1; }' || blind=1
canary R4 'int planted(ECNT *cn) { if (cn->tabn < 1) return 1; return 0; }' || blind=1
canary_fine 'int planted(ECNT *cn, int x) { return x / cn->tabn + cn->maxcol; }' || blind=1

# rule 5
canary R5 'int planted(const char *s) { return WpeStringToNumber(s); }' || blind=1

# rule 6
canary R6 'void planted(const char *t, const WpeNumberType *ty) { wpe_number_parse(t, ty, 0); }' || blind=1
canary R6 'void planted(ECNT *cn) { (void)e_number_store(cn, WPE_NUM(WPE_NUM_TAB), 3); }' || blind=1
canary R6 'void planted(ECNT *cn, int c) { if (c) e_number_store(cn, WPE_NUM(WPE_NUM_TAB), 3); }' || blind=1
canary R6 'void planted(W_O_NUMSTR *n, char *t, int c) { if (c) c = 0; else e_number_field_accept(n, t, 0); }' || blind=1
canary R6 'void planted(W_O_NUMSTR *n, char *t, int i) { for (i = 0; i < 2; i++) e_number_field_accept(n, t, 0); }' || blind=1
canary_fine 'int planted(ECNT *cn) { if (e_number_store(cn, WPE_NUM(WPE_NUM_TAB), 3) != 0) return -1; return 0; }' || blind=1

if [ -s "$WORK/problems" ]; then
  sort "$WORK/problems"
  echo "check-number-fields: $(wc -l < "$WORK/problems") offender(s)"
  exit $BAD
fi
if [ "$blind" -ne 0 ]; then
  exit $BAD
fi
if [ "$nm_checked" -eq 0 ]; then
  echo "check-number-fields: clean (source rules only; no objects to read with nm)"
else
  echo "check-number-fields: clean"
fi
exit 0
