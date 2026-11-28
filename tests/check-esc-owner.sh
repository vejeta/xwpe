#!/bin/sh
# check-esc-owner.sh -- the key that ends a running activity has one owner.
#
# Esc is promised all over the product: a build's waiting line, a starting
# debugger's, a language server's answer, the assistant's pane and the chat's
# input row all say "Esc ...".  Which of them one Esc ends is the table in
# we_cancel.c, and the editor's key loop asks that table once.  A second test
# of the key in the loop, a cancel called from somewhere else, a promise with
# no claim behind it, or a claim nobody promises is a drift the user reads as
# "Esc did nothing" or "Esc ended the wrong thing".
#
#   check-esc-owner.sh [--srcdir DIR] [--allow FILE] [--list] [--selftest]
#
# Clauses (K7, the lines a claim says from any window, lands with the pass
# that gives the table its decided order; each clause is added here with its
# own canaries):
#
#  K1 key loop.  In the body of e_eingabe (we_edit.c): WPE_ESC appears once,
#     in the loop head; CtrlC appears nowhere; e_cancel_key( is called exactly
#     once, on a line after the calls of e_debug_console_input and
#     e_lsp_on_edit and before the call of e_ai_chat_key; its third argument
#     is the local that e_key_is_mouse( sets where the mouse is read; and
#     e_key_cancels_answer is nowhere in the tree.
#
#  K2 cancels.  Each cancel function the table names, and each function such
#     an adapter of the owner calls, is called only from we_cancel.c and from
#     the source file that defines it: a cancel called from a third file is a
#     second owner of the same end.  A name the table or an adapter holds that
#     is not defined anywhere fails (the scan is blind).
#
#  K3 the pane.  e_ai_chat_key (we_ai_ui.c) holds no WPE_ESC: the pane's
#     close is the table's claim, so no Esc reaches that handler.
#
#  K4 promises (default deny, every string).  Comments are stripped, then
#     every string literal of the product that holds the word Esc, in any
#     case ("ESC cancels" is a promise too), is
#     attributed to the function that holds it, or to the macro that names it
#     (most promises are file-scope #defines).  Each holder needs one allow
#     entry, with the number of such strings it holds: more is a new promise
#     nobody has read, fewer is a stale line.  An entry naming a holder that
#     is not in the tree fails, and a claim| entry whose argument is not a
#     row of the table fails.
#
#  K5 mentions (default deny, every function).  Every function of the product
#     that mentions WPE_ESC or CtrlC, or that compares a key with the byte
#     (27, 033, 0x1b, '\033', '\x1b', either side of the test, or a macro,
#     constant or enumerator of the product whose value is that byte), has one
#     allow entry with a fingerprint: the number of such
#     mentions in its body.  A changed count is red until the entry is read
#     again, so a result| function that gains a key test is asked about.  The
#     script does not tell a key from a result by syntax (one line of the
#     assistant's boxes does both); the kind is the reader's judgement, and a
#     function whose only mention is a numeric escape code must be word|.
#
#  K6 the table.  Every row has a what, a busy, a cancel and Esc in its keys;
#     one row alone has the Ctrl-C bit; a front_only row has an in_front and
#     is named in the allow file; and each row sits under the #ifdef of the
#     module that owns its predicates, so a build without that feature has no
#     claim it could not answer.  A row the allow file names that is not in
#     the table fails.
#
# Allow file (tests/esc-owner.allow), one line per excused place:
#
#   K4|<kind>|<argument>|<file>|<holder>|<count>|<reason>
#   K5|<kind>|<file>|<function>|<count>|<reason>
#   K6|front|<what>|<reason>
#
# K4 kinds: claim|<the row's what>, loop|<a key loop of its own>, word|<a
# key's name or a report>.  K5 kinds: loop|, result|, claim|, word|.  A
# function or symbol an entry names that is not in the tree fails: the scan is
# blind, so a stale entry is red.
#
# --list prints what the scan found as allow lines to start from.  --selftest
# runs only the canaries: a clean fixture tree that must pass, and a fixture
# per bypass that must be named by its clause, so a scan gone blind fails.
# Without either the script scans the sources and then runs the canaries.
#
# Environment (set by `make check`): WPE_FEATURE_SOURCES, srcdir.  Without
# WPE_FEATURE_SOURCES the guard half of K6 cannot tell a feature's sources
# from the editor's own: it says SKIP and the script exits 77, never 0 with a
# half unrun.
#
# Exit: 0 clean, 1 offenders or a blind scanner, 77 a half that could not run.

SRCDIR=${srcdir:-.}
BAD=1
SKIP=77
ALLOW=
SELFTEST_ONLY=
LIST_ONLY=
HERE=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --list) LIST_ONLY=1 ;;
    --selftest) SELFTEST_ONLY=1 ;;
    -*) echo "check-esc-owner: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/esc-owner.allow

. "$HERE/workdir.sh"
work_dir esc-owner $BAD
PROBLEMS=$WORK/problems
: > "$PROBLEMS"

problem() { echo "$*" >> "$PROBLEMS"; }

# What the scan could not do: said at the end with status 77, never passed.
SKIPPED=

BLANK_C=$(cat "$HERE/blank-c.awk")

