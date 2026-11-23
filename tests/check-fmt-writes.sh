#!/bin/sh
# check-fmt-writes.sh -- a text built with a format reaches storage through
# one owner (we_fmt.h), and what it writes is bounded by the storage itself.
#
# Every status row, command for the debugger, request for a server and path
# the editor builds with a format used to be written with the C library's own
# writers, each with a bound worked out by hand somewhere else.  Two of them
# wrote past their array (Search > Replace's report, the window frame's
# line:column) and one request header wrote past its buffer when the request
# line was long.  The owner states what a text that does not fit becomes
# (refused, cut and marked, or left out whole), and this script keeps the
# editor's sources from going round it.
#
#   check-fmt-writes.sh [--srcdir DIR] [--list]
#
# Rules over every top level source except we_fmt.c and we_fmt.h (the
# owner), after comments and the contents of literals are blanked:
#
#  R1. sprintf, vsprintf, asprintf, vasprintf and their __builtin forms are
#      named nowhere; a "%n" in a format is refused.  No allow line.
#  R2. snprintf, vsnprintf, swprintf, vswprintf, strftime and their
#      __builtin forms are named only in the functions the allow file lists
#      (FMT|file|function|count|why), the sites still to move onto the owner.
#      A function holding more than its count is named, and a line that
#      allows more than the scan finds is stale and fails, so the counts can
#      only fall.
#  R3. The value of snprintf or vsnprintf is never used: the call stands
#      alone as a statement (or as the body of an if, else, while or for).
#      That names `len += snprintf(...)`, `n = snprintf(...)`,
#      `return snprintf(...)` and a comparison of the value.  No allow line.
#  R4. The destination of snprintf or vsnprintf is not at an offset (no "+"
#      or "-" outside a subscript or a "->"), and its size has no "-".
#  R5. The refusal of a whole text is read.  A call of wpe_fmt_whole,
#      wpe_fmt_vwhole, WPE_FMT_WHOLE or wpe_fmt_end that stands alone, is
#      cast to void (also "(void)!"), is the body of an if, else, while or
#      for, has an empty body after its own test, sits in a #define, or is
#      returned by a function that does not carry WPE_FMT_MUST_READ, is
#      named.  The compiler holds the same line (-Werror=unused-result) for
#      what its warning sees; this scan covers what it does not.
#  R6. An array is given as an array: the first argument of wpe_fmt_whole or
#      wpe_fmt_shown is not followed by sizeof of the same name (the
#      WPE_FMT_WHOLE and WPE_FMT_SHOWN macros are that spelling, and refuse
#      a pointer at compile time).
#  R7. stpcpy, stpncpy, strlcpy, strlcat, wcscpy, wcsncpy, wcscat and the
#      __builtin copy forms are named nowhere.  (strcpy, strcat, strncpy and
#      strncat are held by rule 9 of check-path-buffers.sh until their own
#      pass.)
#  R8. No #define outside we_fmt.h whose value is the literal "..." (or the
#      UTF-8 ellipsis): the one mark of a cut sentence is WPE_FMT_CUT_MARK.
#      The one other owner's row marker is allowed by a MARK line.
#  MACRO. A #define whose body names a word of R1, R2 or R7 is named
#      wherever it stands, so a macro cannot carry a banned writer out.
#
#  R5 also names a refusal stored in a variable that the function never reads
#  (`int r = wpe_fmt_whole(...)`), a function that returns such a variable or
#  the call itself without WPE_FMT_MUST_READ (and every call of a function
#  that carries it is held to the same rule), a call dropped by a comma
#  expression or a conditional statement, and a writer named without being
#  called (a function pointer): R3 for snprintf and vsnprintf, R5 for the
#  must-read ones.
#
# Out of this scan, each with what brings it back in:
#  - a statement spliced over lines with a backslash inside a word, or a word
#    made by "##" pasting: the poison of the end state (every writer name
#    refused by the compiler wherever it is spelt) closes it; revisit when
#    that pass lands;
#  - fmemopen with fprintf, a stream used as storage: revisit on the first
#    call of fmemopen or open_memstream in a product source;
#  - a sizeof of another array than the destination, or of a pointer
#    (`wpe_fmt_whole(a, sizeof b, ...)`): the compiler does not see it either;
#    revisit if a finding of this shape is made, and then the array macro is
#    made the only spelling;
#  - a stored refusal read only by a statement that does not test it (the
#    variable named in a trace): revisit on a finding.
#
# A function is named at its "{" by the last line before it that holds
# IDENT( (so a K&R definition and a header split over lines are the
# function's), and the brace depth ends it (so a body on one line is its own
# function).
#
# Each rule has a canary: the script plants an offender in a tree of its own
# and fails when the rule does not name it (by function where it is a
# function's), so a scanner gone blind fails instead of passing.  The
# negative canaries plant the safe spellings and fail when a rule names them.
# Two compile probes check the compiler's side: a pointer given to
# WPE_FMT_WHOLE does not compile, and a refusal ignored warns.
#
# --list prints the allow lines for the current tree.
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1
LIST=0

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    *) echo "check-fmt-writes: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

