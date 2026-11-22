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
#   check-path-buffers.sh [--srcdir DIR] [--allow FILE] [--list] [--selftest]
#
# --selftest runs only the canaries.  Twelve rules (the first ten numbered
# below, the system's reason is said (R11), and the owner's size (R12)), each with canaries: the script plants an offender in a copy of
# the sources and fails when the rule does not name it, so a scanner gone
# blind fails instead of passing.  The compiler holds the same line from the
# other end: warn_unused_result on the owner's declarations warns on a refusal
# ignored where the scan cannot see it, and probe_attribute below fails when
# that attribute is gone.  --list prints the copy calls found, one allow line
# for each function, for a pass that moves them.
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
#  7. No other path number.  A #define whose name ends in PATH_MAX, PATHMAX,
#     PATH_SIZE, DIR_MAX, FOLDER_MAX or NAME_MAX has to be, directly or
#     through other defines, one of the owner's numbers.  The names that end
#     so and are not a path (a function's name, a count of folders) are listed
#     in the allow file with their reason, and a line that matches no define
#     is stale.  #define PATH_MAX or NAME_MAX is never allowed.
#  8. The system's numbers stay in the owner.  PATH_MAX, _POSIX_PATH_MAX and
#     NAME_MAX appear in no source but we_path.h.
#  9. No unbounded copy that nobody has judged.  Every strcpy, strcat, strncpy
#     and sprintf of a top level source is counted per function and listed in
#     the allow file with its kind: "sized" (the destination is allocated at
#     its sources' length), "text" (a fixed or checked text into a larger
#     array) or "checked" (the length is tested just before).  A path that
#     fits none of them is not listed: it goes through the owner.  A count
#     may only fall, and a line the scan does not meet is stale.  The same
#     goes for a scanf, fscanf or sscanf (also vscanf, vfscanf, vsscanf) that
#     reads a word with %s or %[ and no width (%*s and %ms do not count; a
#     positional %1$s is unbounded, %1$255s is not): such a word is as long as
#     its input, so it is bounded by its buffer, or listed as a SCAN line with
#     the reason the input is bounded.  Read in .c and .h files, the format
#     anywhere in the call however many lines it spans, adjacent literals
#     joined.  Not seen: a format held in a macro or a variable, and a wrapper
#     under another name.
# 10. The cuts of every pass stay gone.  The private numbers of the passes
#     (WPE_BUILD_PATH_MAX, E_LSP_PATH_MAX, ...) and the strncpy of a path into
#     the buffers they moved are named in no source, and no string literal but
#     the owner's WPE_PATH_TOO_LONG_FMT says a path is longer than the system
#     allows (the allow file names the few sentences that are another
#     owner's).  The owner's sentence and its number stay the owner's:
#     WPE_PATH_TOO_LONG_FMT is named in no source but we_path.h, and no
#     snprintf or sprintf takes WPE_PATH_MAX, WPE_NAME_MAX,
#     WPE_PLATFORM_PATH_MAX or WPE_PLATFORM_NAME_MAX as an argument (a site
#     says wpe_path_say_too_long or wpe_path_say_name_too_long).
# 11. The system's reason is said, not guessed (R11a, R11b).
# 12. A buffer that holds a path is the owner's size.  A char array, local or
#     file scope, passed as a path to a libc call that takes one (stat, fopen,
#     getcwd, unlink ...), as the destination of the owner's writers, or as
#     the argument a PATHFN line of the allow file names, is sized by
#     WPE_PATH_SIZE, WPE_PATH_MAX or WPE_NAME_SIZE: directly, through defines,
#     or plus other terms.  A product, a division or a bare number is named.
#     ARRAY lines excuse an array with a reason; a stale PATHFN or ARRAY line
#     is R12x.  R13, in the same pass: wpe_ai_shell_quote cuts a text that does
#     not fit, so only ws_quote_whole and the ai_tool_* functions call it;
#     any other function quotes a path with wpe_ai_shell_quote_whole or is
#     listed as a QUOTE line.  Not seen: an array of several dimensions, one declared over
#     several statements, and a path handed on through a pointer variable.
#
# Exit: 0 clean, 1 offenders or a blind scanner, 2 the script could not run
# (the compiler does not run).

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")
# The one owner of the compiler call (tests/compiler.sh).
. "$HERE/compiler.sh"
ALLOW=
LIST=0
SELFTEST=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-path-buffers: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/path-buffers.allow

OWNER_HEADER=we_path.h

. "$(dirname "$0")/workdir.sh"
work_dir path-buffers $BAD

