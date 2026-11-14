#!/bin/sh
# check-help-buffers.sh -- a help or Info token reaches fixed storage through
# one owner, and a row of a buffer is written through the buffer's owner.
#
# we_helpbuf.h holds the writers that cut at a character boundary and refuse
# a markup byte that does not fit; we_buffer.c holds the writers of a row.
# The Help viewer and the Info reader once copied a link name, a heading, a
# node name or a menu entry into an array with a delimiter-scan loop, a
# strcpy, a strcat or a sprintf, grew a line in place by shifting its tail,
# and wrote a whole line into a row sized for a narrower one.  Each of those
# runs past the array when the text is longer than the author thought, and
# Info files come from the system and the user.
#
#   check-help-buffers.sh [--srcdir DIR]
#
# Four rules over every top level source except we_helpbuf.c (the owner),
# each with a canary: the script plants an offender in a tree of its own and
# fails when the rule does not name it, so a scanner gone blind fails instead
# of passing.  The negative canaries plant the safe spellings and fail when a
# rule names them.
#
#  1. No strcpy, strcat, sprintf or vsprintf whose destination is a char
#     array declared in the same function.  (A nested strcat(strcpy(d, a), b)
#     is read call by call, innermost first.)
#  2. No assignment NAME[EXPR] = SRC[EXPR] or = *(...) into such an array in
#     a loop head or a loop body, unless the statement or the loop head
#     names sizeof NAME.  A for head is one unit: the statement splitter
#     does not cut inside parentheses.
#  3. No in-place shift X[EXPR+k] = X[EXPR] of any array or pointer: that is
#     how a line is grown, and the owner's insert refuses when it is full.
#  4. No strcpy, strcat, sprintf, strncpy or memcpy into the bytes of a
#     buffer row (bf[...].s) outside we_buffer.c, which owns them.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-help-buffers: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

OWNER_SOURCE=we_helpbuf.c
ROW_OWNER_SOURCE=we_buffer.c

WORK=$(mktemp -d "${TMPDIR:-/tmp}/help-buffers.XXXXXX") || exit $BAD
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