. "$(dirname "$0")/workdir.sh"
work_dir fmt-writes $BAD

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$(dirname "$0")/blank-c.awk")

# The files that own the concern.
OWNER_FILES="we_fmt.c we_fmt.h"

# blank_file FILE MODE - the file's lines with comments blanked and the
# contents of literals dropped (mode 0) or kept (mode 1), one output line per
# input line.
blank_file() {
  awk "$BLANK_C"'
    FNR == 1 { blank_incom = 0 }
    { print blank_c($0, '"$2"') }' "$1"
}

# scan_file BLANKED REL MUSTREAD PHASE - the findings of the rules in one
# blanked file, "RULE: rel:line: function: what" each.  PHASE 1 prints only
# "MUSTREAD name" for each function declared with WPE_FMT_MUST_READ.
#
# The file is cut into units at ';', '{' and '}' that stand outside
# parentheses, brackets and string literals; the scan keeps the brace depth
# and the function it is in.
scan_file() {
  awk -v rel="$2" -v mustread=" $3 " -v phase="$4" '
    function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }
    function squeeze(s) { gsub(/[ \t]+/, "", s); return s }
    # nth_arg(s, from, n) - the nth argument of the call whose "(" comes just
    # before position from.
    function nth_arg(s, from, n,    i, len, depth, c, cur, k) {
      depth = 1; cur = ""; k = 1; len = length(s)
      for (i = from; i <= len; i++) {
        c = substr(s, i, 1)
        if (c == "(" || c == "[" || c == "{") depth++
        else if (c == ")" || c == "]" || c == "}") { depth--; if (depth == 0) break }
        if (c == "," && depth == 1) { if (k == n) return cur; k++; cur = ""; continue }
        cur = cur c
      }
      if (k == n) return cur
      return ""
    }
    # call_end(s, from) - the position just after the ")" of the call whose
    # "(" comes just before position from.
    function call_end(s, from,    i, len, depth, c) {
      depth = 1; len = length(s)
      for (i = from; i <= len; i++) {
        c = substr(s, i, 1)
        if (c == "(" || c == "[") depth++
        else if (c == ")" || c == "]") { depth--; if (depth == 0) return i + 1 }
      }
      return len + 1
    }
    # strip_heads(p) - p without the control heads that stand in front of a
    # statement: else, do, case X:, default:, if (...), while (...), for
    # (...), switch (...).
    function strip_heads(p,    e, again) {
      again = 1
      while (again) {
        again = 0
        p = trim(p)
        if (p ~ /^(else|do)([^A-Za-z0-9_]|$)/) { sub(/^(else|do)/, "", p); again = 1; continue }
        if (p ~ /^(case[^:]*|default[ \t]*):/) { sub(/^[^:]*:/, "", p); again = 1; continue }
        if (p ~ /^(if|while|for|switch)[ \t]*\(/) {
          e = call_end(p, index(p, "(") + 1)
          # a head whose ")" is not in p is the one the call stands in
          if (e > length(p) + 1 || substr(p, e - 1, 1) != ")") break
          p = substr(p, e)
          again = 1
        }
      }
      return p
    }
    function report(rule, what, ln) {
      if (phase == 1) return
      print rule ": " rel ":" (ln == "" ? unitline : ln) ": " (fname == "" ? "-" : fname) ": " what
    }
    function is_r1(t) {
      return (t ~ /^(sprintf|vsprintf|asprintf|vasprintf|__sprintf_chk|__vsprintf_chk)$/ ||
              t ~ /^__builtin_(v?a?sprintf)$/ || t ~ /^__builtin___v?sprintf_chk$/)
    }
    function is_fmt(t) {
      return (t ~ /^(snprintf|vsnprintf|swprintf|vswprintf|strftime|__snprintf_chk|__vsnprintf_chk)$/ ||
              t ~ /^__builtin_v?snprintf$/ || t ~ /^__builtin___v?snprintf_chk$/)
    }
    function is_copy(t) {
      return (t ~ /^(stpcpy|stpncpy|strlcpy|strlcat|wcscpy|wcsncpy|wcscat)$/ ||
              t ~ /^__builtin_(stpcpy|stpncpy)$/ || t ~ /^__builtin___(stpcpy|stpncpy)_chk$/)
    }
    function is_must(t) {
      return (t == "wpe_fmt_whole" || t == "wpe_fmt_vwhole" || t == "WPE_FMT_WHOLE" || t == "wpe_fmt_end")
    }
    function has_must(u,    rest, t) {
      rest = u
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        t = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        if (is_must(t)) return 1
      }
      return 0
    }
    # fmt_call(u, pos, tok) - rules 3 and 4 on one bounded write.
    function fmt_call(u, pos, tok,    p, a1, a2, t, from) {
      from = pos + length(tok)
      if (substr(u, from) !~ /^[ \t]*\(/) return
      from += index(substr(u, from), "(")
      report("FMT", tok)
      p = strip_heads(substr(u, 1, pos - 1))
      if (p !~ /^(\(void\))?[ \t]*$/) report("R3", "the value of " tok " is used")
      a1 = nth_arg(u, from, 1); a2 = nth_arg(u, from, 2)
      t = a1; gsub(/->/, "", t)
      while (match(t, /\[[^\[\]]*\]/)) t = substr(t, 1, RSTART - 1) substr(t, RSTART + RLENGTH)
      if (t ~ /[+-]/) report("R4", tok " writes at an offset: " trim(a1))
      t = a2; gsub(/->/, "", t)
      if (t ~ /-/) report("R4", tok " has a size with arithmetic: " trim(a2))
    }
    # must_call(u, pos, tok) - rule 5 on one must-read call.
    function must_call(u, pos, tok,    p, from, rest) {
      from = pos + length(tok)
      if (substr(u, from) !~ /^[ \t]*\(/) return
      from += index(substr(u, from), "(")
      p = strip_heads(substr(u, 1, pos - 1))
      if (p == "") { report("R5", "the result of " tok " is not read"); return }
      if (p ~ /^\(void\)[ \t!]*$/) { report("R5", "the result of " tok " is cast away"); return }
      rest = substr(u, call_end(u, from))
      # a comma expression throws the value of the call away
      if (p ~ /^\(+[ \t]*$/ && rest ~ /^[ \t]*,/) { report("R5", "the result of " tok " is dropped by a comma"); return }
      # so does a conditional expression that is a statement
      if (p ~ /^[^=]*[?:][ \t(]*$/ && p !~ /^(return|if|while|for)/ && p !~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/) {
        report("R5", "the result of " tok " is dropped by a conditional expression"); return
      }
      if (p ~ /^return[ \t]*\(?[ \t]*$/) {
        if (rest ~ /^[ \t);]*$/ && !(fname in mr) && !hdr_must)
          report("R5", "a function returns the result of " tok " without WPE_FMT_MUST_READ")
        return
      }
      # a result stored in a variable must be read later in the function
      if (rest ~ /^[ \t);]*$/ && match(p, /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t(]*$/)) {
        stored_name = substr(p, RSTART, RLENGTH); sub(/[ \t]*=.*$/, "", stored_name)
        stored[stored_name] = unitline; is_read[stored_name] = 0
        skip_name = stored_name
      }
    }
    # note_reads(u) - the variables holding a refusal that this unit reads;
    # a unit that returns one makes its function carry the attribute.
    function note_reads(u,    rest, tok) {
      rest = u
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        tok = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        if ((tok in stored) && tok != skip_name) {
          is_read[tok] = 1
          if (strip_heads(u) ~ ("^return[ \t(]*" tok "[ \t);]*$") && !(fname in mr) && !hdr_must)
            report("R5", "a function returns the stored result " tok " without WPE_FMT_MUST_READ")
        }
      }
    }
    # end_function() - a stored refusal nobody read.
    function end_function(    nm) {
      for (nm in stored)
        if (!is_read[nm])
          report("R5", "the result of a must-read call is stored in " nm " and never read", stored[nm])
      delete stored
      delete is_read
    }
    # array_call(u, pos, tok) - rule 6.
    function array_call(u, pos, tok,    from, a1, a2) {
      from = pos + length(tok)
      if (substr(u, from) !~ /^[ \t]*\(/) return
      from += index(substr(u, from), "(")
      a1 = squeeze(nth_arg(u, from, 1)); a2 = squeeze(nth_arg(u, from, 2))
      sub(/^sizeof\(?/, "", a2); sub(/\)$/, "", a2)
      if (a1 != "" && a1 == a2)
        report("R6", tok " is given the array " a1 " and its sizeof: write the macro")
    }
    function check_unit(u,    rest, off, tok, pos, prev, first, as_value) {
      rest = u; off = 0; skip_name = ""
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        tok = substr(rest, RSTART, RLENGTH)
        pos = off + RSTART
        prev = (pos > 1) ? substr(u, pos - 1, 1) : " "
        rest = substr(rest, RSTART + RLENGTH)
        off = pos + length(tok) - 1
        if (prev == "." || prev == ">" ) continue
        # a writer named but not called is a function pointer: its calls
        # could not be seen
        as_value = (substr(u, pos + length(tok)) !~ /^[ \t]*\(/)
        if (as_value && (tok == "snprintf" || tok == "vsnprintf")) report("R3", tok " is used as a value")
        else if (as_value && (is_must(tok) || (tok in mr))) report("R5", tok " is used as a value")
        else if (is_r1(tok)) report("R1", tok " writes with no bound")
        else if (is_copy(tok)) report("R7", tok " copies with no bound")
        else if (tok == "snprintf" || tok == "vsnprintf") fmt_call(u, pos, tok)
        else if (is_fmt(tok)) report("FMT", tok)
        else if (is_must(tok) || (tok in mr)) must_call(u, pos, tok)
        if (tok == "wpe_fmt_whole" || tok == "wpe_fmt_shown") array_call(u, pos, tok)
      }
      note_reads(u)
    }
    # note_decl(u) - a unit that declares a function with WPE_FMT_MUST_READ.
    function note_decl(u,    t) {
      if (u !~ /WPE_FMT_MUST_READ/) return
      t = u; sub(/\(.*$/, "", t); sub(/[ \t]+$/, "", t)
      # the attribute may stand before the name: take the name before the first "("
      if (match(t, /[A-Za-z_][A-Za-z0-9_]*$/)) {
        t = substr(t, RSTART)
        if (phase == 1) print "MUSTREAD " t
      }
    }
    function note_function(u,    t) {
      if (u ~ /^[A-Za-z_][^;]*\(/) {
        t = u; sub(/\(.*$/, "", t); sub(/[ \t]+$/, "", t)
        if (match(t, /[A-Za-z_][A-Za-z0-9_]*$/)) cand = substr(t, RSTART)
      }
      fname = cand
      hdr_must = (u ~ /WPE_FMT_MUST_READ/)
      if (phase == 1 && hdr_must && fname != "") print "MUSTREAD " fname
    }
    function unit(u, term, ln,    t) {
      u = trim(u)
      unitline = ln
      if (term == "}") {
        if (pending_empty != "" && u == "") report("R5", "a must-read result is tested and the body is empty", pending_empty)
        pending_empty = ""
        if (u != "") check_unit(u)
        depth--
        if (depth <= 0) { end_function(); depth = 0; fname = ""; hdr_must = 0 }
        return
      }
      pending_empty = ""
      t = u term
      if (depth == 0) {
        if (term == "{") note_function(t)
        else if (t ~ /^[A-Za-z_][^;]*\(/) {
          note_decl(t)
          cand = t; sub(/\(.*$/, "", cand); sub(/[ \t]+$/, "", cand)
          if (match(cand, /[A-Za-z_][A-Za-z0-9_]*$/)) cand = substr(cand, RSTART); else cand = ""
        }
      }
      if (term == "{") {
        depth++
        check_unit(t)
        if (t ~ /^(if|while|for)([ \t(]|\{)/ && has_must(t)) pending_empty = ln
        return
      }
      check_unit(t)
      if (t ~ /^(if|while|for)[ \t]*\(/ && strip_heads(u) == "" && has_must(u))
        report("R5", "a must-read result is tested and the body is empty")
    }
    # directive(line) - a preprocessor line: its words are scanned wherever
    # a macro would carry them.
    function directive(line, ln,    rest, tok, isdef) {
      isdef = (line ~ /^[ \t]*#[ \t]*define/)
      sub(/^[ \t]*#[ \t]*[a-z]+/, "", line)
      rest = line
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        tok = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        if (is_r1(tok) || is_fmt(tok) || is_copy(tok)) report("MACRO", "a #define names " tok, ln)
        else if (isdef && is_must(tok)) report("R5", "a #define holds the call of " tok, ln)
      }
    }
    BEGIN {
      depth = 0; paren = 0; cur = ""; first = 0; cont = 0; fname = ""; cand = ""; pp = 0
      pending_empty = ""; hdr_must = 0
      n = split(mustread, mrl, " ")
      for (k = 1; k <= n; k++) mr[mrl[k]] = 1
    }
    {
      line = $0
      if (cont) { directive(line, NR); cont = (line ~ /\\[ \t]*$/); next }
      if (line ~ /^[ \t]*#/) {
        if (line ~ /^[ \t]*#[ \t]*if/) ppdepth[++pp] = depth
        else if (line ~ /^[ \t]*#[ \t]*(else|elif)/) { if (pp > 0) depth = ppdepth[pp] }
        else if (line ~ /^[ \t]*#[ \t]*endif/) { if (pp > 0) pp-- }
        directive(line, NR)
        cont = (line ~ /\\[ \t]*$/); next
      }
      n = length(line)
      for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (cur == "" && c ~ /[ \t]/) continue
        if (cur == "") first = NR
        if (c == "\"" || c == "\047") { cur = cur c; continue }
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

# scan_literals LITERALFILE REL - rule 8 and the "%n" refusal, on the lines
# with their literals kept.
scan_literals() {
  awk -v rel="$2" '
    function report(rule, what, name) {
      print rule ": " rel ":" NR ": " (name == "" ? "-" : name) ": " what
    }
    BEGIN { ellipsis = sprintf("\"%c%c%c\"", 226, 128, 166) }
    /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+"/ {
      name = $0; sub(/^[ \t]*#[ \t]*define[ \t]+/, "", name); sub(/[ \t].*$/, "", name)
      val = $0; sub(/^[ \t]*#[ \t]*define[ \t]+[A-Za-z0-9_]+[ \t]+/, "", val); sub(/[ \t]+$/, "", val)
      if (val == "\"...\"" || val == ellipsis)
        report("MARK", "a #define holds the cut mark: use WPE_FMT_CUT_MARK", name)
    }
    {
      l = $0
      while (match(l, /"([^"\\]|\\.)*"/)) {
        lit = substr(l, RSTART, RLENGTH)
        l = substr(l, RSTART + RLENGTH)
        gsub(/%%/, "", lit)
        if (lit ~ /%[-+ #0-9.*hlLjzt]*n/) report("R1", "a format holds %n")
      }
    }' "$1"
}

# scan_tree DIR OUT - run the rules on the top level sources under DIR; the
# findings go to OUT, one per line, each starting with its rule.
scan_tree() {
  dir=$1; out=$2
  t=$(mktemp -d "$WORK/scan.XXXXXX")
  : > "$out"
  : > "$t/must.txt"
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    case " $OWNER_FILES " in *" $rel "*) continue ;; esac
    blank_file "$f" 0 > "$t/s0.txt"
    scan_file "$t/s0.txt" "$rel" "" 1 >> "$t/must.txt"
  done
  mr=$(sed -n 's/^MUSTREAD //p' "$t/must.txt" | sort -u | tr '\n' ' ')
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    case " $OWNER_FILES " in *" $rel "*) continue ;; esac
    blank_file "$f" 0 > "$t/s0.txt"
    blank_file "$f" 1 > "$t/s1.txt"
    scan_file "$t/s0.txt" "$rel" "$mr" 0 >> "$out"
    scan_literals "$t/s1.txt" "$rel" >> "$out"
  done
}

# plant CODE - a tree of its own holding CODE as we_planted.c; sets PLANTED to
# the findings file.
plant() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$1" > "$c/we_planted.c"
  scan_tree "$c" "$c/out"
  PLANTED=$c/out
}

# canary RULE FUNCTION CODE - fail unless RULE names FUNCTION in CODE.
canary() {
  rule=$1; fn=$2; code=$3
  plant "$code"
  if ! grep -q "^$rule: we_planted.c:[0-9]*: $fn: " "$PLANTED"; then
    echo "canary $rule in $fn: a planted offender was not named: $code"
    return 1
  fi
  return 0
}

# safe_canary CODE - fail when any rule names the safe spelling CODE.
safe_canary() {
  plant "$1"
  grep -v '^FMT:' "$PLANTED" > "$PLANTED.rest"
  if [ -s "$PLANTED.rest" ]; then
    echo "safe canary: a bounded spelling was named: $1"
    sed -n 's/^/  /p' "$PLANTED.rest"
    return 1
  fi
  return 0
}

# not_named RULE FUNCTION CODE - fail when RULE names FUNCTION in CODE.
not_named() {
  rule=$1; fn=$2; code=$3
  plant "$code"
  if grep -q "^$rule: we_planted.c:[0-9]*: $fn: " "$PLANTED"; then
    echo "canary: $rule named $fn where it must not: $code"
    return 1
  fi
  return 0
}

# canary2 RULE FUNCTION CODE1 CODE2 - the same, over a tree of two files (a
# wrapper in one, its caller in the other).
canary2() {
  rule=$1; fn=$2
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$3" > "$c/we_planted.c"
  printf '%s\n' "$4" > "$c/we_planted2.c"
  scan_tree "$c" "$c/out"
  if ! grep -q "^$rule: we_planted2*.c:[0-9]*: $fn: " "$c/out"; then
    echo "canary $rule in $fn: a planted offender was not named: $3 / $4"
    return 1
  fi
  return 0
}

blind=0
canary R1 planted 'void planted(char *p) { sprintf(p, "%d", 1); }' || blind=1
canary R1 planted 'void planted(char *p, const char *f, va_list ap) { vsprintf(p, f, ap); }' || blind=1
canary R1 planted 'void planted(char *p) { __builtin_sprintf(p, "x"); }' || blind=1
canary R1 planted 'void planted(char **p) { asprintf(p, "x"); }' || blind=1
canary R1 - 'void planted(int *x) { printf("%n", x); }' || blind=1
canary FMT planted 'void planted(char *p) { snprintf(p, 8, "x"); }' || blind=1
canary FMT planted 'void planted(char *p, const char *f, va_list ap) { vsnprintf(p, 8, f, ap); }' || blind=1
canary FMT planted 'void planted(char *p) { __builtin_snprintf(p, 8, "x"); }' || blind=1
canary R3 planted 'void planted(char *p, int len, int cap) { len += snprintf(p + len, cap - len, "x"); }' || blind=1
canary R3 planted 'void planted(char *a) { int n; n = snprintf(a, 8, "x"); }' || blind=1
canary R3 planted 'int planted(char *o, int n) { return snprintf(o, n, "x"); }' || blind=1
canary R3 planted 'void planted(char *a) { if (snprintf(a, 8, "x") >= 8) return; }' || blind=1
canary R3 planted 'void planted(char *a) { int n = snprintf(a, 8, "x"); }' || blind=1
canary R4 planted 'void planted(char *a, int u) { snprintf(a + u, 8 - u, "x"); }' || blind=1
canary R4 planted 'void planted(char *a, int u) { snprintf(a, sizeof a - 1, "x"); }' || blind=1
canary MACRO - '#define W snprintf
void planted(void) { }' || blind=1
canary MACRO - '#define W2(a) \
  sprintf(a, "x")
void planted(void) { }' || blind=1
canary R5 planted 'void planted(char *a) { wpe_fmt_whole(a, 8, "x"); }' || blind=1
canary R5 planted 'void planted(char *a) { (void) wpe_fmt_whole(a, 8, "x"); }' || blind=1
canary R5 planted 'void planted(char *a) { (void)!wpe_fmt_whole(a, 8, "x"); }' || blind=1
canary R5 planted 'void planted(char *a, int c) { if (c) wpe_fmt_whole(a, 8, "x"); else wpe_fmt_whole(a, 8, "y"); }' || blind=1
canary R5 planted 'void planted(char *a) { if (wpe_fmt_whole(a, 8, "x") == 0) { } }' || blind=1
canary R5 planted 'void planted(char *a) { if (wpe_fmt_whole(a, 8, "x") == 0) ; }' || blind=1
canary R5 planted 'void planted(char *a) { WPE_FMT_WHOLE(a, "x"); }' || blind=1
canary R5 planted 'int planted(char *a) { return wpe_fmt_whole(a, 8, "x"); }' || blind=1
canary R5 planted 'void planted(WpeFmtText *t) { wpe_fmt_end(t, 0); }' || blind=1
canary R5 - '#define RUN(a) wpe_fmt_whole(a, 8, "x")
void planted(void) { }' || blind=1
canary R6 planted 'void planted(void) { char buf[8]; if (wpe_fmt_whole(buf, sizeof buf, "x") != 0) return; }' || blind=1
canary R6 planted 'void planted(void) { char buf[8]; wpe_fmt_shown(buf, sizeof(buf), "x"); }' || blind=1
canary R7 planted 'void planted(char *d, const char *s) { stpcpy(d, s); }' || blind=1
canary R7 planted 'void planted(char *d, const char *s) { strlcpy(d, s, 8); }' || blind=1
canary MARK W3 '#define W3 "..."' || blind=1
# Bypasses: a result stored and never read, a wrapper that stores and returns
# it, a result dropped by a comma or a conditional, a writer used as a value.
canary R5 planted 'void planted(char *a) { int r = wpe_fmt_whole(a, 8, "x"); }' || blind=1
canary R5 planted 'void planted(char *a) { int r; r = wpe_fmt_end(0, 0); }' || blind=1
canary R5 planted 'int planted(char *a) { int r = wpe_fmt_whole(a, 8, "x"); return r; }' || blind=1
canary R5 planted 'int planted(char *a) { int r = wpe_fmt_whole(a, 8, "x"); if (a) return(r); return 0; }' || blind=1
canary R5 planted 'void planted(char *a, int c) { c ? wpe_fmt_whole(a, 8, "x") : 0; }' || blind=1
canary R5 planted 'void planted(char *a) { (wpe_fmt_whole(a, 8, "x"), 0); }' || blind=1
canary R5 planted 'void planted(char *a) { int (*f)(char *, size_t, const char *, ...) = wpe_fmt_whole; f(a, 8, "x"); }' || blind=1
canary R3 planted 'void planted(void) { int (*f)(char *, size_t, const char *, ...) = snprintf; }' || blind=1
canary R3 planted 'void planted(void) { void *f = &snprintf; }' || blind=1
canary2 R5 wrap_it 'int wrap_it(char *a) { int r = wpe_fmt_whole(a, 8, "x"); return r; }' 'void planted(char *a) { wrap_it(a); }' || blind=1
canary2 R5 planted 'static inline WPE_FMT_MUST_READ int wrap_it(char *a) { return wpe_fmt_whole(a, 8, "x"); }' 'void planted(char *a) { wrap_it(a); }' || blind=1
safe_canary 'static inline WPE_FMT_MUST_READ int planted(char *a) { int r = wpe_fmt_whole(a, 8, "x"); return r; }' || blind=1
safe_canary 'int planted(char *a, int c) { int r = c ? wpe_fmt_whole(a, 8, "x") : 0; return r != 0; }' || blind=1
safe_canary 'void planted(char *a, int c) { int r; r = wpe_fmt_whole(a, 8, "x"); if (c) r = 1; if (r) return; }' || blind=1
# K&R definition, split header and one-line body are the function'"'"'s own.
canary R3 planted 'int planted(a) char *a; { return snprintf(a, 8, "x"); }' || blind=1
canary R3 planted 'static int
planted(char *a)
{
  return snprintf(a, 8, "x");
}' || blind=1
canary R3 first_line 'static int first_line(char *a){ return snprintf(a, 8, "x"); }
void second_line(char *a) { }' || blind=1
not_named R3 second_line 'static int first_line(char *a){ return snprintf(a, 8, "x"); }
void second_line(char *a) { snprintf(a, 8, "y"); }' || blind=1
canary FMT second_line 'static int first_line(char *a){ return 0; }
void second_line(char *a) { snprintf(a, 8, "y"); }' || blind=1
safe_canary '/* sprintf(a, x) and snprintf(a + b, c - d, e) in a comment */
void planted(void) { }' || blind=1
safe_canary 'void planted(const char *s) { const char *w = "sprintf strcpy snprintf %d"; (void) w; }' || blind=1
safe_canary 'void planted(void) { my_snprintf_like(1); xsprintf(2); }' || blind=1
safe_canary 'void planted(char *a) { wpe_fmt_shown(a, 8, "x"); WPE_FMT_SHOWN(a, "x"); }' || blind=1
safe_canary 'void planted(char *a) { if (wpe_fmt_whole(a, 8, "x") != WPE_FMT_OK) return; }' || blind=1
safe_canary 'void planted(char *a) { int r = wpe_fmt_whole(a, 8, "x"); if (r) return; }' || blind=1
safe_canary 'void planted(char *a) { if (WPE_FMT_WHOLE(a, "x") != 0) return; }' || blind=1
safe_canary 'static inline WPE_FMT_MUST_READ int planted(char *a) { return wpe_fmt_whole(a, 8, "x"); }' || blind=1
safe_canary 'void planted(char *a, int c) { if (c) wpe_fmt_shown(a, 8, "x"); else wpe_fmt_shown(a, 8, "y"); }' || blind=1
safe_canary '#define WPE_X 1
void planted(char *a, int i) { a[i + 1] = 0; }' || blind=1
safe_canary 'void planted(struct s *p) { p->snprintf_count++; }' || blind=1
# A snprintf alone is found by R2 only; the rule without allow names it.
plant 'void planted(char *a) { snprintf(a, 8, "x"); }'
grep -v '^FMT:' "$PLANTED" > "$PLANTED.rest"
[ -s "$PLANTED.rest" ] && { echo "canary: a bounded call standing alone was named by a rule but R2"; sed -n 's/^/  /p' "$PLANTED.rest"; blind=1; }
grep -q '^FMT: we_planted.c:[0-9]*: planted: snprintf' "$PLANTED" || { echo "canary: R2 did not name a bounded call standing alone"; blind=1; }

# probes: what the compiler holds.
. "$(dirname "$0")/compiler.sh"
test_cc_require
probe_dir=$WORK/probe
mkdir "$probe_dir"
printf '%s\n' '#include "we_fmt.h"' 'int f(void) { char a[8]; return wpe_fmt_whole(a, sizeof a, "x") + WPE_FMT_WHOLE(a, "y"); }' > "$probe_dir/array_ok.c"
printf '%s\n' '#include "we_fmt.h"' 'int f(char *p) { return WPE_FMT_WHOLE(p, "x"); }' > "$probe_dir/array_bad.c"
printf '%s\n' '#include "we_fmt.h"' 'void f(char *a) { wpe_fmt_whole(a, 8, "x"); }' > "$probe_dir/result_bad.c"
printf '%s\n' '#include "we_fmt.h"' 'int f(char *a) { return wpe_fmt_whole(a, 8, "x"); }' > "$probe_dir/result_ok.c"
# -c, not -fsyntax-only: the unused-result warning is made after parsing.
probe() { test_cc -std=gnu99 -O0 -c -o /dev/null -Werror=unused-result -I"$SRCDIR" "$@" > /dev/null 2>&1; }
probe "$probe_dir/array_ok.c" || { echo "probe: WPE_FMT_WHOLE does not compile on an array"; blind=1; }
probe "$probe_dir/array_bad.c" && { echo "probe: WPE_FMT_WHOLE compiles on a pointer"; blind=1; }
probe "$probe_dir/result_ok.c" || { echo "probe: a returned result does not compile"; blind=1; }
probe "$probe_dir/result_bad.c" && { echo "probe: an ignored refusal compiles without an error"; blind=1; }

# apply_allow FINDINGS ALLOW OUT - the findings that are not covered by the
# allow file.  A line of the allow file is "RULE|file|function|count|why":
# the sites of that kind a function may still hold.  A function holding more
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

# list_allow FINDINGS - the allow lines of the tree as it is.
list_allow() {
  awk -F': ' '
    $1 == "FMT" || $1 == "MARK" {
      loc = $2; sub(/:[0-9]+$/, "", loc)
      k = $1 "|" loc "|" $3; cnt[k]++
    }
    END { for (k in cnt) print k "|" cnt[k] "|to move onto the owner" }' "$1" | sort
}

ALLOW=${ALLOW:-$SRCDIR/tests/fmt-writes.allow}
[ -f "$ALLOW" ] || ALLOW=/dev/null
scan_tree "$SRCDIR" "$WORK/scanned"
if [ "$LIST" -eq 1 ]; then
  list_allow "$WORK/scanned"
  exit 0
fi
apply_allow "$WORK/scanned" "$ALLOW" "$WORK/problems"
if [ -s "$WORK/problems" ]; then
  sort "$WORK/problems"
  echo "check-fmt-writes: $(wc -l < "$WORK/problems") offender(s)"
  exit $BAD
fi
if [ "$blind" -ne 0 ]; then
  exit $BAD
fi
echo "check-fmt-writes: clean"
exit 0