# strip_comments FILE - the file's lines with comments blanked, string
# literals kept, one output line per input line.
strip_comments() {
  awk "$BLANK_C"'
    FNR == 1 { blank_incom = 0 }
    { print blank_c($0, 1) }' "$1"
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
function arg_split(s, from,    i, n, len, depth, c, cur, instr, q) {
  n = 0; depth = 1; cur = ""; instr = 0; len = length(s)
  for (i = from; i <= len; i++) {
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
function strip_heads(s,    done, i, len, depth, c) {
  done = 0
  while (!done) {
    done = 1
    sub(/^[ \t]+/, "", s)
    if (s ~ /^(else|do)([ \t]|$)/) { sub(/^(else|do)[ \t]*/, "", s); done = 0; continue }
    if (s ~ /^(if|while|for)[ \t]*\(/) {
      depth = 0; len = length(s)
      for (i = index(s, "("); i <= len; i++) {
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

# The awk functions rule 9 reads a scan with.
#   unbounded_in_lit(lit): how many conversions of one format read a word with
#     %s or %[ and no width.  The width is the digits after the % (after a
#     positional "n$" if there is one); %*s stores nothing and %ms allocates,
#     so neither counts; %1$s is unbounded, %1$255s is not.
#   unbounded_in_call(call): the same over every string literal of a call's
#     text, adjacent literals ("%d" "%s") joined into one format first.
#   feed(line): add @line to the call being read, up to the paren that closes
#     it, skipping parens inside literals; returns what follows that paren, or
#     "" while the call is still open.  Uses CALL_BUF, CALL_DEPTH, CALL_OPEN.
AWK_SCAN_LIB='
function unbounded_in_lit(lit,    i, j, n, c, count, suppress, alloc, width) {
  count = 0; n = length(lit); i = 1
  while (i <= n) {
    c = substr(lit, i, 1)
    if (c != "%") { i++; continue }
    i++
    if (substr(lit, i, 1) == "%") { i++; continue }
    j = i
    while (substr(lit, j, 1) ~ /[0-9]/) j++
    if (j > i && substr(lit, j, 1) == "$") i = j + 1
    suppress = 0; alloc = 0; width = 0
    if (substr(lit, i, 1) == "*") { suppress = 1; i++ }
    while (substr(lit, i, 1) ~ /[0-9]/) { width = 1; i++ }
    while (substr(lit, i, 1) ~ /[hlLqjzt]/) i++
    if (substr(lit, i, 1) == "m") { alloc = 1; i++ }
    c = substr(lit, i, 1); i++
    if ((c == "s" || c == "[") && !suppress && !alloc && !width) count++
  }
  return count
}
function unbounded_in_call(call,    i, n, c, cur, count, in_group) {
  count = 0; n = length(call); i = 1; cur = ""; in_group = 0
  while (i <= n) {
    c = substr(call, i, 1)
    if (c == "\"") {
      i++
      while (i <= n && substr(call, i, 1) != "\"") {
        if (substr(call, i, 1) == "\\") { cur = cur substr(call, i, 2); i += 2 }
        else { cur = cur substr(call, i, 1); i++ }
      }
      i++; in_group = 1
      continue
    }
    if (c ~ /[ \t]/ && in_group) { i++; continue }
    if (in_group) { count += unbounded_in_lit(cur); cur = ""; in_group = 0 }
    if (c == "\047") {
      i++
      while (i <= n && substr(call, i, 1) != "\047") i += (substr(call, i, 1) == "\\") ? 2 : 1
    }
    i++
  }
  if (in_group) count += unbounded_in_lit(cur)
  return count
}
function feed(line,    i, n, c, q) {
  n = length(line); i = 1
  while (i <= n) {
    c = substr(line, i, 1)
    if (c == "\"" || c == "\047") {
      q = c; CALL_BUF = CALL_BUF c; i++
      while (i <= n && substr(line, i, 1) != q) {
        if (substr(line, i, 1) == "\\") { CALL_BUF = CALL_BUF substr(line, i, 2); i += 2 }
        else { CALL_BUF = CALL_BUF substr(line, i, 1); i++ }
      }
      CALL_BUF = CALL_BUF q; i++
      continue
    }
    if (c == "(") CALL_DEPTH++
    else if (c == ")") {
      CALL_DEPTH--
      if (CALL_DEPTH == 0) { CALL_OPEN = 0; return substr(line, i + 1) }
    }
    CALL_BUF = CALL_BUF c; i++
  }
  CALL_BUF = CALL_BUF " "
  return ""
}
'

# scan_tree DIR OUT [ALLOW] - run the rules on the top level sources under
# DIR; the findings go to OUT, one per line, each starting with its rule.
# ALLOW is the file that lists what rules 7, 9 and 10 let stand; a canary's
# tree has none, so anything the rule meets is an offender there.  The copy
# calls found are left in SCAN_COPIES, "file|function|count" each.
scan_tree() {
  dir=$1; out=$2; allow=${3:-/dev/null}
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
        okname = "(WPE_PATH_SIZE|WPE_NAME_SIZE)"
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

  # Rule 7: no other path number.
  awk -F'|' -v owner="$OWNER_HEADER" '
    function owned(name, depth,    v, w) {
      v = val[name]
      if (v ~ /(^|[^A-Za-z0-9_])WPE_(PATH_MAX|PATH_SIZE|NAME_MAX|NAME_SIZE|PATH_FIELD_MAX|NAME_FIELD_MAX)([^A-Za-z0-9_]|$)/)
        return 1
      w = v; gsub(/[ \t()]/, "", w)
      if (depth < 8 && w ~ /^[A-Za-z_][A-Za-z0-9_]*$/ && (w in val))
        return owned(w, depth + 1)
      return 0
    }
    FILENAME == ARGV[1] { if ($1 == "NUMBER") ok[$2] = 1; next }
    {
      v = $0; sub(/^[^|]*\|[^|]*\|[^|]*\|?/, "", v)
      if (!($3 in val)) val[$3] = v
      defs[++n] = $1 "|" $2 "|" $3
    }
    END {
      for (i = 1; i <= n; i++) {
        split(defs[i], d, "|"); rel = d[1]; ln = d[2]; name = d[3]
        if (rel == owner) continue
        if (name == "PATH_MAX" || name == "NAME_MAX" || name == "_POSIX_PATH_MAX") {
          print "R7: " rel ":" ln ": " name " is defined outside " owner; continue
        }
        if (name !~ /(PATH_MAX|PATHMAX|PATH_SIZE|DIR_MAX|FOLDER_MAX|NAME_MAX)$/) continue
        if (name in ok) { used[name] = 1; continue }
        if (!owned(name, 0))
          print "R7: " rel ":" ln ": " name " is a path number of its own: derive it from WPE_PATH_SIZE, WPE_PATH_MAX or WPE_NAME_MAX, or list it as not a path"
      }
      for (k in ok) if (!(k in used))
        print "R7: stale allow line: no define of " k " ends like a path number"
    }' "$allow" "$t/defines" >> "$out"

  # Rules 8 and 10, over every comment stripped source but the owner.
  awk -F'|' -v owner="$OWNER_HEADER" "$AWK_LIB"'
    BEGIN {
      nid = split("WPE_BUILD_PATH_MAX E_EXECUTE_PATH_MAX E_D_COMPILE_PATH_MAX E_LSP_PATH_MAX E_DAP_FRAME_PATH_MAX HOST_PATH_MAX E_GDBMI_PATH_MAX E_SCALA_SETUP_DIR_MAX E_LSP_ROOT_MAX WPE_SYNTAX_PATH_MAX AI_TRANSCRIPT_PATH_MAX CONTAIN_TOKEN_MAX WLF_NAME_SIZE", ids, " ")
    }
    FILENAME == ARGV[1] { if ($1 == "SENTENCE") sent[$2] = 1; next }
    {
      rel = FILENAME; sub(/^.*\//, "", rel)
      if (rel == owner) next
      l = tight($0)
      if (l ~ /(^|[^A-Za-z0-9_])(PATH_MAX|_POSIX_PATH_MAX|NAME_MAX)([^A-Za-z0-9_]|$)/)
        print "R8: " rel ":" FNR ": the system number of a path or a name is named outside " owner
      for (i = 1; i <= nid; i++)
        if (l ~ ("(^|[^A-Za-z0-9_])" ids[i] "([^A-Za-z0-9_]|$)"))
          print "R10: " rel ":" FNR ": a private path number is back: " ids[i]
      if (l ~ /(^|[^A-Za-z0-9_])char[ \t]+path\[1024\]/)
        print "R10: " rel ":" FNR ": a path from the model is held in 1024 bytes and cut: take it whole (strndup)"
      if (l ~ /strncpy[ \t]*\([ \t]*(g_lsp_file|g_dap_progdir|g_dap_stop_file|e_d_file|ck_dir)[^A-Za-z0-9_]/)
        print "R10: " rel ":" FNR ": a path is cut by strncpy into a buffer the owner sized"
      if (l ~ /"[^"]*(path is longer than|path is too long)[^"]*"/) {
        if (rel in sent) sentused[rel] = 1
        else print "R10: " rel ":" FNR ": a sentence about a path\047s length of its own: say wpe_path_say_too_long or wpe_path_say_name_too_long"
      }
    }
    END {
      for (k in sent) if (!(k in sentused))
        print "R10: stale allow line: " k " has no sentence about a path\047s length of its own"
    }' "$allow" "$t"/s/*.c "$t"/s/*.h >> "$out"

  # Rule 10, the sentence and its number, over whole statements (a call split
  # over lines is one statement).  WPE_PATH_TOO_LONG_FMT is named only in the
  # owner's header, and no snprintf or sprintf takes one of the path numbers
  # as an argument: a site that prints a number says its own, and it is the
  # editor's room, not the system's.  Whole words only: WPE_PATH_MAX_HOPS is
  # another number.
  awk -v owner="$OWNER_HEADER" "$AWK_LIB"'
    BEGIN {
      word = "(^|[^A-Za-z0-9_])"
      endw = "([^A-Za-z0-9_]|$)"
      numbers = word "(WPE_PATH_MAX|WPE_NAME_MAX|WPE_PLATFORM_PATH_MAX|WPE_PLATFORM_NAME_MAX)" endw
      fmt = word "WPE_PATH_TOO_LONG_FMT" endw
      fn = word "(snprintf|sprintf)[ \t]*\\("
    }
    {
      cut_stmt($0)
      if (REL == owner) next
      if (S ~ fmt)
        print "R10: " REL ":" LN ": WPE_PATH_TOO_LONG_FMT is named outside " owner ": say wpe_path_say_too_long or wpe_path_say_name_too_long"
      rest = S
      while (match(rest, fn)) {
        start = RSTART + RLENGTH
        n = arg_split(rest, start)
        for (k = 1; k <= n; k++)
          if (ARGS[k] ~ numbers) {
            print "R10: " REL ":" LN ": a path number is an argument of snprintf or sprintf: the sentence and its number are the owner\047s"
            break
          }
        rest = substr(rest, start)
      }
    }' "$t/stmts" >> "$out"

  # Rule 9: copies counted per function, against the allow file.  A function
  # starts at a line in column 0 that names an identifier followed by "(" and
  # does not end with ";" (a prototype), as in check-start-plan.sh.
  : > "$t/copies"
  for f in "$t"/s/*.c; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    awk -v rel="$rel" '
      BEGIN { fn = "(file scope)" }
      {
        out = $0
        if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
          name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
          if (name != "") fn = name
        }
        s = out
        while (match(s, /(^|[^A-Za-z0-9_])(strcpy|strcat|strncpy|sprintf)[ \t]*[(]/)) {
          cnt[fn]++; s = substr(s, RSTART + RLENGTH)
        }
      }
      END { for (f in cnt) print rel "|" f "|" cnt[f] }' "$f" >> "$t/copies"
  done
  sort "$t/copies" > "$t/copies.sorted"
  cp "$t/copies.sorted" "$WORK/last-copies"
  awk -F'|' '
    FILENAME == ARGV[1] {
      if ($1 != "COPY") next
      key = $2 "|" $3
      if (NF < 6 || $4 !~ /^[0-9]+$/) { print "R9: malformed allow line: " $0; next }
      if ($5 !~ /^(sized|text|checked)$/) {
        print "R9: allow line for " $2 " " $3 " is of kind " $5 ", not sized, text or checked: a path goes through the owner"; next
      }
      al[key] = $4; kind[key] = $5; next
    }
    {
      key = $1 "|" $2; got[key] = 1
      if (!(key in al))
        print "R9: " $1 ": " $2 " has " $3 " strcpy, strcat, strncpy or sprintf call(s) nobody has judged: move them onto the path owner, or list them with their kind"
      else if ($3 + 0 > al[key] + 0)
        print "R9: " $1 ": " $2 " has " $3 " call(s), the allow file says " al[key]
      else if ($3 + 0 < al[key] + 0)
        print "R9: " $1 ": " $2 " has " $3 " call(s), lower the allow line from " al[key]
    }
    END {
      for (k in al) if (!(k in got)) print "R9: stale allow line, delete it: " k
    }' "$allow" "$t/copies.sorted" >> "$out"

  # Rule 9, the other way into a buffer: a scan that reads a word with %s or
  # %[ and no width writes as many bytes as the input has.  Counted per
  # function like the copies, against SCAN allow lines.
  : > "$t/scans"
  for f in "$t"/s/*.c "$t"/s/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    awk -v rel="$rel" "$AWK_SCAN_LIB"'
      BEGIN { fn = "(file scope)"; CALL_OPEN = 0 }
      {
        out = $0
        if (!CALL_OPEN && out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
          name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
          if (name != "") fn = name
        }
        s = out
        while (1) {
          if (CALL_OPEN) {
            s = feed(s)
            if (CALL_OPEN) break
            cnt[call_fn] += unbounded_in_call(CALL_BUF)
          } else {
            if (!match(s, /(^|[^A-Za-z0-9_])(scanf|fscanf|sscanf|vscanf|vfscanf|vsscanf)[ \t]*[(]/)) break
            s = substr(s, RSTART + RLENGTH)
            CALL_OPEN = 1; CALL_DEPTH = 1; CALL_BUF = ""; call_fn = fn
          }
        }
      }
      END { for (f in cnt) if (cnt[f] > 0) print rel "|" f "|" cnt[f] }' "$f" >> "$t/scans"
  done
  sort "$t/scans" > "$t/scans.sorted"
  cp "$t/scans.sorted" "$WORK/last-scans"
  awk -F'|' '
    FILENAME == ARGV[1] {
      if ($1 != "SCAN") next
      key = $2 "|" $3
      if (NF < 5 || $4 !~ /^[0-9]+$/ || $5 == "") { print "R9: malformed allow line: " $0; next }
      al[key] = $4; next
    }
    {
      key = $1 "|" $2; got[key] = 1
      if (!(key in al))
        print "R9: " $1 ": " $2 " reads " $3 " word(s) with %s or %[ and no width: bound the scan by its buffer (a width, or a line reader and wpe_field_take), or list it as a SCAN line with its reason"
      else if ($3 + 0 > al[key] + 0)
        print "R9: " $1 ": " $2 " reads " $3 " unbounded word(s), the allow file says " al[key]
      else if ($3 + 0 < al[key] + 0)
        print "R9: " $1 ": " $2 " reads " $3 " unbounded word(s), lower the allow line from " al[key]
    }
    END {
      for (k in al) if (!(k in got)) print "R9: stale allow line, delete it: " k
    }' "$allow" "$t/scans.sorted" >> "$out"

  # Rule 11: the system's reason is said, not guessed.  Per function, over
  # the comment stripped sources (strings kept): "P" when it holds a word of
  # the project's "did not open" sentences, "G" when it asks the system about
  # a path (access, stat, lstat, fopen, open, opendir, realpath), holds a
  # string that says "not found" or "cannot open", and names neither errno
  # nor strerror: a cause the editor guessed after the system answered.
  : > "$t/words"
  for f in "$t"/s/*.c; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    awk -v rel="$rel" '
      function flush() {
        if (fn == "") return
        if (proj) print "P|" rel "|" fn
        if (call && lit && !named) print "G|" rel "|" fn
      }
      {
        out = $0
        if (out ~ /^}/) { flush(); fn = ""; next }
        if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
          name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
          if (name != "") { flush(); fn = name; call = 0; lit = 0; named = 0; proj = 0 }
        }
        if (fn == "") next
        code = out; gsub(/"([^"\\]|\\.)*"/, "\"\"", code)
        if (code ~ /(^|[^A-Za-z0-9_.>])(access|stat|lstat|fopen|open|opendir|realpath|wpe_read_(open|stream_open|text|folder|name_ok))[ \t]*[(]/) call = 1
        if (code ~ /(^|[^A-Za-z0-9_])(errno|strerror)([^A-Za-z0-9_]|$)/) named = 1
        if (out ~ /"[^"]*([Nn]ot found|Can[\\\047]*t open|[Cc]annot open)[^"]*"/) lit = 1
        if (out ~ /"[^"]*(Project file not found|Cannot open project)[^"]*"/) proj = 1
      }
      END { flush() }' "$f" >> "$t/words"
  done
  sort "$t/words" > "$t/words.sorted"
  cp "$t/words.sorted" "$WORK/last-words"
  awk -F'|' '
    FILENAME == ARGV[1] {
      if ($1 == "PROJECTWORDS") {
        if (NF < 4) { print "R11a: malformed allow line: " $0; next }
        pw[$2 "|" $3] = 1; next
      }
      if ($1 == "GUESS") {
        if (NF < 5 || $4 == "") { print "R11b: malformed allow line: " $0; next }
        gs[$2 "|" $3] = 1
      }
      next
    }
    {
      key = $2 "|" $3
      if ($1 == "P") {
        pwseen[key] = 1
        if (!(key in pw))
          print "R11a: " $2 ": " $3 " says the project did not open in words of its own: the reporter e_prj_say_not_opened is the one place for them"
      } else {
        gsseen[key] = 1
        if (!(key in gs))
          print "R11b: " $2 ": " $3 " asks the system about a path and then says a cause it guessed (not found, cannot open) without the system\047s errno or strerror"
      }
    }
    END {
      for (k in pw) if (!(k in pwseen)) print "R11a: stale allow line, delete it: PROJECTWORDS " k
      for (k in gs) if (!(k in gsseen)) print "R11b: stale allow line, delete it: GUESS " k
    }' "$allow" "$t/words.sorted" >> "$out"

  # Rule 12: a buffer that receives or names a path is the owner's size.  A
  # char array, of any scope, whose size is not WPE_PATH_SIZE, WPE_PATH_MAX or
  # WPE_NAME_SIZE (directly, through defines, or plus other terms) is named
  # when it is passed as a path to a libc call that takes one, to the owner's
  # writers (as the destination), or to a function a PATHFN line of the allow
  # file names.  The statements of one file are read in order with the brace
  # depth, so an array is known from its declaration to the end of its
  # function (a file scope array to the end of the file).
  awk -F'|' "$AWK_LIB"'
    function expand(e, depth,    out, rest, id, pre) {
      out = ""; rest = e
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*/)) {
        id = substr(rest, RSTART, RLENGTH); pre = substr(rest, 1, RSTART - 1)
        rest = substr(rest, RSTART + RLENGTH)
        if (id ~ /^(WPE_PATH_SIZE|WPE_PATH_MAX|WPE_NAME_SIZE)$/ || !(id in val) || depth >= 8)
          out = out pre id
        else
          out = out pre "(" expand(val[id], depth + 1) ")"
      }
      return out rest
    }
    # sized_by_owner(e) - is the size @e the owners number, alone or plus other terms?
    function sized_by_owner(e,    x) {
      x = expand(e, 0)
      gsub(/"[^"]*"/, "", x)
      if (x ~ /[*\/%-]|<<|>>/) return 0
      return (x ~ /(^|[^A-Za-z0-9_])(WPE_PATH_SIZE|WPE_PATH_MAX|WPE_NAME_SIZE)([^A-Za-z0-9_]|$)/)
    }
    function declare(name, size, fn,    tbl) {
      gsub(/[ \t]+/, "", size)
      if (fn == "(file scope)") { fsize[name] = size; ffn[name] = fn; return }
      lsize[name] = size; lfn[name] = fn
    }
    # declarations(stmt, fn) - the char arrays a declaration statement makes
    function declarations(t, fn,    rest, pre, m, after, nm, closep, size, ptr, multi) {
      while (t ~ /^(static|const|register|extern|volatile|unsigned|signed)[ \t]+/) sub(/^[A-Za-z]+[ \t]+/, "", t)
      if (t !~ /^char([ \t*]|$)/) return
      rest = substr(t, 5)
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\[/)) {
        pre = substr(rest, 1, RSTART - 1); m = substr(rest, RSTART, RLENGTH)
        after = substr(rest, RSTART + RLENGTH)
        nm = m; sub(/[ \t]*\[$/, "", nm)
        closep = index(after, "]"); if (!closep) break
        size = substr(after, 1, closep - 1); rest = substr(after, closep + 1)
        ptr = (pre ~ /\*[ \t]*$/); multi = (rest ~ /^[ \t]*\[/)
        if (!ptr && !multi && size !~ /^[ \t]*$/) declare(nm, size, fn)
      }
    }
    # array_named(arg) - the name of the array @arg is, or points into, or ""
    function array_named(a,    nm) {
      sub(/^[ \t]+/, "", a); sub(/[ \t]+$/, "", a)
      if (a ~ /^[A-Za-z_][A-Za-z0-9_]*$/) return a
      if (match(a, /^&[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\[/)) { nm = substr(a, 2, RLENGTH - 2); gsub(/[ \t\[]/, "", nm); return nm }
      if (match(a, /^[A-Za-z_][A-Za-z0-9_]*[ \t]*\+/)) { nm = substr(a, 1, RLENGTH - 1); gsub(/[ \t]/, "", nm); return nm }
      return ""
    }
    function check_call(rest, fname, ids,    start, n, k, nm, size, fn, key) {
      while (match(rest, "(^|[^A-Za-z0-9_.>])" fname "[ \t]*\\(")) {
        start = RSTART + RLENGTH
        n = arg_split(rest, start)
        for (k = 1; k <= n; k++) {
          if (index(ids, " " (k - 1) " ") == 0) continue
          nm = array_named(ARGS[k])
          if (nm == "") continue
          if (nm in lsize) { size = lsize[nm]; fn = lfn[nm] }
          else if (nm in fsize) { size = fsize[nm]; fn = ffn[nm] }
          else continue
          if (sized_by_owner(size)) continue
          key = REL "|" fn "|" nm
          if (key in ex) { exused[key] = 1; continue }
          print "R12: " REL ":" LN ": " nm "[" size "] is passed to " fname " as a path: size it WPE_PATH_SIZE, WPE_PATH_MAX or WPE_NAME_SIZE, or list it as an ARRAY line"
        }
        rest = substr(rest, start)
      }
    }
    BEGIN {
      # the libc calls that take a path, and which arguments (0 based) are one
      split("stat:0 lstat:0 open:0 openat:1 fopen:0 freopen:0 access:0 chdir:0 mkdir:0 rmdir:0 unlink:0 remove:0 rename:0,1 link:0,1 symlink:0,1 readlink:0,1 realpath:0,1 getcwd:0 opendir:0 mkstemp:0 mkostemp:0 mkdtemp:0 truncate:0 chmod:0 execv:0 execve:0 execvp:0 statvfs:0 fstatat:1 wpe_path_copy:0 wpe_path_join:0 wpe_path_cat:0 wpe_path_search_in:2 wpe_read_open:0 wpe_read_stream_open:0 wpe_read_text:0 wpe_read_folder:0 wpe_read_name_ok:0", sk, " ")
      for (i in sk) {
        split(sk[i], kv, ":"); gsub(/,/, " ", kv[2])
        sink[kv[1]] = " " kv[2] " "
      }
    }
    FILENAME == ARGV[1] {
      if ($1 == "PATHFN") {
        if (NF < 5 || $4 !~ /^[0-9]+$/ || $5 == "") { print "R12x: malformed allow line: " $0; next }
        sink[$3] = sink[$3] " " $4 " "
        pathfn[$2 "|" $3] = 1
      } else if ($1 == "QUOTE") {
        if (NF < 4 || $4 == "") { print "R12x: malformed allow line: " $0; next }
        qok[$2 "|" $3] = 1
      } else if ($1 == "ARRAY") {
        if (NF < 5 || $5 == "") { print "R12x: malformed allow line: " $0; next }
        ex[$2 "|" $3 "|" $4] = 1
      }
      next
    }
    FILENAME == ARGV[2] {
      v = $0; sub(/^[^|]*\|[^|]*\|[^|]*\|?/, "", v)
      if (!($3 in val)) val[$3] = v
      next
    }
    {
      cut_stmt($0)
      if (REL != lastrel) {
        lastrel = REL; depth = 0; infn = 0; pending = ""
        for (k in lsize) delete lsize[k]
        for (k in fsize) delete fsize[k]
      }
      s = S; sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s)
      if (s ~ /^#/) next
      opens = (s ~ /\{$/); closes = (s ~ /\}$/)
      if (depth == 0) {
        if (opens && s !~ /=/ && s ~ /\(/) {
          fname = s; sub(/[ \t]*\(.*/, "", fname); sub(/.*[^A-Za-z0-9_]/, "", fname)
          defs[REL "|" fname] = 1
          for (k in lsize) delete lsize[k]
          infn = 1
        } else if (s == "{" && pending != "") {
          fname = pending; defs[REL "|" fname] = 1
          for (k in lsize) delete lsize[k]
          infn = 1
        } else if (!opens) {
          if (s ~ /\(/ && s !~ /=/ && s !~ /^typedef/) {
            pending = s; sub(/[ \t]*\(.*/, "", pending); sub(/.*[^A-Za-z0-9_]/, "", pending)
          }
          declarations(s, "(file scope)")
        }
      } else if (infn) {
        declarations(s, fname)
        # The cutting quoter: a path quoted by it into a buffer too small is
        # cut into the path of another file.  Only ws_quote_whole and the
        # tools (which quote the model'"'"'s words, not a path) call it.
        if (s ~ /(^|[^A-Za-z0-9_.>])wpe_ai_shell_quote[ \t]*\(/ && fname != "ws_quote_whole" && fname !~ /^ai_tool_/) {
          if ((REL "|" fname) in qok) qused[REL "|" fname] = 1
          else print "R13: " REL ":" LN ": " fname " calls wpe_ai_shell_quote, which cuts: quote a path with wpe_ai_shell_quote_whole, or list the function as a QUOTE line"
        }
        if (s ~ /\(/ && (length(lsize) > 0 || length(fsize) > 0))
          for (nm in sink) check_call(s, nm, sink[nm])
      }
      if (opens) depth++
      else if (closes) { depth--; if (depth <= 0) { depth = 0; infn = 0 } }
    }
    END {
      for (k in pathfn) {
        split(k, p, "|")
        if (!(k in defs)) print "R12x: stale allow line, delete it: PATHFN " p[1] " " p[2] " (no such function in the file)"
      }
      for (k in ex) if (!(k in exused)) print "R12x: stale allow line, delete it: ARRAY " k
      for (k in qok) if (!(k in qused)) print "R12x: stale allow line, delete it: QUOTE " k
    }' "$allow" "$t/defines" "$t/stmts" >> "$out"
}

# plant_tree CODE ALLOWLINES - a tree of its own (the owner's header and one
# source holding CODE), scanned with an allow file of ALLOWLINES (none when
# empty); the findings are left in $PLANT_OUT.
plant_tree() {
  code=$1; lines=$2
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  cp "$SRCDIR/$OWNER_HEADER" "$c"/ 2>/dev/null
  : > "$c/we_planted.c"    # the rules that read the sources want one to exist
  printf '%s\n' "$code" > "$c/${PLANT_FILE:-we_planted.c}"
  : > "$c/allow"
  [ -z "$lines" ] || printf '%s\n' "$lines" > "$c/allow"
  scan_tree "$c" "$c/out" "$c/allow"
  PLANT_OUT=$c/out
}

# canary RULE CODE [ALLOWLINES] - plant CODE in a tree of its own (the
# owner's header and one source), scan, and fail unless RULE names it.  The
# rules look at statements, never at the rest of the tree, so a small tree
# proves the same and keeps the scan fast.
canary() {
  rule=$1; code=$2
  plant_tree "$code" "$3"
  if ! grep -q "^$rule:" "$PLANT_OUT"; then
    echo "canary $rule: a planted offender was not found: $code"
    return 1
  fi
  return 0
}

# quiet_canary RULE CODE [ALLOWLINES] - the safe spelling: fail when RULE
# names CODE, so a rule that grew too wide fails instead of passing.
quiet_canary() {
  rule=$1; code=$2
  plant_tree "$code" "$3"
  if grep -q "^$rule:" "$PLANT_OUT"; then
    echo "canary $rule: a safe spelling was named: $code"
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
# the same program reading it must compile.  A compiler that does not know the
# flag skips the probe in plain view; one that knows it and cannot compile the
# good program fails the script.
probe_attribute() {
  printf 'int x;\n' > "$WORK/probe_empty.c"
  if ! test_cc -c -o /dev/null -Werror=unused-result "$WORK/probe_empty.c" 2>/dev/null; then
    echo "SKIP: attribute probe: $CC has no -Werror=unused-result"
    return 0
  fi
  good=$WORK/probe_good.c
  printf '#include <string.h>\n#include "we_path.h"\n#include "we_fieldtake.h"\nint planted(char *a) { return wpe_path_copy(a, 4, "x") + wpe_path_join(a, 4, "x", "y") + wpe_path_cat(a, 4, "x") + wpe_field_take("x", a, 4); }\n' > "$good"
  if ! test_cc -I"$SRCDIR" -c -o /dev/null -Werror=unused-result "$good" 2>"$WORK/probe_good.err"; then
    echo "canary attribute: $CC knows -Werror=unused-result but the program that reads the answers does not compile, so the probe is blind:"
    head -n 3 "$WORK/probe_good.err"
    return 1
  fi
  for call in 'wpe_path_copy(a, 4, "x")' 'wpe_path_join(a, 4, "x", "y")' \
              'wpe_path_cat(a, 4, "x")' 'wpe_field_take("x", a, 4)'; do
    bad=$WORK/probe_bad.c
    printf '#include <string.h>\n#include "we_path.h"\n#include "we_fieldtake.h"\nvoid planted(char *a, int c) { if (c) %s; }\n' "$call" > "$bad"
    if test_cc -I"$SRCDIR" -c -o /dev/null -Werror=unused-result "$bad" 2>/dev/null; then
      echo "canary attribute: an ignored $call compiles; warn_unused_result is gone from we_path.h or we_fieldtake.h"
      return 1
    fi
  done
  echo "check-path-buffers: the attribute probe compiled with $CC and was judged"
  return 0
}

blind=0
test_cc_require
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
# Rule 7: a number of a path's own.
canary R7 '#define MY_DIR_MAX 512' || blind=1
canary R7 '#define E_X_PATH_MAX 2048' || blind=1
canary R7 '#define E_X_PATHMAX 2048' || blind=1
canary R7 '#define E_X_FOLDER_MAX 4096' || blind=1
canary R7 '#define E_X_NAME_MAX 300' || blind=1
canary R7 '#define PATH_MAX 4096' || blind=1
canary R7 '#define A_PATH_MAX B_PATH_MAX
#define B_PATH_MAX 300' || blind=1
quiet_canary R7 '#define E_X_PATH_SIZE (WPE_PATH_SIZE / 2 + 1)' || blind=1
quiet_canary R7 '#define E_X_PATH_MAX WPE_PATH_SIZE' || blind=1
quiet_canary R7 '#define E_X_PATH_MAX B_PATH_MAX
#define B_PATH_MAX (WPE_PATH_MAX)' || blind=1
quiet_canary R7 '#define E_X_NAME_MAX 32' 'NUMBER|E_X_NAME_MAX|a function name' || blind=1
canary R7 '#define E_Y 1' 'NUMBER|E_X_NAME_MAX|a function name' || blind=1
# Rule 8: the system's numbers stay in the owner.
canary R8 'void planted(void) { char b[PATH_MAX]; }' || blind=1
canary R8 'int planted(void) { return _POSIX_PATH_MAX; }' || blind=1
canary R8 'int planted(void) { return NAME_MAX; }' || blind=1
canary R8 'int planted(void) { return sizeof(char[ PATH_MAX + 8 ]); }' || blind=1
quiet_canary R8 'int planted(void) { return WPE_PATH_MAX + E_D_REQ_PATH_MAX; } /* PATH_MAX */' || blind=1
# Rule 9: a copy nobody has judged, more than judged, fewer, and a stale line.
canary R9 'void planted(char *a, char *b) { strcpy(a, b); }' || blind=1
canary R9 'void planted(char *a, char *b) { strcat(a, b); }' || blind=1
canary R9 'void planted(char *a, char *b) { sprintf(a, "%s", b); }' || blind=1
canary R9 'void planted(char *a, char *b) { strncpy(a, b, 3); }' || blind=1
canary R9 'void planted(char *a, char *b) {
 strcpy(a, b);
 strcpy(a, b);
}' 'COPY|we_planted.c|planted|1|text|one' || blind=1
canary R9 'void planted(char *a, char *b) { strcpy(a, b); }' 'COPY|we_planted.c|planted|2|text|two' || blind=1
canary R9 'void planted(char *a, char *b) { (void)a; (void)b; }' 'COPY|we_planted.c|planted|1|text|stale' || blind=1
canary R9 'void planted(char *a, char *b) { strcpy(a, b); }' 'COPY|we_planted.c|planted|1|path|a path' || blind=1
quiet_canary R9 'void planted(char *a, char *b) { strcpy(a, b); }' 'COPY|we_planted.c|planted|1|sized|allocated at its length' || blind=1
quiet_canary R9 'void planted(char *a, char *b) { snprintf(a, 9, "%s", b); (void)memcpy(a, b, 1); }' || blind=1
# Rule 9, the scans: a word read with %s or %[ and no width.  Seen: the call
# in a .c or a .h file (a static inline owner lives in a header), its format
# anywhere between the paren that opens the call and the one that closes it
# however many lines apart, adjacent string literals joined into one format,
# and a positional %n$s or %n$[ with no width.  NOT seen, by design: a format
# held in a macro or in a variable (fscanf(f, FMT, w), fscanf(f, fmt, w)), and
# a scan through a wrapper under another name.  A function that reads with a
# format it did not spell out has to say so in a SCAN line or be bounded by
# its buffer some other way.  The forms below that must stay clean are the
# bounded ones.
canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%s", w); }' || blind=1
canary R9 'int planted(char *s, char *w) { return sscanf(s, "%d %s", &s, w); }' || blind=1
canary R9 'int planted(char *w) { return scanf("%[^\n]", w); }' || blind=1
canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%ls", w); }' || blind=1
canary R9 'int planted(FILE *f, char *w) {
 return fscanf(f,
               "%d%s", &f, w);
}' || blind=1
PLANT_FILE=we_planted.h
canary R9 'static inline int planted(FILE *f, char *w) { return fscanf(f, "%s", w); }' || blind=1
canary R9 'static inline int planted(char *s, char *w) { return sscanf(s, "%[a-z]", w); }' || blind=1
quiet_canary R9 'static inline int planted(FILE *f, char *w) { return fscanf(f, "%255s", w); }' || blind=1
quiet_canary R9 'static inline int planted(FILE *f, char *w) { return fscanf(f, "%s", w); }' 'SCAN|we_planted.h|planted|1|the input is a line the caller measured' || blind=1
PLANT_FILE=we_planted.c
canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%d" "%s", &f, w); }' || blind=1
canary R9 'int planted(FILE *f, char *w) {
 return fscanf(f, "%d"
                  " %"
                  "s", &f, w);
}' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) {
 return fscanf(f, "%d"
                  " %"
                  "255s", &f, w);
}' || blind=1
canary R9 'int planted(FILE *f, char *w) {
 return fscanf(
     f,
     w == 0 ? w : w,
     "%s",
     w);
}' || blind=1
canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%1$s", w); }' || blind=1
canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%2$[a-z]", w); }' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%1$255s", w); }' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%1$*s", w); }' || blind=1
canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%s", w); }' 'SCAN|we_planted.c|planted|2|two' || blind=1
canary R9 'int planted(FILE *f, char *w) { return f != w; }' 'SCAN|we_planted.c|planted|1|stale' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%255s", w); }' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%d%d", &f, &f); }' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%*s%c", w); }' || blind=1
quiet_canary R9 'int planted(FILE *f, char **w) { return fscanf(f, "%ms", w); }' || blind=1
quiet_canary R9 'int planted(char *s, char *w) { return sscanf(s, "%12[a-z]", w); }' || blind=1
quiet_canary R9 'int planted(FILE *f) { return fscanf(f, "100%% %d", &f); }' || blind=1
quiet_canary R9 'int planted(FILE *f, char *w) { return fscanf(f, "%s", w); }' 'SCAN|we_planted.c|planted|1|a word the file format bounds elsewhere' || blind=1
# Rule 10: a private number or a cut that is back, and a sentence of its own.
canary R10 'void planted(void) { char a[E_LSP_PATH_MAX]; }' || blind=1
canary R10 'void planted(void) { char a[WPE_BUILD_PATH_MAX]; }' || blind=1
canary R10 'void planted(void) { char a[HOST_PATH_MAX + 1]; }' || blind=1
canary R10 'void planted(char *s) { strncpy(g_lsp_file, s, 9); }' || blind=1
canary R10 'void planted(char *s) { strncpy( ck_dir , s, 9); }' || blind=1
canary R10 'const char *planted = "The file path is longer than we can hold.";' || blind=1
canary R10 'const char *planted = "Name path is too long";' || blind=1
quiet_canary R10 'const char *planted = "The file path is too long";' 'SENTENCE|we_planted.c|the debugger'\''s own line' || blind=1
canary R10 'int planted;' 'SENTENCE|we_planted.c|stale' || blind=1
quiet_canary R10 '/* the path is longer than */ int planted;' || blind=1
# Rule 10, the owner's sentence and number: a site formatting the sentence
# itself (split over two lines, as one is), and a site printing a path number.
canary R10 'void planted(char *m, int n) { snprintf(m, n, WPE_PATH_TOO_LONG_FMT, "file", 4096); }' || blind=1
canary R10 'void planted(char *m, int n) {
 snprintf(m, n, "the %s limit is %d",
          "file", WPE_PATH_MAX);
}' || blind=1
canary R10 'void planted(char *m, int n) { sprintf(m, "%d", WPE_NAME_MAX); }' || blind=1
canary R10 'void planted(char *m, int n) { snprintf(m, n, "%d", WPE_PLATFORM_PATH_MAX); }' || blind=1
quiet_canary R10 'void planted(char *m, int n) { snprintf(m, n, "%d", WPE_PATH_MAX_HOPS); }' || blind=1
quiet_canary R10 'void planted(char *m, int n) { wpe_path_say_too_long(m, n, "file"); }' || blind=1
# Rule 11: the system's reason is said, not guessed.
canary R11b 'void planted(FENSTER *f, const char *p) { if (access(p, F_OK) != 0) e_error("X not found", 0, f->fb); }' || blind=1
canary R11b 'void planted(FENSTER *f, const char *p) { if (fopen(p, "r") == NULL) e_error("Can\'"'"'t open X", 0, f->fb); }' || blind=1
quiet_canary R11b 'void planted(char *b, const char *p) { if (access(p, F_OK) != 0) snprintf(b, 99, "Cannot open %s: %s", p, strerror(errno)); }' || blind=1
canary R11b 'void planted(FENSTER *f, const char *p, const WpeReadAsk *a, WpeReadOutcome *o) { if (wpe_read_stream_open(p, a, o) == NULL) e_error("X not found", 0, f->fb); }' || blind=1
canary R11b 'void planted(FENSTER *f, const char *p, const WpeReadAsk *a, WpeReadOutcome *o) { if (wpe_read_open(p, a, o) < 0) e_error("Can\'"'"'t open X", 0, f->fb); }' || blind=1
quiet_canary R11b 'void planted(FENSTER *f, const char *p, const WpeReadAsk *a, WpeReadOutcome *o) { if (wpe_read_text(p, a, 0, o) == NULL) e_say_read_refused(f, "file", p, o); }' || blind=1
quiet_canary R11b 'void planted(FENSTER *f) { e_error("X not found", 0, f->fb); }' || blind=1
quiet_canary R11b 'void planted(FENSTER *f, const char *p) { if (access(p, F_OK) != 0) e_error("X not found", 0, f->fb); }' 'GUESS|we_planted.c|planted|keep|the word is about a symbol' || blind=1
canary R11b 'int planted;' 'GUESS|we_planted.c|planted|keep|stale' || blind=1
canary R11b 'void planted(void) { }' 'GUESS|we_planted.c|planted' || blind=1
canary R11a 'void planted(FENSTER *f) { e_error("Project file not found", 0, f->fb); }' || blind=1
canary R11a 'void planted(FENSTER *f) { e_error("Cannot open project x", 0, f->fb); }' || blind=1
quiet_canary R11a 'void planted(FENSTER *f) { e_error("Project file not found", 0, f->fb); }' 'PROJECTWORDS|we_planted.c|planted|the reporter' || blind=1
canary R11a 'void planted(FENSTER *f) { }' 'PROJECTWORDS|we_planted.c|planted|stale' || blind=1
# Rule 10, the cuts of the containment pass: its token number and the model's
# path in 1024 bytes.
canary R10 '#define CONTAIN_TOKEN_MAX 256' || blind=1
canary R10 'void planted(const char *p) { char path[1024]; memcpy(path, p, 9); }' || blind=1
quiet_canary R10 'void planted(const char *p) { char path_name[1024]; char *path = 0; (void)p; }' || blind=1
# Rule 12: a buffer that holds a path is the size the owner gave.  K1 is the shape of
# the cut that sat in we_contain.c: a token array sized by a number of its own
# and handed to the folder resolver.
K_CONTAIN='static int contain_csv_next(const char **c, char *tok, size_t n) { return 0; }
static int contain_real(const char *dir, char *out, size_t n) { return 0; }
#define CONTAIN_TOKEN_MAX 256
void planted(const char *c)
{
 char tok[CONTAIN_TOKEN_MAX], real[WPE_PATH_SIZE];
 while (contain_csv_next(&c, tok, sizeof tok))
  contain_real(tok, real, sizeof real);
}'
canary R12 "$K_CONTAIN" 'PATHFN|we_planted.c|contain_real|0|a folder' || blind=1
# K2: the same, through the reader of folders.
canary R12 'static int contain_folder_next(const char **c, char *tok, size_t n) { return 0; }
#define CONTAIN_TOKEN_MAX 256
void planted(const char *c)
{
 char tok[CONTAIN_TOKEN_MAX];
 while (contain_folder_next(&c, tok, sizeof tok)) { }
}' 'PATHFN|we_planted.c|contain_folder_next|1|a folder' || blind=1
# K3: a working folder read into a number.
canary R12 'void planted(void) { char cwd[1024]; getcwd(cwd, sizeof cwd); }' || blind=1
# K4: a file scope array handed to a function of ours that takes a path.
canary R12 'static char d[1024];
static int wrap(const char *a, char *b) { return 0; }
void planted(void) { wrap("x", d); }' 'PATHFN|we_planted.c|wrap|1|receives a path' || blind=1
# K5: a path built into a buffer of a size of its own and looked up.
canary R12 'void planted(const char *dir) { char p[1200]; struct stat st; snprintf(p, sizeof p, "%s/x", dir); stat(p, &st); }' || blind=1
canary R12 'void planted(const char *dir) { char p[WPE_PATH_SIZE / 4]; if (wpe_path_copy(p, sizeof p, dir) < 0) return; }' || blind=1
canary R12 'void planted(void) { char a[WPE_PATH_SIZE * 2]; (void)realpath(".", a); }' || blind=1
canary R12 'void planted(const WpeReadAsk *k, WpeReadOutcome *o) { char p[1200]; FILE *fp = wpe_read_stream_open(p, k, o); (void)fp; }' || blind=1
quiet_canary R12 'void planted(const WpeReadAsk *k, WpeReadOutcome *o) { char p[WPE_PATH_SIZE]; FILE *fp = wpe_read_stream_open(p, k, o); (void)fp; }' || blind=1
canary R12 'void planted(const char *dir) { char *argv[2]; char p[512]; (void)argv; unlink(p + 1); }' || blind=1
canary R12 'int planted(a) char *a;
{
 char p[300];
 return chdir(p);
}' || blind=1
# Q1..Q4: the safe spellings stay quiet.
quiet_canary R12 'static int contain_real(const char *dir, char *out, size_t n) { return 0; }
void planted(const char *c) { char tok[WPE_PATH_SIZE]; contain_real(c, tok, sizeof tok); }' 'PATHFN|we_planted.c|contain_real|1|a folder' || blind=1
quiet_canary R12 '#define A_SIZE B_SIZE
#define B_SIZE WPE_PATH_SIZE
void planted(void) { char p[A_SIZE]; struct stat st; stat(p, &st); }' || blind=1
quiet_canary R12 '#define SUFFIX ".o"
void planted(void) { char p[WPE_PATH_SIZE + sizeof SUFFIX]; struct stat st; stat(p, &st); }' || blind=1
quiet_canary R12 'void planted(const char *dir) { char row[1400]; snprintf(row, sizeof row, "%s", dir); puts(row); }' || blind=1
quiet_canary R12 'void planted(void) { char n[WPE_NAME_SIZE]; char p[WPE_PATH_MAX]; unlink(n); unlink(p); }' || blind=1
quiet_canary R12 'void planted(void) { static char *tab[3]; char (*q)[4]; (void)tab; (void)q; }' || blind=1
quiet_canary R12 'void planted(void) { char p[1024]; unlink(p); }' 'ARRAY|we_planted.c|planted|p|a name the system itself bounds' || blind=1
# Rule 12, the cutting quoter: a path quoted by wpe_ai_shell_quote is cut.
canary R13 'void planted(const char *path) { char q[1200]; wpe_ai_shell_quote(q, sizeof q, path); }' || blind=1
canary R13 'void ai_hook_command(const char *path) { char q[1200]; wpe_ai_shell_quote(q, sizeof q, path); }' || blind=1
quiet_canary R13 'void ai_tool_quote_or(const char *arg) { char q[1200]; wpe_ai_shell_quote(q, sizeof q, arg); }' || blind=1
quiet_canary R13 'void ws_quote_whole(const char *p) { char q[1200]; wpe_ai_shell_quote(q, sizeof q, p); }' || blind=1
quiet_canary R13 'void planted(const char *p) { char q[WPE_PATH_SIZE]; (void)wpe_ai_shell_quote_whole(q, sizeof q, p); }' || blind=1
quiet_canary R13 'void planted(const char *m) { char q[600]; wpe_ai_shell_quote(q, sizeof q, m); }' 'QUOTE|we_planted.c|planted|a message' || blind=1
canary R12x 'void planted(void) { }' 'QUOTE|we_planted.c|planted|stale' || blind=1
canary R12x 'void planted(void) { }' 'ARRAY|we_planted.c|planted|p|stale' || blind=1
canary R12x 'void planted(void) { }' 'PATHFN|we_planted.c|gone|0|stale' || blind=1
canary R12x 'void planted(void) { }' 'PATHFN|we_planted.c|planted|x|malformed' || blind=1

if [ "$SELFTEST" -eq 1 ]; then
  # Only the canaries: every rule must name its planted offender and stay
  # quiet on the safe spellings, whatever the tree holds.
  [ "$blind" -eq 0 ] || exit $BAD
  echo "check-path-buffers: selftest ok"
  exit 0
fi
if [ ! -f "$ALLOW" ]; then
  echo "check-path-buffers: no allow file at $ALLOW"
  exit $BAD
fi
scan_tree "$SRCDIR" "$WORK/problems" "$ALLOW"
if [ "$LIST" -eq 1 ]; then
  awk -F'|' '{ print "COPY|" $1 "|" $2 "|" $3 "|sized|reason" }' "$WORK/last-copies"
  awk -F'|' '$1 == "G" { print "GUESS|" $2 "|" $3 "|keep|reason" }' "$WORK/last-words"
  awk -F'|' '{ print "SCAN|" $1 "|" $2 "|" $3 "|reason" }' "$WORK/last-scans"
  exit 0
fi
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