# The functions the scans share (awk text, put in front of each program after
# tests/blank-c.awk).  A program reads one file into code[1..total] and calls
# index_functions(); holder[1..total] then names the function or the macro
# each line belongs to ("-" for file scope outside a macro).
AWK_COMMON='
# The name of the function whose header is LINE.
function header_name(line,   head, parts, k) {
  head = line
  sub(/\(.*/, "", head)
  sub(/[^A-Za-z0-9_]+$/, "", head)
  k = split(head, parts, /[^A-Za-z0-9_]+/)
  return parts[k]
}

# 1 when LINE opens a function: a col-0 header that is no declaration.
function is_header(line) {
  return (line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ &&
          line !~ /^(typedef|struct|union|enum|extern|static const)[ \t]/)
}

# The function each line is in: the last col-0 header seen.  defined[name]
# and header_line[name] hold the functions the file defines.
function index_functions(   i, fname, line) {
  fname = ""
  for (i = 1; i <= total; i++) {
    line = code[i]
    if (is_header(line)) {
      fname = header_name(line)
      defined[fname] = 1
      header_line[fname] = i
    }
    func_of[i] = fname
  }
}

# How many times the identifier NAME stands alone in TEXT.
function count_word(text, name,   n, at, rest, before, after, consumed, len) {
  n = 0; consumed = 0; rest = text; len = length(name)
  while ((at = index(rest, name)) > 0) {
    before = (consumed + at) > 1 ? substr(text, consumed + at - 1, 1) : ""
    after = substr(rest, at + len)
    if (before !~ /[A-Za-z0-9_]/ && after !~ /^[A-Za-z0-9_]/) n++
    consumed += at + len - 1
    rest = substr(rest, at + len)
  }
  return n
}

# How many comparisons with a numeric escape code are in TEXT: the byte an
# Esc arrives as, spelled as a number instead of as the key it is.
function count_numeric_esc(text,   n_num, k, n_lit, lit, lits, q, bs) {
  q = "\047"; bs = "\\\\"
  # decimal, hex, octal, and the character spellings of the byte
  n_lit = split("27@0x1[bB]@0+33@" q bs "0?33" q "@" q bs "x1[bB]" q, lits, "@")
  n_num = 0
  for (k = 1; k <= n_lit; k++) {
    lit = lits[k]
    if (text ~ ("(==|!=)[ \t]*" lit "($|[^0-9A-Za-z_])")) n_num++
    if (text ~ ("case[ \t]+" lit "[ \t]*:")) n_num++
    if (text ~ ("(^|[^0-9A-Za-z_.])" lit "[ \t]*(==|!=)")) n_num++
  }
  return n_num
}

# How many comparisons of TEXT are with a name that stands for the Esc byte
# (a macro or constant whose value is the byte, found by scan_aliases): the
# same test as a number, behind a name.  ALIASES is the names, one per space.
function count_alias_compare(text, names,   n_cmp, k, n_name, nm, list) {
  n_cmp = 0
  n_name = split(names, list, " ")
  for (k = 1; k <= n_name; k++) {
    nm = list[k]
    if (index(text, nm) == 0) continue
    if (text ~ ("(==|!=)[ \t]*\\(?[ \t]*" nm "($|[^A-Za-z0-9_])")) n_cmp++
    if (text ~ ("case[ \t]+" nm "[ \t]*:")) n_cmp++
    if (text ~ ("(^|[^A-Za-z0-9_])" nm "[ \t]*\\)?[ \t]*(==|!=)")) n_cmp++
  }
  return n_cmp
}

# The string literals of LINE, one per output record, through the callback
# each_literal(text).  A literal does not cross a line (tests/blank-c.awk).
function literals_of(line,   out, n, i, c, cur, instr) {
  out = 0; n = length(line); i = 1; instr = 0; cur = ""
  while (i <= n) {
    c = substr(line, i, 1)
    if (instr) {
      if (c == "\\") { cur = cur substr(line, i, 2); i += 2; continue }
      if (c == "\"") { instr = 0; each_literal(cur); i++; continue }
      cur = cur c; i++; continue
    }
    if (c == "\"") { instr = 1; cur = ""; i++; continue }
    if (c == "\047") {                      # a character literal is no string
      i++
      while (i <= n && substr(line, i, 1) != "\047") {
        if (substr(line, i, 1) == "\\") i++
        i++
      }
      i++; continue
    }
    i++
  }
}

# The statements of the function FNM: a line that holds ";", "{" or "}" ends
# one, so a call split over lines is one statement.
function statements_of(fnm,   i, stmt, start, line, depth, opened, nopen, nclose) {
  ns = 0; stmt = ""; start = 0; depth = 0; opened = 0
  for (i = header_line[fnm]; i <= total && func_of[i] == fnm; i++) {
    line = code[i]
    if (!opened) {
      if (index(line, "{") == 0) continue
      opened = 1
      line = substr(line, index(line, "{"))
    }
    nopen = gsub(/\{/, "{", line); nclose = gsub(/\}/, "}", line)
    depth += nopen - nclose
    if (stmt == "") start = i
    stmt = stmt " " line
    if (line ~ /[;{}]/) { ns++; text[ns] = stmt; sline[ns] = start; stmt = ""; start = 0 }
    if (depth <= 0 && opened) break
  }
}
'

# The product sources: every editor source and header but the two key tables
# (they hold a key's number, which is what the table is for).
product_files() {
  (cd "$1" && ls we_*.c We*.c *.h 2> /dev/null |
    grep -v -e '^keys\.h$' -e '^unixkeys\.h$')
}

# ---- the tree scan --------------------------------------------------------
#
# scan_tree DIR prints one record per line:
#   S|file|holder|count|line|sample     a holder of strings that name Esc (K4)
#   M|file|function|count|numeric|reads|result|byte   a mention of a key (K5)
#   D|file|function                     a function the file defines (K2, K6)

SCAN_TREE_AWK='
function each_literal(t) {
  if (t !~ /(^|[^A-Za-z])[Ee][Ss][Cc]([^A-Za-z]|$)/) return
  gsub(/[|	]/, "/", t)          # a record field holds no separator of its own
  strings++
  if (first_line == 0) { first_line = lineno; sample = substr(t, 1, 60) }
}

BEGIN { blank_incom = 0 }
FNR == 1 {
  if (total > 0) finish_file()
  blank_incom = 0; total = 0; delete code; delete code1
}
{
  save = blank_incom
  code1[FNR] = blank_c($0, 1)
  blank_incom = save
  code[FNR] = blank_c($0, 0)
  total = FNR
  file = FILENAME
}
END { finish_file() }

function finish_file(   i, line, fn, in_def, def_name, mentions, numeric,
                        aliased, reads, result_cmp, byte_esc, str_n, key, hold) {
  # who holds each line: a macro body, a function body, or file scope
  fn = ""; in_def = 0; def_name = ""
  for (i = 1; i <= total; i++) {
    line = code1[i]
    if (in_def) {
      holder[i] = def_name
      if (line !~ /\\[ \t]*$/) in_def = 0
      continue
    }
    if (line ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/) {
      def_name = line
      sub(/^[ \t]*#[ \t]*define[ \t]+/, "", def_name)
      sub(/[^A-Za-z0-9_].*$/, "", def_name)
      holder[i] = def_name
      if (line ~ /\\[ \t]*$/) in_def = 1
      continue
    }
    if (is_header(code[i])) {
      fn = header_name(code[i])
      defined[fn] = 1
      printf "D|%s|%s\n", file, fn
    }
    holder[i] = (fn == "" ? "-" : fn)
  }

  # K4: the strings that name Esc, per holder
  delete str_count; delete str_line; delete str_sample
  for (i = 1; i <= total; i++) {
    strings = 0; first_line = 0; sample = ""; lineno = i
    literals_of(code1[i])
    if (strings == 0) continue
    hold = holder[i]
    str_count[hold] += strings
    if (!(hold in str_line)) { str_line[hold] = first_line; str_sample[hold] = sample }
  }
  for (hold in str_count)
    printf "S|%s|%s|%d|%d|%s\n", file, hold, str_count[hold], str_line[hold],
           str_sample[hold]

  # K5: the functions that mention a key, and what their body looks like
  delete m_count; delete m_numeric; delete m_reads; delete m_result; delete m_byte
  for (i = 1; i <= total; i++) {
    line = code[i]
    mentions = count_word(line, "WPE_ESC") + count_word(line, "CtrlC")
    aliased = count_alias_compare(code[i], aliases)
    numeric = count_numeric_esc(code1[i]) + aliased
    if (mentions + numeric == 0) continue
    hold = holder[i]
    m_count[hold] += mentions + numeric
    m_numeric[hold] += numeric
    if (line ~ /(e_getch|wpe_paint_ask_key|wpe_paint_modal_getch|WpeGetch|wpe_field_take)[ \t]*\(/)
      m_reads[hold] = 1
    if (line ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*\([^;]*\)[ \t]*(==|!=)[ \t]*WPE_ESC/ ||
        line ~ /return[ \t]*\(?[ \t]*WPE_ESC/)
      m_result[hold] = 1
    if (aliased || code1[i] ~ /'"'"'\\033'"'"'|'"'"'\\x1[bB]'"'"'|0x1[bB]|(^|[^0-9A-Za-z_])0+33/)
      m_byte[hold] = 1
  }
  for (hold in m_count)
    printf "M|%s|%s|%d|%d|%d|%d|%d\n", file, hold, m_count[hold],
           m_numeric[hold] + 0, m_reads[hold] + 0, m_result[hold] + 0,
           m_byte[hold] + 0

  delete holder; delete defined; delete header_line; delete func_of
  total = 0
}
'

# ---- the names that stand for the Esc byte ---------------------------------
#
# scan_aliases DIR prints, on one line, every macro or constant of the product
# whose value is the Esc byte (27, 033, 0x1b, '\033', '\x1b', WPE_ESC, or
# another such name): a key loop that tests the key against one of them is the
# same test as a number, and K5 counts it so.

SCAN_ALIAS_AWK='
function esc_value_regex(   q, bs) {
  q = "\047"; bs = "\\\\"
  return "(27|0x1[bB]|0+33|" q bs "0?33" q "|" q bs "x1[bB]" q "|WPE_ESC)"
}

function add_alias(name) {
  if (name != "" && !(name in is_alias)) { is_alias[name] = 1; changed = 1 }
}

BEGIN { blank_incom = 0; val_re = esc_value_regex() }
FNR == 1 { blank_incom = 0 }
{
  line = blank_c($0, 1)
  gsub(/[ \t]+$/, "", line)
  if (line ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+/) {
    name = line
    sub(/^[ \t]*#[ \t]*define[ \t]+/, "", name)
    rest = name
    sub(/[^A-Za-z0-9_].*$/, "", name)
    sub(/^[A-Za-z0-9_]+[ \t]+/, "", rest)
    gsub(/^[ \t(]+|[ \t)]+$/, "", rest)
    if (rest ~ ("^" val_re "$")) add_alias(name)
    else if (rest ~ /^[A-Za-z_][A-Za-z0-9_]*$/) defined_as[name] = rest
    next
  }
  if (line ~ /(^|[^A-Za-z0-9_])const([^A-Za-z0-9_]|$)/ &&
      match(line, "[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*" val_re "[ \t]*[;,]")) {
    name = substr(line, RSTART, RLENGTH)
    sub(/[ \t]*=.*$/, "", name)
    add_alias(name)
    next
  }
  # an enumerator: an upper-case name set to the byte
  rest = line
  while (match(rest, "(^|[{,])[ \t]*[A-Z][A-Z0-9_]*[ \t]*=[ \t]*" val_re "[ \t]*([,}]|$)")) {
    name = substr(rest, RSTART, RLENGTH)
    sub(/^[{,]?[ \t]*/, "", name)
    sub(/[ \t]*=.*$/, "", name)
    add_alias(name)
    rest = substr(rest, RSTART + RLENGTH)
  }
}
END {
  changed = 1
  while (changed) {
    changed = 0
    for (name in defined_as)
      if (defined_as[name] in is_alias) { add_alias(name); delete defined_as[name] }
  }
  out = ""
  for (name in is_alias) if (name != "WPE_ESC") out = out " " name
  print substr(out, 2)
}
'

scan_aliases() {
  dir=$1
  files=
  for f in $(product_files "$dir"); do
    [ -f "$dir/$f" ] && files="$files $dir/$f"
  done
  # shellcheck disable=SC2086
  awk "$BLANK_C$SCAN_ALIAS_AWK" $files | tr '\n' ' '
}

scan_tree() {
  dir=$1
  aliases=$(scan_aliases "$dir")
  for f in $(product_files "$dir"); do
    [ -f "$dir/$f" ] || continue
    # run in the tree, so a record names its file as the allow file does
    (cd "$dir" && awk -v aliases="$aliases" "$BLANK_C$AWK_COMMON$SCAN_TREE_AWK" "$f")
  done
}

# ---- the table scan -------------------------------------------------------
#
# scan_table DIR prints one record per row of the table in we_cancel.c:
#   R|n|guard|what|keys|row_wait|front_only|busy|ending|in_front|cancel
# and one per call inside a function of that file:
#   F|function|callee

SCAN_TABLE_AWK='
BEGIN { blank_incom = 0 }
{
  save = blank_incom
  code1[FNR] = blank_c($0, 1)
  blank_incom = save
  code[FNR] = blank_c($0, 0)
  total = FNR
}
END {
  index_functions()

  # the calls of each function of the owner (K2 follows an adapter through them)
  for (fn in defined) {
    statements_of(fn)
    for (k = 1; k <= ns; k++) {
      rest = text[k]
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
        name = substr(rest, RSTART, RLENGTH)
        sub(/[ \t]*\($/, "", name)
        rest = substr(rest, RSTART + RLENGTH)
        if (name ~ /^(if|while|for|switch|sizeof|return|defined)$/) continue
        printf "F|%s|%s\n", fn, name
      }
    }
  }

  # the table
  start = 0
  for (i = 1; i <= total; i++)
    if (code1[i] ~ /wpe_cancel_claims\[[ \t]*\][ \t]*=/) { start = i; break }
  if (start == 0) {
    print "R|0|MISSING|||||||||"
    exit
  }
  depth = 0; guard = "none"; rows = 0; rowtext = ""; rowguard = ""
  for (i = start; i <= total; i++) {
    line = code1[i]
    if (line ~ /^[ \t]*#[ \t]*(ifdef|ifndef|if)[ \t]/) {
      g = line
      sub(/^[ \t]*#[ \t]*(ifdef|ifndef|if)[ \t]+/, "", g)
      sub(/[^A-Za-z0-9_].*$/, "", g)
      depth++
      stack[depth] = g
      guard = g
      continue
    }
    if (line ~ /^[ \t]*#[ \t]*(else|elif)/) continue
    if (line ~ /^[ \t]*#[ \t]*endif/) {
      if (depth > 0) depth--
      guard = (depth > 0 ? stack[depth] : "none")
      continue
    }
    if (i == start) {
      sub(/^[^=]*=[ \t]*\{?/, "", line)
    }
    if (line ~ /^[ \t]*$/) continue
    if (rowtext == "" && line ~ /^\}[ \t]*;[ \t]*$/) break   # the table ends
    if (rowtext == "") rowguard = guard
    rowtext = rowtext " " line
    if (index(line, "}") > 0) {
      rows++
      emit_row(rowtext, rowguard, rows)
      rowtext = ""
    }
  }
}

function emit_row(t, guard, n,   what, rest, np, f, k) {
  sub(/^[^{]*\{/, "", t)
  sub(/\}[ \t]*,?[ \t]*$/, "", t)
  if (match(t, /"[^"]*"/) == 0) {
    printf "R|%d|%s|MISSING|||||||\n", n, guard
    return
  }
  what = substr(t, RSTART + 1, RLENGTH - 2)
  rest = substr(t, RSTART + RLENGTH)
  sub(/^[ \t]*,[ \t]*/, "", rest)
  np = split(rest, f, ",")
  for (k = 1; k <= 7; k++) {
    if (k > np) f[k] = ""
    gsub(/[ \t]+/, "", f[k])
    gsub(/\|/, "+", f[k])       # a record field holds no separator of its own
  }
  printf "R|%d|%s|%s|%s|%s|%s|%s|%s|%s|%s\n", n, guard, what, f[1], f[2],
         f[3], f[4], f[5], f[6], f[7]
}
'

scan_table() {
  dir=$1
  if [ ! -f "$dir/we_cancel.c" ]; then
    echo "R|0|MISSING|||||||||"
    return
  fi
  awk "$BLANK_C$AWK_COMMON$SCAN_TABLE_AWK" "$dir/we_cancel.c"
}

# ---- K1: the key loop -----------------------------------------------------

SCAN_EDIT_AWK='
BEGIN { blank_incom = 0 }
{ code[FNR] = blank_c($0, 0); total = FNR }
END {
  index_functions()
  if (!("e_eingabe" in defined)) {
    print "K1: we_edit.c: e_eingabe is not in the file (the scan is blind)"
    exit
  }
  if (!("e_key_is_mouse" in defined))
    print "K1: we_edit.c: e_key_is_mouse is not in the file (the scan is blind)"

  esc_n = 0; esc_line = 0; ctrlc_n = 0; ctrlc_line = 0
  cancel_n = 0; cancel_line = 0; console_line = 0; lsp_line = 0; chat_line = 0
  head_line = 0
  for (i = header_line["e_eingabe"]; i <= total && func_of[i] == "e_eingabe"; i++) {
    line = code[i]
    n = count_word(line, "WPE_ESC")
    if (n) { esc_n += n; if (!esc_line) esc_line = i }
    m = count_word(line, "CtrlC")
    if (m) { ctrlc_n += m; if (!ctrlc_line) ctrlc_line = i }
    if (line ~ /while[ \t]*\(/ && !head_line) head_line = i
    if (index(line, "e_cancel_key") && line ~ /e_cancel_key[ \t]*\(/) {
      cancel_n++
      if (!cancel_line) cancel_line = i
    }
    if (line ~ /e_debug_console_input[ \t]*\(/ && !console_line) console_line = i
    if (line ~ /e_lsp_on_edit[ \t]*\(/ && !lsp_line) lsp_line = i
    if (line ~ /e_ai_chat_key[ \t]*\(/ && !chat_line) chat_line = i
    if (line ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*e_key_is_mouse[ \t]*\(/) {
      click_var = line
      sub(/^[ \t]*/, "", click_var)
      sub(/[ \t]*=[ \t]*e_key_is_mouse.*$/, "", click_var)
    }
  }

  if (esc_n != 1)
    printf "K1: we_edit.c:%d: WPE_ESC appears %d times in e_eingabe: once, in the loop head\n",
           esc_line, esc_n
  else if (head_line && esc_line != head_line)
    printf "K1: we_edit.c:%d: WPE_ESC in e_eingabe is not in the loop head\n", esc_line
  if (ctrlc_n)
    printf "K1: we_edit.c:%d: CtrlC appears in e_eingabe: the table alone says which claim takes it\n",
           ctrlc_line
  if (!console_line)
    print "K1: we_edit.c: e_debug_console_input is not called in e_eingabe (the scan is blind)"
  if (!lsp_line)
    print "K1: we_edit.c: e_lsp_on_edit is not called in e_eingabe (the scan is blind)"
  if (!chat_line)
    print "K1: we_edit.c: e_ai_chat_key is not called in e_eingabe (the scan is blind)"
  if (cancel_n == 0)
    print "K1: we_edit.c: e_cancel_key is not called in e_eingabe: the chain has no owner"
  if (cancel_n > 1)
    printf "K1: we_edit.c:%d: e_cancel_key is called %d times in e_eingabe: once, where the chain started\n",
           cancel_line, cancel_n
  if (cancel_line && console_line && cancel_line < console_line)
    printf "K1: we_edit.c:%d: e_cancel_key is called above e_debug_console_input: typed text for a running program comes first\n",
           cancel_line
  if (cancel_line && lsp_line && cancel_line < lsp_line)
    printf "K1: we_edit.c:%d: e_cancel_key is called above e_lsp_on_edit\n", cancel_line
  if (cancel_line && chat_line && cancel_line > chat_line)
    printf "K1: we_edit.c:%d: e_cancel_key is called below e_ai_chat_key: the pane would take the key first\n",
           cancel_line

  # the third argument is the click local
  statements_of("e_eingabe")
  arg = ""
  for (k = 1; k <= ns; k++) {
    if (text[k] !~ /e_cancel_key[ \t]*\(/) continue
    stmt = text[k]
    sub(/^.*e_cancel_key[ \t]*\(/, "", stmt)
    sub(/\).*/, "", stmt)
    np = split(stmt, parts, ",")
    if (np != 3) {
      printf "K1: we_edit.c:%d: e_cancel_key takes %d arguments: the window, the key and whether it is a click\n",
             sline[k], np
      continue
    }
    gsub(/[ \t]/, "", parts[3])
    arg = parts[3]
  }
  if (arg == "")
    print "K1: we_edit.c: the call of e_cancel_key was not read (the scan is blind)"
  else if (click_var == "")
    print "K1: we_edit.c: no local is set from e_key_is_mouse: a click on the bottom row is not told from the key"
  else if (arg != click_var)
    printf "K1: we_edit.c: the third argument of e_cancel_key is %s, not the click local %s\n",
           arg, click_var
}
'

scan_edit() {
  dir=$1
  if [ ! -f "$dir/we_edit.c" ]; then
    echo "K1: we_edit.c not found under $dir (the scan is blind)"
    return
  fi
  awk "$BLANK_C$AWK_COMMON$SCAN_EDIT_AWK" "$dir/we_edit.c"
}

# ---- K3: the chat pane ----------------------------------------------------

SCAN_PANE_AWK='
BEGIN { blank_incom = 0 }
{ code[FNR] = blank_c($0, 0); total = FNR }
END {
  index_functions()
  if (!("e_ai_chat_key" in defined)) {
    print "K3: we_ai_ui.c: e_ai_chat_key is not in the file (the scan is blind)"
    exit
  }
  n = 0; at = 0
  for (i = header_line["e_ai_chat_key"]; i <= total && func_of[i] == "e_ai_chat_key"; i++) {
    k = count_word(code[i], "WPE_ESC")
    if (k) { n += k; if (!at) at = i }
  }
  if (n)
    printf "K3: we_ai_ui.c:%d: e_ai_chat_key holds WPE_ESC %d times: the pane close is the table claim\n",
           at, n
}
'

scan_pane() {
  dir=$1
  if [ ! -f "$dir/we_ai_ui.c" ]; then
    echo "K3: we_ai_ui.c not found under $dir (the scan is blind)"
    return
  fi
  awk "$BLANK_C$AWK_COMMON$SCAN_PANE_AWK" "$dir/we_ai_ui.c"
}

# ---- the clauses that read the records and the allow file -----------------

# The allow lines of one clause, without their comments.
allow_of() {
  clause=$1; allow=$2
  if [ ! -f "$allow" ]; then
    problem "no allow file at $allow"
    return
  fi
  grep "^$clause|" "$allow"
}

# K2: a cancel of the table is called only from we_cancel.c and its own file.
k2_clause() {
  dir=$1
  # the names: the table's cancel entries, and what an adapter of the owner
  # calls (the module's own cancel).
  awk -F'|' '$1 == "R" && $2 != "0" && $11 != "" && $11 != "NULL" { print $11 }' \
    "$WORK/tab.out" | sort -u > "$WORK/k2.table"
  awk -F'|' '$1 == "F" { print $2 "|" $3 }' "$WORK/rec_calls.out" |
    sort -u > "$WORK/k2.calls"
  : > "$WORK/k2.names"
  while read -r name; do
    [ -n "$name" ] || continue
    if grep -q "^$name|" "$WORK/k2.calls" 2> /dev/null; then
      # an adapter of the owner: the module's cancel is what it calls
      awk -F'|' -v n="$name" '$1 == n { print $2 }' "$WORK/k2.calls" >> "$WORK/k2.names"
    else
      echo "$name" >> "$WORK/k2.names"
    fi
  done < "$WORK/k2.table"
  sort -u "$WORK/k2.names" > "$WORK/k2.list"

  # a name nothing defines: the scan is blind
  while read -r name; do
    [ -n "$name" ] || continue
    awk -F'|' -v n="$name" '$1 == "D" && $3 == n { found = 1 }
         END { exit found ? 0 : 1 }' "$WORK/rec.out" ||
      problem "K2: $name is named by the owner but defined in no product source (the scan is blind)"
  done < "$WORK/k2.list"

  # every call of every name, in one pass over each source
  names=$(awk 'NR > 1 { printf " " } { printf "%s", $0 }' "$WORK/k2.list")
  : > "$WORK/k2.hits"
  for f in $(product_files "$dir"); do
    [ -f "$dir/$f" ] || continue
    case $f in
      *.h | we_cancel.c) continue ;;
    esac
    awk -v names="$names" -v base="$f" "$BLANK_C"'
      BEGIN { blank_incom = 0; n = split(names, want, " ") }
      {
        line = blank_c($0, 0)
        # the name a col-0 line defines or declares ("type name(") is no call
        def_name = ""
        if (line ~ /^[A-Za-z_]/ && match(line, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
          head = substr(line, 1, RSTART - 1)
          if (head ~ /[A-Za-z_*]/) {
            def_name = substr(line, RSTART, RLENGTH)
            sub(/[ \t]*\($/, "", def_name)
          }
        }
        for (k = 1; k <= n; k++) {
          if (want[k] == def_name) continue
          if (index(line, want[k]) == 0) continue
          len = length(want[k]); rest = line
          while ((at = index(rest, want[k])) > 0) {
            if (substr(rest, at + len) ~ /^[ \t]*\(/) hits[want[k]]++
            rest = substr(rest, at + len)
          }
        }
      }
      END { for (k in hits) printf "%s|%s|%d\n", base, k, hits[k] }' \
      "$dir/$f" >> "$WORK/k2.hits"
  done

  while read -r hit; do
    [ -n "$hit" ] || continue
    file=$(echo "$hit" | cut -d'|' -f1)
    name=$(echo "$hit" | cut -d'|' -f2)
    cnt=$(echo "$hit" | cut -d'|' -f3)
    own=$(awk -F'|' -v n="$name" '$1 == "D" && $3 == n { print $2; exit }' "$WORK/rec.out")
    [ "$file" = "$own" ] && continue
    problem "K2: $file calls $name $cnt times: a cancel belongs to we_cancel.c and to $own alone"
  done < "$WORK/k2.hits"
}

# K4: every string that names Esc has an allow entry, and every entry a string.
k4_clause() {
  allow=$1
  awk -F'|' '$1 == "S" { printf "%s|%s\t%s\t%s\t%s\n", $2, $3, $4, $5, $6 }' \
    "$WORK/rec.out" | sort > "$WORK/k4.have"
  allow_of K4 "$allow" | sort > "$WORK/k4.want"
  # a claim entry must name a row of the table
  awk -F'|' '$1 == "R" && $2 != "0" { print $4 }' "$WORK/tab.out" |
    sort -u > "$WORK/k4.whats"
  while IFS='|' read -r clause kind arg file hold want reason; do
    [ -n "$clause" ] || continue
    rec=$(awk -F'\t' -v k="$file|$hold" '$1 == k { print; exit }' "$WORK/k4.have")
    if [ -z "$rec" ]; then
      problem "K4: the allow entry $kind|$arg|$file|$hold names a holder with no string naming Esc (stale)"
      continue
    fi
    have=$(echo "$rec" | cut -f2)
    ln=$(echo "$rec" | cut -f3)
    if [ "$have" != "$want" ]; then
      problem "K4: $file:$ln: $hold holds $have strings naming Esc, the entry says $want"
    fi
    case $kind in
      (claim)
        grep -qxF "$arg" "$WORK/k4.whats" ||
          problem "K4: the allow entry for $file:$hold names the claim $arg, which the table does not have"
        ;;
      (loop)
        grep -qF "|$arg" "$WORK/k4.loops" 2> /dev/null ||
          problem "K4: the allow entry for $file:$hold names the loop $arg, which is not in the tree"
        ;;
      (word) : ;;
      (*) problem "K4: the allow entry for $file:$hold has the kind $kind, not claim, loop or word" ;;
    esac
  done < "$WORK/k4.want"
  # a holder with no entry
  cut -f1 "$WORK/k4.have" | while read -r key; do
    grep -qF "|$key|" "$WORK/k4.want" && continue
    file=$(echo "$key" | cut -d'|' -f1)
    hold=$(echo "$key" | cut -d'|' -f2)
    rec=$(awk -F'\t' -v k="$key" '$1 == k { print; exit }' "$WORK/k4.have")
    cnt=$(echo "$rec" | cut -f2)
    ln=$(echo "$rec" | cut -f3)
    sample=$(echo "$rec" | cut -f4)
    problem "K4: $file:$ln: $hold says \"$sample\" and has no allow entry (want: K4|claim|<what>|$file|$hold|$cnt|<reason>)"
  done
}

# K5: every function that mentions a key has an entry, with its exact count.
k5_clause() {
  allow=$1
  awk -F'|' '$1 == "M" { printf "%s|%s\t%s\t%s\t%s\t%s\t%s\n", $2, $3, $4, $5, $6,
                          $7, $8 }' "$WORK/rec.out" | sort > "$WORK/k5.have"
  allow_of K5 "$allow" | sort > "$WORK/k5.want"
  awk -F'|' '{ print $3 "|" $4 }' "$WORK/k5.want" > "$WORK/k5.want.keys"
  while IFS='|' read -r clause kind file fn want reason; do
    [ -n "$clause" ] || continue
    rec=$(awk -F'\t' -v k="$file|$fn" '$1 == k { print; exit }' "$WORK/k5.have")
    if [ -z "$rec" ]; then
      problem "K5: the allow entry $kind|$file|$fn|$want names a function that mentions no key (stale)"
      continue
    fi
    have=$(echo "$rec" | cut -f2)
    numeric=$(echo "$rec" | cut -f3)
    if [ "$have" != "$want" ]; then
      problem "K5: $file: $fn mentions a key $have times, the entry says $want: read the function again"
    fi
    case $kind in
      (loop|result|claim|word) : ;;
      (*) problem "K5: the allow entry for $file:$fn has the kind $kind, not loop, result, claim or word" ;;
    esac
    if [ "$numeric" != "0" ] && [ "$kind" != "word" ] && [ "$have" = "$numeric" ]; then
      problem "K5: $file: $fn compares a key with a numeric escape code: the entry must be word|"
    fi
  done < "$WORK/k5.want"
  cut -f1 "$WORK/k5.have" | while read -r key; do
    grep -qxF "$key" "$WORK/k5.want.keys" && continue
    file=$(echo "$key" | cut -d'|' -f1)
    fn=$(echo "$key" | cut -d'|' -f2)
    rec=$(awk -F'\t' -v k="$key" '$1 == k { print; exit }' "$WORK/k5.have")
    cnt=$(echo "$rec" | cut -f2)
    kind=$(k5_kind "$file" $(echo "$rec" | cut -f2,3,4,5,6 | tr '\t' ' '))
    problem "K5: $file: $fn mentions a key $cnt times and has no allow entry (want: K5|$kind|$file|$fn|$cnt|<reason>)"
  done
}

# k5_kind FILE COUNT NUMERIC READS RESULT BYTE - the kind --list guesses for a
# K5 record: the evidence in the body decides, and the reader of --list judges
# the guess before it becomes an entry.
k5_kind() {
  file=$1; total_n=$2; numeric=$3; reads=$4; result_cmp=$5; byte=$6
  if [ "$file" = "we_cancel.c" ]; then echo "claim"; return; fi
  if [ "$numeric" = "$total_n" ]; then echo "word"; return; fi
  if [ "$reads" != "0" ]; then echo "loop"; return; fi
  if [ "$byte" != "0" ] && [ "$result_cmp" = "0" ]; then echo "word"; return; fi
  echo "result"
}

# K6: the shape of the table.
k6_clause() {
  allow=$1
  if grep -q "^R|0|MISSING" "$WORK/tab.out"; then
    problem "K6: we_cancel.c has no table wpe_cancel_claims (the scan is blind)"
    return
  fi
  awk -F'|' '$1 == "R" && $2 != "0"' "$WORK/tab.out" | while read -r row; do
    n=$(echo "$row" | cut -d'|' -f2)
    guard=$(echo "$row" | cut -d'|' -f3)
    what=$(echo "$row" | cut -d'|' -f4)
    keys=$(echo "$row" | cut -d'|' -f5)
    front=$(echo "$row" | cut -d'|' -f7)
    busy=$(echo "$row" | cut -d'|' -f8)
    infront=$(echo "$row" | cut -d'|' -f10)
    cancel=$(echo "$row" | cut -d'|' -f11)
    if [ "$what" = "MISSING" ] || [ -z "$what" ]; then
      problem "K6: row $n has no what: a claim the user is never named"
      continue
    fi
    [ "$busy" = "NULL" ] || [ -z "$busy" ] &&
      problem "K6: the row \"$what\" has no busy predicate"
    [ "$cancel" = "NULL" ] || [ -z "$cancel" ] &&
      problem "K6: the row \"$what\" has no cancel"
    case $keys in
      (*WPE_CANCEL_ESC*) : ;;
      (*) problem "K6: the row \"$what\" does not list Esc" ;;
    esac
    case $keys in
      (*WPE_CANCEL_CTRL_C*)
        if [ "$what" != "the debugger's answer" ]; then
          problem "K6: the row \"$what\" lists Ctrl-C: one row alone takes it (the debugger's answer)"
        fi
        ;;
    esac
    if [ "$front" != "0" ]; then
      if [ "$infront" = "NULL" ] || [ -z "$infront" ]; then
        problem "K6: the row \"$what\" is front_only and has no in_front predicate"
      fi
      grep -q "^K6|front|$what|" "$WORK/k6.want" ||
        problem "K6: the row \"$what\" is front_only and the allow file does not name it (want: K6|front|$what|<reason>)"
    fi
    # the guard of the row and the file that defines its busy predicate
    if [ -n "$FEATURES_OK" ]; then
      file=$(awk -F'|' -v n="$busy" '$1 == "D" && $3 == n { print $2; exit }' "$WORK/rec.out")
      if [ -z "$file" ]; then
        problem "K6: the row \"$what\" names $busy, which no product source defines (the scan is blind)"
      else
        want=$(guard_of "$file")
        case $want in
          (WPE_AI)
            [ "$guard" = "WPE_AI" ] ||
              problem "K6: the row \"$what\" is not under WPE_AI but $busy lives in $file: a build without the assistant would not link"
            ;;
          (PROG)
            case $guard in
              (PROG|none) : ;;
              (*) problem "K6: the row \"$what\" is under $guard but $busy lives in $file (PROG)" ;;
            esac
            ;;
          (DEBUGGER)
            case $guard in
              (DEBUGGER|none) : ;;
              (*) problem "K6: the row \"$what\" is under $guard but $busy lives in $file (DEBUGGER)" ;;
            esac
            ;;
          (FEATURE)
            problem "K6: the row \"$what\" names $busy, which lives in $file, a display's source"
            ;;
        esac
      fi
    fi
  done
  # a front entry the table does not have
  awk -F'|' '$1 == "K6" && $2 == "front" { print $3 }' "$WORK/k6.want" |
    while read -r what; do
    grep -q "|$what|" "$WORK/tab.out" ||
      problem "K6: the allow entry names the front_only row \"$what\", which the table does not have (stale)"
  done
}

# The guard a source file's claims must sit under, from WPE_FEATURE_SOURCES.
guard_of() {
  file=$1
  case $file in
    (we_prog.c) echo PROG; return ;;
    (we_debug.c) echo DEBUGGER; return ;;
  esac
  feature=$(echo "$FEATURE_MAP" | awk -F'|' -v f="$file" '$2 == f { print $1; exit }')
  case $feature in
    (enable-ai|enable-ai-agent-host) echo WPE_AI; return ;;
    ("") echo none; return ;;
    (*) echo FEATURE; return ;;
  esac
}

# ---- --list: the allow lines to start from --------------------------------

list_allow() {
  awk -F'|' '$1 == "R" && $2 != "0" { print $4 }' "$WORK/tab.out" | sort -u |
    while read -r what; do
    echo "# the table has a row \"$what\""
  done
  awk -F'|' '$1 == "S"' "$WORK/rec.out" | sort -t'|' -k2,3 |
    while read -r rec; do
    file=$(echo "$rec" | cut -d'|' -f2)
    hold=$(echo "$rec" | cut -d'|' -f3)
    cnt=$(echo "$rec" | cut -d'|' -f4)
    sample=$(echo "$rec" | cut -d'|' -f6)
    echo "K4|claim|CHOOSE|$file|$hold|$cnt|$sample"
  done
  awk -F'|' '$1 == "M"' "$WORK/rec.out" | sort -t'|' -k2,3 |
    while read -r rec; do
    file=$(echo "$rec" | cut -d'|' -f2)
    fn=$(echo "$rec" | cut -d'|' -f3)
    cnt=$(echo "$rec" | cut -d'|' -f4)
    rest=$(echo "$rec" | cut -d'|' -f4-8 | tr '|' ' ')
    echo "K5|$(k5_kind "$file" $rest)|$file|$fn|$cnt|CHOOSE"
  done
  awk -F'|' '$1 == "R" && $2 != "0" && $7 != "0" { print "K6|front|" $4 "|CHOOSE" }' "$WORK/tab.out"
}

# ---- the scan of one tree, all clauses ------------------------------------

run_clauses() {
  dir=$1; allow=$2
  scan_tree "$dir" > "$WORK/rec.out"
  scan_table "$dir" > "$WORK/tab.out"
  scan_edit "$dir"
  scan_pane "$dir"
  k1_tree_clause "$dir"
  scan_calls_of_owner "$dir" > "$WORK/rec_calls.out"
  # every function name in the tree, for a K4 loop| entry's blind guard
  awk -F'|' '$1 == "D" { print $2 "|" $3 }' "$WORK/rec.out" | sort -u > "$WORK/k4.loops"
  allow_of K6 "$allow" > "$WORK/k6.want"
  k2_clause "$dir"
  k4_clause "$allow"
  k5_clause "$allow"
  k6_clause "$allow"
}

# The tree half of K1: the helper that tested the key for the debugger's
# answer is gone, and the answer is a row of the table like any other.
k1_tree_clause() {
  dir=$1
  for f in $(product_files "$dir"); do
    case $f in
      *.h) continue ;;
    esac
    [ -f "$dir/$f" ] || continue
    grep -q "e_key_cancels_answer" "$dir/$f" &&
      echo "K1: $f names e_key_cancels_answer: the debugger's answer is a row of the table"
  done
  return 0
}

# The calls inside each function of we_cancel.c (K2 follows an adapter).
scan_calls_of_owner() {
  dir=$1
  scan_table "$dir" | awk -F'|' '$1 == "F"'
}

# ---- the fixtures of the canaries -----------------------------------------

emit_fx_cancel() {
  cat <<'EOF'
#define WPE_CANCEL_ESC 1u
#define WPE_CANCEL_CTRL_C 2u

static unsigned e_cancel_key_bit(int c)
{
 if (c == WPE_ESC)
  return(WPE_CANCEL_ESC);
 if (c == CtrlC)
  return(WPE_CANCEL_CTRL_C);
 return(0);
}

static void e_cancel_build(FENSTER *top)
{
 (void)top;
 e_build_cancel(); /*PLANT:adapter_call*/
}

static void e_cancel_answer(FENSTER *top)
{
 (void)top;
 e_d_answer_cancel();
}

static const WpeCancelClaim wpe_cancel_claims[] = {
#ifdef PROG
 { "the build", WPE_CANCEL_ESC, 0, 0,
   e_build_busy, e_build_ending, e_messages_window_on_top, e_cancel_build },
 /*PLANT:table_row*/
#endif
#ifdef DEBUGGER
 { "the debugger's answer", WPE_CANCEL_ESC | WPE_CANCEL_CTRL_C, 1, 0,
   e_d_answer_busy, NULL, NULL, e_cancel_answer },
#endif
#ifdef WPE_AI
 { "the chat pane", WPE_CANCEL_ESC, 0, 1,
   wpe_ai_chat_pane_open, NULL, wpe_ai_chat_pane_in_front,
   wpe_ai_chat_pane_close },
#endif
};
EOF
}

emit_fx_edit() {
  cat <<'EOF'
static int e_key_is_mouse(int c)
{
 return(c < 0);
}

int e_eingabe(ECNT *e)
{
 FENSTER *f = e->f[e->mxedt];
 int c = 0;
 int by_click = 0;

 while (c != WPE_ESC)
 {
  /*PLANT:loop_top*/
  c = e_getch();
  by_click = e_key_is_mouse(c); /*PLANT:click_local*/
  if (e_debug_console_input(c, f))
   continue;
  e_lsp_on_edit(f, c);
  if (e_cancel_key(f, c, by_click)) /*PLANT:cancel_call*/
  {
   c = 0;
   continue;
  }
  if (e_ai_chat_key(f, c)) continue;
  /*PLANT:after_chat*/
 }
 return(0);
}

int e_dialog_result(FENSTER *f)
{
 /*PLANT:result_fn*/
 if (e_opt_kst(f) != WPE_ESC)
  return(1);
 return(0);
}
EOF
}

emit_fx_prog() {
  cat <<'EOF'
#define E_BUILD_WAIT_FMT "building ... %lds  (Esc cancels)"
/*PLANT:prog_define*/

int e_build_busy(void)
{
 return(g_build.active);
}

int e_build_ending(void)
{
 return(g_build.cancelled && !g_build.reaped);
}

void e_build_cancel(void)
{
 g_build.cancelled = 1;
}

int e_messages_window_on_top(const FENSTER *top)
{
 return(top != NULL && e_window_pane(top) == WPE_PANE_MESSAGES);
}
/*PLANT:prog_body*/
EOF
}

emit_fx_debug() {
  cat <<'EOF'
int e_d_answer_busy(void)
{
 return(e_d_answer.waiting);
}

void e_d_answer_cancel(void)
{
 e_d_answer.waiting = 0;
}
/*PLANT:debug_body*/
EOF
}

emit_fx_ai() {
  cat <<'EOF'
int wpe_ai_chat_pane_open(void)
{
 return(g_ai_chat_pane != NULL);
}

int wpe_ai_chat_pane_in_front(const FENSTER *top)
{
 return(ai_chat_focused_pane(top) != NULL);
}

void wpe_ai_chat_pane_close(FENSTER *top)
{
 (void)top;
 e_ai_chat_close();
}

int e_ai_chat_key(FENSTER *f, int c)
{
 /*PLANT:chat_key*/
 if (c == BUP)
  return(1);
 return(0);
}
/*PLANT:ai_body*/
EOF
}

emit_fx_wind() {
  cat <<'EOF'
/*PLANT:wind_alias*/
int e_wind_close(FENSTER *f)
{
 /*PLANT:wind_body*/
 return(0);
}

int e_plant_loop(FENSTER *f)
{
 int c = e_getch();

 if (c == E_PLANT_KEY)
  return(1);
 return(0);
}
EOF
}

emit_fx_header() {
  cat <<'EOF'
#define E_PLANT_OTHER 1
/*PLANT:header_define*/
EOF
}

emit_fx_allow() {
  cat <<'EOF'
K4|claim|the build|we_prog.c|E_BUILD_WAIT_FMT|1|the build's waiting line
K5|loop|we_edit.c|e_eingabe|1|the editor's key loop
K5|result|we_edit.c|e_dialog_result|1|a box's result
K5|claim|we_cancel.c|e_cancel_key_bit|2|the owner's own key bit
K6|front|the chat pane|the pane closes on a key from its own window
EOF
}

# plant_into DIR MARKER TEXT - DIR gets the clean fixtures, with the line that
# holds MARKER replaced by TEXT (\n in TEXT is a line break; an empty TEXT
# deletes the line).
plant_into() {
  dir=$1; marker=$2; text=$3
  rm -rf "$dir"; mkdir -p "$dir"
  for path in "$WORK/fx"/*; do
    file=$(basename "$path")
    case $file in
      (*.allow) continue ;;
    esac
    awk -v m="/*PLANT:$marker*/" -v t="$text" '
      index($0, m) { print t; next }
      { print }' "$path" > "$dir/$file"
  done
}

canaries_run=0

# canary NAME MARKER TEXT WORD - the planted offender must be named, in a
# message that holds WORD.
canary() {
  canary_name=$1; canary_marker=$2; canary_text=$3; canary_word=$4
  canaries_run=$((canaries_run + 1))
  plant_into "$WORK/fx-case" "$canary_marker" "$canary_text"
  canary_run "$WORK/fx-case" "$WORK/fx/fixture.allow"
  if ! grep -q -F "$canary_word" "$WORK/fx-case.out"; then
    problem "canary $canary_name: not named with \"$canary_word\" (the clause is blind)"
  fi
}

# canary_allow NAME LINE WORD - an allow file with one line more must be named.
canary_allow() {
  canary_name=$1; canary_line=$2; canary_word=$3
  canaries_run=$((canaries_run + 1))
  { cat "$WORK/fx/fixture.allow"; echo "$canary_line"; } > "$WORK/fx-case.allow"
  canary_run "$WORK/fx" "$WORK/fx-case.allow"
  if ! grep -q -F "$canary_word" "$WORK/fx-case.out"; then
    problem "canary $canary_name: not named with \"$canary_word\" (the clause is blind)"
  fi
}

# canary_run DIR ALLOW - every offence of one planted tree, in one file.
canary_run() {
  PROBLEMS=$WORK/fx-case.out
  : > "$PROBLEMS"
  run_clauses "$1" "$2" >> "$PROBLEMS" 2>&1
  PROBLEMS=$WORK/problems
}

selftest() {
  rm -rf "$WORK/fx"; mkdir -p "$WORK/fx"
  emit_fx_cancel > "$WORK/fx/we_cancel.c"
  emit_fx_edit > "$WORK/fx/we_edit.c"
  emit_fx_prog > "$WORK/fx/we_prog.c"
  emit_fx_debug > "$WORK/fx/we_debug.c"
  emit_fx_ai > "$WORK/fx/we_ai_ui.c"
  emit_fx_wind > "$WORK/fx/we_wind.c"
  emit_fx_header > "$WORK/fx/we_fx.h"
  emit_fx_allow > "$WORK/fx/fixture.allow"
  # The fixture tree's own feature map, so the guard canary does not depend on
  # the environment make check sets for the real tree.
  FEATURE_MAP="enable-ai|we_ai_ui.c"
  FEATURES_OK=1

  # the clean fixtures pass
  PROBLEMS=$WORK/fx-clean.out
  : > "$PROBLEMS"
  run_clauses "$WORK/fx" "$WORK/fx/fixture.allow" >> "$PROBLEMS" 2>&1
  PROBLEMS=$WORK/problems
  if [ -s "$WORK/fx-clean.out" ]; then
    problem "selftest: the clean fixtures are named (a clause is noisy):"
    cat "$WORK/fx-clean.out" >> "$PROBLEMS"
  fi

  # K1: a key test beside the owner, a second call, a call above the program's
  # own input, another third argument, no click local, and the deleted helper
  # back in the tree.
  canary k1-key-test loop_top ' if (c == WPE_ESC && fake_busy()) fake_cancel();' \
    "WPE_ESC appears 2 times"
  canary k1-ctrlc loop_top ' if (c == CtrlC) c = 0;' "CtrlC appears"
  canary k1-second-call after_chat ' if (e_cancel_key(f, c, by_click)) c = 0;' \
    "called 2 times"
  canary k1-no-call cancel_call '' "e_cancel_key is not called"
  canary k1-call-above loop_top ' if (e_cancel_key(f, c, by_click)) c = 0;' \
    "above e_debug_console_input"
  canary k1-third-argument cancel_call ' if (e_cancel_key(f, c, 0)) c = 0;' \
    "not the click local"
  canary k1-no-click-local click_local ' by_click = 0;' \
    "no local is set from e_key_is_mouse"
  canary k1-old-helper wind_body 'int e_key_cancels_answer(int c)\n{\n return(c == WPE_ESC);\n}' \
    "e_key_cancels_answer"

  # K2: a cancel called from a third file, and a name nothing defines.
  canary k2-cancel-elsewhere wind_body ' e_build_cancel();' \
    "we_wind.c calls e_build_cancel"
  canary k2-exported-cancel-elsewhere wind_body ' wpe_ai_chat_pane_close(f);' \
    "we_wind.c calls wpe_ai_chat_pane_close"
  canary k2-name-gone adapter_call ' e_build_cancel(); e_build_cancel_gone();' \
    "defined in no product source"

  # K3: the pane's Esc branch back.
  canary k3-pane-esc chat_key ' if (c == WPE_ESC) { e_ai_chat_close(); return(1); }' \
    "e_ai_chat_key holds WPE_ESC"

  # K4: a promise with no entry, a promise in a function, a stale entry, a
  # grown holder, a claim the table does not have.
  canary k4-new-promise prog_define '#define E_NEW_LINE "hold on (Esc stops it)"' \
    "has no allow entry"
  canary k4-promise-in-a-function prog_body 'void e_build_say(FENSTER *f)\n{\n e_status_say(f, "Esc ends it");\n}' \
    "has no allow entry"
  canary_allow k4-stale 'K4|claim|the build|we_prog.c|E_GONE_LINE|1|a macro that is gone' \
    "no string naming Esc"
  canary_allow k4-count 'K4|claim|the build|we_prog.c|E_BUILD_WAIT_FMT|2|a second promise' \
    "holds 1 strings naming Esc"
  canary_allow k4-unknown-claim 'K4|claim|the pipeline|we_prog.c|E_BUILD_WAIT_FMT|1|no such row' \
    "which the table does not have"

  # K5: a result function that gains a key test, a numeric escape code, a
  # function nobody listed, a stale entry.
  canary k5-grown-count result_fn ' while (k != WPE_ESC) k++;' \
    "mentions a key 2 times, the entry says 1"
  canary k5-numeric result_fn ' if (k == 27) k = 0;' "mentions a key 2 times"
  canary k5-new-function wind_body ' if (c == WPE_ESC) return(1);' "has no allow entry"
  canary_allow k5-stale 'K5|result|we_wind.c|e_wind_gone|1|a function that is gone' \
    "mentions no key"

  # K4: the promise in any case.
  canary k4-upper-case prog_define '#define E_NEW_LINE "hold on (ESC cancels)"' \
    "has no allow entry"
  canary k4-lower-case prog_define '#define E_NEW_LINE "hold on (esc cancels)"' \
    "has no allow entry"

  # K5: the key under every spelling of its byte, and behind a name.
  canary k5-octal wind_body ' if (c == 033) return(1);' "e_wind_close mentions a key 1 times"
  canary k5-octal-char wind_body " if (c == '\\\\033') return(1);" "e_wind_close mentions a key 1 times"
  canary k5-hex-char wind_body " if (c == '\\\\x1b') return(1);" "e_wind_close mentions a key 1 times"
  canary k5-hex-upper-char wind_body " if (c != '\\\\x1B') return(1);" "e_wind_close mentions a key 1 times"
  canary k5-hex wind_body ' if (c == 0x1B) return(1);' "e_wind_close mentions a key 1 times"
  canary k5-case-octal wind_body ' switch (c) { case 033: return(1); }' "e_wind_close mentions a key 1 times"
  canary k5-reversed wind_body ' if (27 == c) return(1);' "e_wind_close mentions a key 1 times"
  canary k5-macro-27 header_define '#define E_PLANT_KEY 27' "e_plant_loop mentions a key 1 times"
  canary k5-macro-paren header_define '#define E_PLANT_KEY (0x1b)' "e_plant_loop mentions a key 1 times"
  canary k5-macro-octal-char header_define "#define E_PLANT_KEY '\\\\033'" "e_plant_loop mentions a key 1 times"
  canary k5-macro-hex-char header_define "#define E_PLANT_KEY '\\\\x1b'" "e_plant_loop mentions a key 1 times"
  canary k5-macro-octal header_define '#define E_PLANT_KEY 033' "e_plant_loop mentions a key 1 times"
  canary k5-macro-of-macro header_define '#define E_PLANT_KEY E_PLANT_BASE\n#define E_PLANT_BASE 27' \
    "e_plant_loop mentions a key 1 times"
  canary k5-const wind_alias 'static const int E_PLANT_KEY = 27;' "e_plant_loop mentions a key 1 times"
  canary k5-enum wind_alias 'enum { E_PLANT_KEY = 0x1b };' "e_plant_loop mentions a key 1 times"

  # K6: a row with no cancel, no busy, no Esc, a second Ctrl-C row, a
  # front_only row nobody named, a row of a feature under no guard, and a
  # front entry the table does not have.
  canary k6-no-cancel table_row ' { "the fake", WPE_CANCEL_ESC, 0, 0, e_build_busy, NULL, NULL, NULL },' \
    "has no cancel"
  canary k6-no-busy table_row ' { "the fake", WPE_CANCEL_ESC, 0, 0, NULL, NULL, NULL, e_cancel_build },' \
    "has no busy"
  canary k6-no-esc table_row ' { "the fake", WPE_CANCEL_CTRL_C, 0, 0, e_build_busy, NULL, NULL, e_cancel_build },' \
    "does not list Esc"
  canary k6-second-ctrl-c table_row ' { "the fake", WPE_CANCEL_ESC | WPE_CANCEL_CTRL_C, 0, 0, e_build_busy, NULL, NULL, e_cancel_build },' \
    "lists Ctrl-C"
  canary k6-front-unnamed table_row ' { "the fake", WPE_CANCEL_ESC, 0, 1, e_build_busy, NULL, e_messages_window_on_top, e_cancel_build },' \
    "front_only and the allow file does not name it"
  canary k6-no-in-front table_row ' { "the fake", WPE_CANCEL_ESC, 0, 1, e_build_busy, NULL, NULL, e_cancel_build },' \
    "front_only and has no in_front"
  canary k6-wrong-guard table_row ' { "the fake", WPE_CANCEL_ESC, 0, 0, wpe_ai_chat_pane_open, NULL, NULL, wpe_ai_chat_pane_close },' \
    "not under WPE_AI"
  canary_allow k6-front-stale 'K6|front|the pipeline|a row that is gone' \
    "which the table does not have"
}

# ---- the real tree --------------------------------------------------------

FEATURE_MAP=
FEATURES_OK=

read_features() {
  [ -n "$WPE_FEATURE_SOURCES" ] || return 1
  echo "$WPE_FEATURE_SOURCES" | tr ';' '\n' | awk -F: '
    NF == 2 { n = split($2, f, " "); for (k = 1; k <= n; k++) print $1 "|" f[k] }' \
    > "$WORK/features"
  FEATURE_MAP=$(cat "$WORK/features")
  FEATURES_OK=1
  return 0
}

check_allow_file() {
  [ -f "$ALLOW" ] || { problem "no allow file at $ALLOW"; return; }
  awk -F'|' '
    /^#/ || /^[ \t]*$/ { next }
    $1 != "K4" && $1 != "K5" && $1 != "K6" {
      print "check-esc-owner: allow line " NR " starts with " $1 ", not K4, K5 or K6"; bad = 1; next
    }
    $1 == "K4" && NF != 7 { print "check-esc-owner: allow line " NR " has " NF " fields, not 7"; bad = 1 }
    $1 == "K5" && NF != 6 { print "check-esc-owner: allow line " NR " has " NF " fields, not 6"; bad = 1 }
    $1 == "K6" && NF != 4 { print "check-esc-owner: allow line " NR " has " NF " fields, not 4"; bad = 1 }
    END { exit bad }' "$ALLOW" >> "$WORK/problems" || :
}

if [ -n "$LIST_ONLY" ]; then
  scan_tree "$SRCDIR" > "$WORK/rec.out"
  scan_table "$SRCDIR" > "$WORK/tab.out"
  list_allow
  exit 0
fi

if [ -z "$SELFTEST_ONLY" ]; then
  check_allow_file
  if read_features; then
    :
  else
    SKIPPED="$SKIPPED the guard half of K6 not run (no WPE_FEATURE_SOURCES);"
  fi
  run_clauses "$SRCDIR" "$ALLOW" >> "$WORK/problems" 2>&1
fi
selftest

if [ -s "$WORK/problems" ]; then
  echo "check-esc-owner: FAIL"
  sort -u "$WORK/problems"
  exit $BAD
fi
if [ -n "$SKIPPED" ]; then
  echo "check-esc-owner: SKIP,$SKIPPED the rest and the canaries passed"
  exit $SKIP
fi
echo "check-esc-owner: ok (K1-K6, $canaries_run canaries)"
exit 0