# scan_file FILE REL ROWRULE - the findings of the four rules in one
# comment stripped file, "RULE: rel:line: function: what" each.  ROWRULE is 0
# for the source that owns the bytes of a buffer row, where rule 4 is off.
#
# The file is cut into units at ';', '{' and '}' that stand outside
# parentheses, brackets and string literals, so a for head with its two
# semicolons is one unit with its body.  The scan keeps the brace depth, the
# function it is in, the char arrays that function declares, and the loop
# heads that enclose the unit.
scan_file() {
  awk -v rel="$2" -v rowrule="$3" '
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    function squeeze(s) { gsub(/[ \t]+/, "", s); return s }
    # arg1_of(s, from) - the first argument of the call whose "(" comes just
    # before position from, with its leading blanks, "&" and "(" removed.
    function arg1_of(s, from,    i, depth, c, cur) {
      depth = 1; cur = ""
      for (i = from; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "(" || c == "[") depth++
        else if (c == ")" || c == "]") { depth--; if (depth == 0) break }
        if (c == "," && depth == 1) break
        cur = cur c
      }
      return cur
    }
    function lead_ident(s) {
      sub(/^[ \t&(]+/, "", s)
      if (match(s, /^[A-Za-z_][A-Za-z0-9_]*/)) return substr(s, 1, RLENGTH)
      return ""
    }
    function report(rule, what) {
      print rule ": " rel ":" unitline ": " (fname == "" ? "-" : fname) ": " what
    }
    # note_arrays(u) - remember the char arrays a declaration unit declares.
    function note_arrays(u,    body, i, depth, c, cur, n, parts, k, d) {
      body = u
      sub(/^((static|const|unsigned|signed|register)[ \t]+)*char[ \t]+/, "", body)
      sub(/;[ \t]*$/, "", body)
      depth = 0; cur = ""; n = 0
      for (i = 1; i <= length(body); i++) {
        c = substr(body, i, 1)
        if (c == "(" || c == "[" || c == "{") depth++
        else if (c == ")" || c == "]" || c == "}") depth--
        if (c == "," && depth == 0) { parts[++n] = cur; cur = ""; continue }
        cur = cur c
      }
      parts[++n] = cur
      for (k = 1; k <= n; k++) {
        d = trim(parts[k]); sub(/=.*$/, "", d); d = trim(d)
        if (d ~ /^[A-Za-z_][A-Za-z0-9_]*[ \t]*\[/) {
          sub(/[ \t]*\[.*$/, "", d); loc[d] = 1
        }
      }
    }
    function is_decl(u) {
      return (u ~ /^((static|const|unsigned|signed|register)[ \t]+)*char[ \t]/ && u !~ /^[^=]*\(/)
    }
    function unit_in_loop(u,    d) {
      if (u ~ /^(for|while|do)([ \t(]|$|\{)/) return 1
      for (d = 1; d <= depth; d++) if (heads[d] != "") return 1
      return 0
    }
    function bound_text(u,    d, t) {
      t = u
      for (d = 1; d <= depth; d++) t = t " " heads[d]
      return t
    }
    function check_dest_calls(u,    rest, start, a, id, fn) {
      fn = "(^|[^A-Za-z0-9_>.])(strcpy|strcat|sprintf|vsprintf)[ \t]*\\("
      rest = u
      while (match(rest, fn)) {
        start = RSTART + RLENGTH
        a = arg1_of(rest, start)
        id = lead_ident(a)
        if (id != "" && (id in loc))
          report("R1", "a copy function writes the array " id " without its size")
        rest = substr(rest, start)
      }
    }
    function check_scan_loops(u,    nm, re, rest, after, bounded) {
      if (!unit_in_loop(u)) return
      bounded = bound_text(u)
      for (nm in loc) {
        re = "(^|[^A-Za-z0-9_>.])" nm "[ \t]*\\[[^]]*\\][ \t]*="
        rest = u
        while (match(rest, re)) {
          after = substr(rest, RSTART + RLENGTH)
          rest = after
          if (after ~ /^=/) continue
          sub(/^[ \t]+/, "", after)
          if (after !~ /^(\*|[A-Za-z_][A-Za-z0-9_.>-]*[ \t]*\[)/) continue
          if (bounded ~ ("sizeof[ \t(]*" nm "([^A-Za-z0-9_]|$)")) continue
          report("R2", "the array " nm " takes a scanned copy not bounded by its size")
        }
      }
    }
    function check_shifts(u,    rest, name, lidx, after, ridx, p, m) {
      rest = u
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\[[^]]*\][ \t]*=/)) {
        m = substr(rest, RSTART, RLENGTH)
        after = substr(rest, RSTART + RLENGTH)
        rest = after
        if (after ~ /^=/) continue
        name = m; sub(/[ \t]*\[.*$/, "", name)
        lidx = m; sub(/^[^[]*\[/, "", lidx); sub(/\][ \t]*=$/, "", lidx)
        sub(/^[ \t]+/, "", after)
        if (after !~ ("^" name "[ \t]*\\[")) continue
        p = after; sub(/^[A-Za-z_][A-Za-z0-9_]*[ \t]*\[/, "", p)
        ridx = p; sub(/\].*$/, "", ridx)
        lidx = squeeze(lidx); ridx = squeeze(ridx)
        if (ridx != "" && lidx != ridx && index(lidx, ridx "+") == 1 &&
            substr(lidx, length(ridx) + 2) ~ /^[0-9]+$/)
          report("R3", "the line " name " is grown in place by shifting its tail")
      }
    }
    function check_row_writes(u,    rest, start, a) {
      if (rowrule == 0) return
      rest = u
      while (match(rest, /(^|[^A-Za-z0-9_>.])(strcpy|strcat|sprintf|strncpy|memcpy)[ \t]*\(/)) {
        start = RSTART + RLENGTH
        a = arg1_of(rest, start)
        if (a ~ /bf[ \t]*\[[^]]*\][ \t]*\.[ \t]*s([^A-Za-z0-9_]|$)/)
          report("R4", "a buffer row is written with a copy function, not through the buffer owner")
        rest = substr(rest, start)
      }
    }
    function check_unit(u) {
      if (depth >= 1 && is_decl(u)) note_arrays(u)
      check_dest_calls(u)
      check_scan_loops(u)
      check_shifts(u)
      check_row_writes(u)
    }
    # note_function(u) - a unit ending in "{" at depth 0 starts a function:
    # its name is the identifier before the first "(" of this unit or, for a
    # K&R definition whose "{" stands alone, of the unit that held it.
    function note_function(u,    t) {
      if (u ~ /^[A-Za-z_][^;]*\(/) {
        t = u; sub(/\(.*$/, "", t); sub(/[ \t]+$/, "", t)
        if (match(t, /[A-Za-z_][A-Za-z0-9_]*$/)) cand = substr(t, RSTART)
      }
      fname = cand
      delete loc
    }
    function unit(u, term, ln,    t) {
      u = trim(u)
      unitline = ln
      if (term == "}") {
        if (u != "") check_unit(u)
        heads[depth] = ""
        depth--
        if (depth <= 0) { depth = 0; fname = ""; delete loc }
        return
      }
      t = u term
      if (depth == 0) {
        if (term == "{") note_function(t)
        else if (t ~ /^[A-Za-z_][^;]*\(/) {
          cand = t; sub(/\(.*$/, "", cand); sub(/[ \t]+$/, "", cand)
          if (match(cand, /[A-Za-z_][A-Za-z0-9_]*$/)) cand = substr(cand, RSTART); else cand = ""
        }
      }
      if (term == "{") {
        depth++
        heads[depth] = (t ~ /^(for|while|do)([ \t(]|\{)/) ? t : ""
        if (t ~ /^(for|while|do)([ \t(]|\{)/) check_unit(t)
        else if (depth > 1) check_unit(t)
        return
      }
      check_unit(t)
    }
    BEGIN { depth = 0; paren = 0; cur = ""; first = 0; cont = 0; fname = ""; cand = ""; pp = 0 }
    {
      line = $0
      if (cont) { cont = (line ~ /\\[ \t]*$/); next }
      # Each branch of #if/#else starts at the depth the #if had: an "if
      # (...) {" written once per branch would otherwise count twice and
      # leave the rest of the file one level deep, under the wrong function.
      if (line ~ /^[ \t]*#/) {
        if (line ~ /^[ \t]*#[ \t]*if/) ppdepth[++pp] = depth
        else if (line ~ /^[ \t]*#[ \t]*(else|elif)/) { if (pp > 0) depth = ppdepth[pp] }
        else if (line ~ /^[ \t]*#[ \t]*endif/) { if (pp > 0) pp-- }
        cont = (line ~ /\\[ \t]*$/); next
      }
      n = length(line); instr = 0; inchr = 0
      for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (cur == "" && c ~ /[ \t]/) continue
        if (cur == "") first = NR
        if (instr || inchr) {
          cur = cur c
          if (c == "\\") { cur = cur substr(line, i + 1, 1); i++; continue }
          if (instr && c == "\"") instr = 0
          if (inchr && c == "\047") inchr = 0
          continue
        }
        if (c == "\"") { instr = 1; cur = cur c; continue }
        if (c == "\047") { inchr = 1; cur = cur c; continue }
        if (c == "(" || c == "[") paren++
        else if (c == ")" || c == "]") { if (paren > 0) paren-- }
        if (paren == 0 && c == ";") { unit(cur, ";", first); cur = ""; continue }
        if (paren == 0 && c == "{") { unit(cur, "{", first); cur = ""; continue }
        if (paren == 0 && c == "}") { unit(cur, "}", first); cur = ""; continue }
        cur = cur c
      }
      if (cur != "") cur = cur " "
    }' "$1"
}

# scan_tree DIR OUT - run the rules on the top level sources under DIR; the
# findings go to OUT, one per line, each starting with its rule.
scan_tree() {
  dir=$1; out=$2
  t=$(mktemp -d "$WORK/scan.XXXXXX")
  : > "$out"
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    [ "$rel" = "$OWNER_SOURCE" ] && continue
    strip_comments "$f" > "$t/s.txt"
    rowrule=1
    [ "$rel" = "$ROW_OWNER_SOURCE" ] && rowrule=0
    scan_file "$t/s.txt" "$rel" "$rowrule" >> "$out"
  done
}

# canary RULE CODE - plant CODE in a tree of its own, scan, and fail unless
# RULE names it.
canary() {
  rule=$1; code=$2
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$code" > "$c/we_planted.c"
  scan_tree "$c" "$c/out"
  if ! grep -q "^$rule:" "$c/out"; then
    echo "canary $rule: a planted offender was not found: $code"
    return 1
  fi
  return 0
}

# safe_canary CODE - plant the safe spelling CODE and fail when any rule
# names it.
safe_canary() {
  code=$1
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$code" > "$c/we_planted.c"
  scan_tree "$c" "$c/out"
  if [ -s "$c/out" ]; then
    echo "safe canary: a bounded spelling was named: $code"
    sed -n 's/^/  /p' "$c/out"
    return 1
  fi
  return 0
}

blind=0
canary R1 'void planted(const char *s) { char buf[8]; strcpy(buf, s); }' || blind=1
canary R1 'void planted(const char *s) { char a, buf [ 8 ]; sprintf(buf, "%s", s); }' || blind=1
canary R1 'void planted(const char *s) { char buf[8]; strcat(strcpy(buf, "x"), s); }' || blind=1
canary R2 'void planted(const char *p) { char buf[8]; int i; for (i = 0; (buf[i] = p[i]) && p[i] != 58; i++) ; }' || blind=1
canary R2 'void planted(const char *p) { char buf[8]; int i; for (i = 0; p[i]; i++) { buf[i] = p[i]; } }' || blind=1
canary R2 'void planted(const char *p) { char buf[8]; int i = 0; while (*p) buf[i++] = *p++; }' || blind=1
canary R3 'void planted(char *str, int len) { int i; for (i = len; i >= 0; i--) str[i+1] = str[i]; }' || blind=1
canary R3 'void planted(void) { char line[8]; int i; for (i = 3; i >= 0; i--) line[i + 2] = line[i]; }' || blind=1
canary R4 'void planted(BUFFER *b, const char *s) { strcpy(b->bf[0].s, s); }' || blind=1
canary R4 'void planted(BUFFER *b, const char *s) { memcpy(b -> bf[ 2 ] . s, s, 9); }' || blind=1
safe_canary 'void planted(const char *s) { char local[8]; snprintf(local, sizeof local, "%s", s); }' || blind=1
safe_canary 'void planted(const char *s) { char *p = MALLOC(strlen(s) + 1); strcpy(p, s); }' || blind=1
safe_canary 'void planted(const char *p) { char buf[8]; int i; for (i = 0; i < sizeof(buf) - 1 && (buf[i] = p[i]); i++) ; }' || blind=1
safe_canary 'void planted(const char *p) { char buf[8]; int i; for (i = 0; i < (int) sizeof buf - 1; i++) { buf[i] = p[i]; } }' || blind=1
safe_canary 'void planted(const char *p, char *q) { char buf[8]; char *e; for (e = buf; e < buf + sizeof(buf) - 1 && *p; ) *e++ = *p++; q[0] = buf[0]; }' || blind=1
safe_canary 'void planted(int a) { char buf[8];
#ifdef PLANTED
 if (a) {
#else
 if (!a) {
#endif
  buf[0] = 0; }
}
void taker(char *buf, const char *p, int n) { int i; for (i = 0; i < n - 1 && p[i]; i++) buf[i] = p[i]; }' || blind=1

# apply_allow FINDINGS ALLOW OUT - the findings that are not covered by the
# allow file.  A line of the allow file is "RULE|file|function|count|why":
# the sites of that kind a function may still hold, found before the owner
# existed and outside the Help and Info readers.  A function holding more
# than its count has all its findings listed, and a line that allows more
# than the scan finds is stale and fails, so the counts can only fall.
apply_allow() {
  awk -F': ' -v allowfile="$2" '
    function key(rule, loc, fn,    f) {
      f = loc; sub(/:[0-9]+$/, "", f)
      return rule "|" f "|" fn
    }
    BEGIN {
      while ((getline l < allowfile) > 0) {
        if (l ~ /^[ \t]*(#|$)/) continue
        split(l, a, "|")
        allowed[a[1] "|" a[2] "|" a[3]] = a[4] + 0
      }
    }
    { k = key($1, $2, $3); found[k]++; line[NR] = $0; lkey[NR] = k; n = NR }
    END {
      for (i = 1; i <= n; i++)
        if (found[lkey[i]] > allowed[lkey[i]]) print line[i]
      for (k in allowed)
        if (found[k] < allowed[k])
          print "stale allow line: " k " allows " allowed[k] ", the scan finds " found[k] + 0
    }' "$1" > "$3"
}

ALLOW=${ALLOW:-$SRCDIR/tests/help-buffers.allow}
[ -f "$ALLOW" ] || ALLOW=/dev/null
scan_tree "$SRCDIR" "$WORK/scanned"
apply_allow "$WORK/scanned" "$ALLOW" "$WORK/problems"
if [ -s "$WORK/problems" ]; then
  sort "$WORK/problems"
  echo "check-help-buffers: $(wc -l < "$WORK/problems") offender(s)"
  exit $BAD
fi
if [ "$blind" -ne 0 ]; then
  exit $BAD
fi
echo "check-help-buffers: clean"
exit 0
