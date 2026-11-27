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
# --selftest runs only the canaries.  Fourteen rules (the first ten numbered
# below, the system's reason is said (R11), the owner's size (R12), the
# cutting quoter (R13), and the windows the editor writes (R14)), each with canaries: the script plants an offender in a copy of
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
# 11. The system's reason is said, not guessed (R11a, R11b, R11c).  A
#     function that asks the system about a path (access, stat, lstat, fopen,
#     open, opendir, realpath, chdir, or a read of the read owner) and says a
#     cause it guessed is named: R11b for a string literal that says "not
#     found" or "cannot open" with no errno or strerror in the function, R11c
#     for a message table entry (e_msg, e_p_msg, e_d_msg) whose words guess a
#     cause, whether or not the function names errno, since a table entry can
#     never carry the reason.  The entries are read from messages.h (the
#     name of each index) and the three initializers (the words of each
#     index); a count that differs from the defines fails the scan.  A name
#     the table words match that is about something other than a path is
#     listed in TABLE_NOT_A_PATH below.  R11a keeps the project's own
#     sentences in the project reporter.
# 14. A window the editor writes opens through e_window_view_open or
#     e_window_pane_open.  A call of e_edit whose second argument is a string
#     literal other than "", "Help", "Messages", "Watches" or "Stack", or a
#     macro ending in _NAME, names a window that no file backs (the
#     assistant's pane, a report); such a name opens through e_window_view_open,
#     which never reads the disk.  A name held in a variable is not seen.
#     (tests/check-window-kind.sh rule 8 refuses every literal, including
#     those five, and holds the one maker.)
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
# How it is made fast: one awk pass per source file (scan_file) blanks the
# comments, applies every rule that needs one file alone, and writes that
# file's "facts" to the work folder: its defines, the copies and the unbounded
# scans it counts per function, what rule 11 knows of each function and the
# names that function calls, the arrays rule 12 sees handed to a path, and the
# findings it can already make.  One pass over the tree before it (macro_facts)
# collects the macros, since rule 11's macro classes and rule 14's wrappers
# have to be known before any file is judged.  The rules that look at the whole
# tree (1, 7, 9, 10's stale lines, 11 and 12) then read the facts of every
# file in one pass (aggregate), never the sources.  A canary plants a tree of
# two or three small files and scans no more than those.
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

# The message tables rule 11 reads: "table:file" for each (where its
# initializer is).  The sources that hold them, and the entries whose words
# are about something other than a path.
TABLE_FILES="e_msg:we_main.c e_p_msg:we_prog.c e_d_msg:we_debug.c"
# "Command not found" is the answer of a command that is looked up on the
# PATH, not of a file or folder the user named.
TABLE_NOT_A_PATH=" ERR_COMMAND "

# The awk functions rule 11 shares between its scans (macros, sayers, words).
WORDS_LIB='
function load_set(file, set,   line) {
  while ((getline line < file) > 0) set[line] = 1
  close(file)
}
function word_in(code, name) {
  return code ~ ("(^|[^A-Za-z0-9_])" name "([^A-Za-z0-9_]|$)")
}
# A call of a function that asks the system about a path, or a mention of
# one that is not a call (a pointer to it, called through another name).
function has_path_call(code,   c2) {
  # every name below holds one of these; a line without one cannot match
  if (index(code, "open") == 0 && index(code, "stat") == 0 && index(code, "access") == 0 &&
      index(code, "chdir") == 0 && index(code, "realpath") == 0 && index(code, "wpe_read_") == 0) return 0
  c2 = code
  if (index(c2, "struct")) gsub(/struct[ \t]+stat/, "", c2)
  if (c2 ~ /(^|[^A-Za-z0-9_.>])(access|stat|lstat|fopen|opendir|realpath|chdir)([^A-Za-z0-9_]|$)/) return 1
  return c2 ~ /(^|[^A-Za-z0-9_.>])(open|wpe_read_(open|stream_open|text|folder|name_ok))[ \t]*[(]/
}
# Does the code say a table entry whose words guess a cause?  e_msg[ERR_X],
# and the pointer form *(e_msg + ERR_X).
function table_entry_said(code,   rest, m, tn, nm) {
  if (index(code, "msg") == 0) return 0
  rest = code
  while (match(rest, /(e_msg|e_p_msg|e_d_msg)[ \t]*(\[[ \t]*|[+][ \t]*)ERR_[A-Za-z0-9_]+/)) {
    m = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
    tn = m; sub(/[ \t]*(\[|[+]).*/, "", tn)
    nm = m; sub(/^.*(\[|[+])[ \t]*/, "", nm)
    if ((tn "|" nm) in tabw) return 1
  }
  return 0
}
'

# macro_facts DIR TW MC EM - one pass over the top level sources of DIR (the
# .c files first, then the .h, as the shell lists them): the comments are
# blanked and every #define is read, a body continued over lines joined with
# blanks.  Then the classes rule 11 needs go to MC, one "kind|name" a line, and
# the macros rule 14 reads through go to EM, one "obj<TAB>name<TAB>body" or
# "wrap<TAB>name" a line.  TW is the file of message table entries whose words
# guess a cause; a macro is of a class when its body is, and a macro that names
# a macro of a class is of that class too.
macro_facts() {
  awk -v tw="$2" -v mcout="$3" -v emout="$4" "$BLANK_C$WORDS_LIB"'
    # add(kind, name) - name is of the class kind, once.
    function add(kind, name) {
      if (!((kind "|" name) in cls)) { cls[kind "|" name] = 1; grew = 1 }
    }
    # one_macro(name, body) - class this #define for rule 11 and record what
    # rule 14 reads through it.
    function one_macro(name, body,   code, b) {
      code = body; gsub(/"([^"\\]|\\.)*"/, "\"\"", code)
      macro[name] = code
      if (table_entry_said(code)) add("tab", name)
      if (has_path_call(code)) add("call", name)
      if (body ~ /"[^"]*([Nn]ot found|Can[\\\047]*t open|[Cc]annot open)[^"]*"/) add("lit", name)
      if (code ~ /(^|[^A-Za-z0-9_])(errno|strerror)([^A-Za-z0-9_]|$)/) add("named", name)
      b = body; gsub(/^[ \t]+|[ \t]+$/, "", b)
      # an object-like macro (no parameter list): what it stands for, to be
      # read through however many macros it takes to reach a literal
      if (body ~ /^[ \t]/ && b != "") print "obj\t" name "\t" b > emout
      else if (body ~ /(^|[^A-Za-z0-9_.>])e_edit[ \t]*[(]/) print "wrap\t" name > emout
    }
    BEGIN { load_set(tw, tabw); printf "" > mcout; printf "" > emout }
    FNR == 1 { blank_incom = 0; cont = 0; body = "" }
    {
      line = blank_c($0, 1)
      if (cont) body = body " " line
      else if (line ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/) {
        s = line; sub(/^[ \t]*#[ \t]*define[ \t]+/, "", s)
        name = s; sub(/[^A-Za-z0-9_].*$/, "", name)
        body = substr(s, length(name) + 1)
      } else next
      if (body ~ /\\[ \t]*$/) { sub(/[ \t]*\\[ \t]*$/, "", body); cont = 1 }
      else { cont = 0; one_macro(name, body) }
    }
    END {
      for (round = 0; round < 8; round++) {
        grew = 0
        for (c in cls) {
          kind = c; sub(/[|].*/, "", kind); cn = substr(c, length(kind) + 2)
          for (m in macro)
            if (m != cn && word_in(macro[m], cn)) pend[kind "|" m] = 1
        }
        for (p in pend) { if (!(p in cls)) { cls[p] = 1; grew = 1 }; delete pend[p] }
        if (!grew) break
      }
      for (c in cls) print c > mcout
    }' "$1"/*.c "$1"/*.h
}

. "$(dirname "$0")/workdir.sh"
work_dir path-buffers $BAD

# strip_comments FILE - the file's lines with comments blanked, string
# literals kept, one output line per input line.
strip_comments() {
  awk "$BLANK_C"'
    FNR == 1 { blank_incom = 0 }
    { print blank_c($0, 1) }' "$1"
}

# table_words DIR - the message table entries of the sources in DIR whose
# words guess a cause (not found, cannot open, access, find or read), as
# "table|NAME" lines in the file TW names.  A problem of the reading itself (a
# file or a table that is not there, an entry count that differs from the
# number of defines) goes as an R11c line into the file TW_PROBLEMS names, so a
# scan that cannot see the tables fails instead of passing blind.  The result
# is kept per DIR and read again for the same DIR: a canary tree has no tables,
# so it asks for the real ones, and every canary asks for the same.
table_words() {
  tw_dir=$1
  if [ "$tw_dir" != "$TW_DIR" ]; then
    TW_DIR=$tw_dir
    TW=$WORK/tablewords.$(printf '%s' "$tw_dir" | cksum | cut -d' ' -f1)
    TW_PROBLEMS=$TW.problems
  fi
  [ -f "$TW" ] && return 0
  : > "$TW_PROBLEMS"
  tw_work=$(mktemp -d "$WORK/tw.XXXXXX")
  if [ ! -f "$tw_dir/messages.h" ]; then
    echo "R11c: messages.h is missing in $tw_dir: the message tables cannot be read" >> "$TW_PROBLEMS"
  else
    # table|NAME|index, from the raw text: the sections are comments.
    awk '
      /errors defined in e_p_msg/ { t = "e_p_msg"; next }
      /errors defined in e_d_msg/ { t = "e_d_msg"; next }
      /errors defined in e_msg/   { t = "e_msg"; next }
      t != "" && /^#[ \t]*define[ \t]+ERR_[A-Za-z0-9_]+[ \t]+[0-9]+/ { print t "|" $2 "|" $3 }' \
      "$tw_dir/messages.h" > "$tw_work/defs"
    : > "$tw_work/entries"
    for spec in $TABLE_FILES; do
      tab=${spec%%:*}; file=${spec#*:}
      if [ ! -f "$tw_dir/$file" ]; then
        echo "R11c: $file is missing in $tw_dir: the table $tab cannot be read" >> "$TW_PROBLEMS"
        continue
      fi
      strip_comments "$tw_dir/$file" | awk -v tab="$tab" '
        function flush() {
          if (entry ~ /[^ \t\n]/) {
            if (entry ~ /([Nn]ot found|Can[\\\047]*t (open|access|find|read))/) print "HIT|" tab "|" idx
            idx++
          }
          entry = ""
        }
        { txt = txt $0 "\n" }
        END {
          # the definition, "*table[] = {", not the extern declaration of it
          if (!match(txt, "\\*" tab "\\[\\][ \t\n]*=")) { print "MISSING|" tab; exit }
          p = RSTART + RLENGTH
          start = p + index(substr(txt, p), "{")
          n = split(txt, ch, ""); depth = 0; in_s = 0; entry = ""; idx = 0
          for (i = start; i <= n; i++) {
            c = ch[i]
            if (in_s) {
              entry = entry c
              if (c == "\\") { i++; entry = entry ch[i] }
              else if (c == "\"") in_s = 0
              continue
            }
            if (c == "\"") { in_s = 1; entry = entry c; continue }
            if (c == "(" || c == "{") depth++
            else if (c == ")") depth--
            else if (c == "}") { if (depth == 0) { flush(); break } depth-- }
            else if (c == "," && depth == 0) { flush(); continue }
            entry = entry c
          }
          print "COUNT|" tab "|" idx
        }' >> "$tw_work/entries"
    done
    awk -F'|' -v skip="$TABLE_NOT_A_PATH" '
      FILENAME == ARGV[1] { name[$1 "|" $3] = $2; defs[$1]++; next }
      $1 == "MISSING" { print "R11c: the table " $2 " is not found in its source: the scan is blind"; next }
      $1 == "COUNT" {
        if (defs[$2] + 0 != $3 + 0)
          print "R11c: the table " $2 " has " $3 " entries and messages.h defines " defs[$2] + 0 ": the join by index is wrong"
        next
      }
      $1 == "HIT" {
        k = $2 "|" $3
        if (!(k in name)) { print "R11c: the table " $2 " has a guessing entry (index " $3 ") that messages.h gives no name"; next }
        if (index(skip, " " name[k] " ") == 0) print "WORD|" $2 "|" name[k]
      }' "$tw_work/defs" "$tw_work/entries" > "$tw_work/joined"
    grep '^R11c' "$tw_work/joined" >> "$TW_PROBLEMS"
    awk -F'|' '$1 == "WORD" { print $2 "|" $3 }' "$tw_work/joined" > "$TW"
    if [ ! -s "$TW" ]; then
      echo "R11c: no message table entry guesses a cause in $tw_dir: the scan is blind" >> "$TW_PROBLEMS"
    fi
  fi
  [ -f "$TW" ] || : > "$TW"
  rm -rf "$tw_work"
}

# The awk functions the rules that read one statement share: the arguments of
# a call, the heads that only say whether the statement runs, and the blanks
# inside a declaration's brackets.
AWK_LIB='
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

# scan_file FILE REL ALLOW MC EM TW - one awk pass over one source file: the
# comments are blanked, every rule that one file alone decides is applied, and
# the facts the tree rules need go to stdout, one record a line.  REL is the
# file's name in the tree, ALLOW the file that says what rules 10, 12 and 13
# let stand, MC the macro classes of rule 11, EM the macros rule 14 reads
# through, TW the message table entries whose words guess a cause.  Records
# are tab separated with the payload last, so a tab inside it is harmless:
#   F     text                          a finding of a per-file rule
#   DEF   rel|line|name|value           a #define (rules 1, 7 and 12's sizes)
#   CPY   rel|function|count            strcpy, strcat, strncpy, sprintf (rule 9)
#   SCN   rel|function|count            a word read with %s or %[ and no width (rule 9)
#   SU    rel                           this file used a SENTENCE allow line (rule 10)
#   FN    rel  function  flags  names   what rule 11 knows of one function and
#                                       the names it calls.  flags: c asks the
#                                       system about a path, l says a literal
#                                       that guesses a cause, n names errno or
#                                       strerror, t says a table entry that
#                                       guesses, p says the project did not open
#   AH    rel  line  callee  array  size  scope
#                                       a declared char array handed to a call
#                                       that takes a path (rule 12)
#   FD    rel  function                 a function body the rule 12 walk saw
#   QU    rel  function                 this file used a QUOTE allow line (rule 13)
scan_file() {
  awk -v rel="$2" -v owner="$OWNER_HEADER" -v allowfile="$3" \
      -v mc="$4" -v em="$5" -v tw="$6" \
      "$BLANK_C$AWK_LIB$AWK_SCAN_LIB$WORDS_LIB"'
    # finding(text) - a finding of a rule one file alone decides.
    function finding(text) { print "F\t" text }

    # feed_statements(line) - add one comment blanked line to the statement
    # being read: the text from the first token after a ";", "{" or "}" to the
    # next one, a statement of several lines joined with blanks, the line
    # number that of its first token.  A preprocessor line is its own
    # statement.
    function feed_statements(line,   n, i, c, s) {
      if (line ~ /^[ \t]*#/) {
        stmt_emit()
        s = line; sub(/^[ \t]+/, "", s)
        one_statement(s, NR)
        return
      }
      n = length(line)
      for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (c == ";" || c == "{" || c == "}") { stmt = stmt c; stmt_emit(); continue }
        if (stmt == "" && c !~ /[ \t]/) first = NR
        if (stmt != "" || c !~ /[ \t]/) stmt = stmt c
      }
      if (stmt != "") stmt = stmt " "
    }
    # stmt_emit() - the statement being read is whole: every rule reads it.
    function stmt_emit(   s) {
      if (stmt ~ /[^ \t]/) { s = stmt; sub(/^[ \t]+/, "", s); one_statement(s, first) }
      stmt = ""; first = 0
    }
    # one_statement(s, ln) - the rules that read one statement.
    function one_statement(s, ln) {
      rule2_stmt(s, ln)
      rule3_stmt(s, ln)
      rule4_stmt(s, ln)
      if (!is_owner) rule10_stmt(s, ln)
      rule12_stmt(s, ln)
    }

    # rule2_stmt(s, ln) - a copy function with a field'"'"'s text as its source.
    # A field'"'"'s text is "->txt", or a name a statement of the same file set to
    # one.
    function rule2_stmt(s, ln,   t, rest, start, n, k, a, hit, eq, lhs) {
      if (!rel_names_r2 && index(s, "->txt") == 0 && index(s, "str") == 0 &&
          index(s, "mem") == 0 && index(s, "sprintf") == 0 && index(s, "stpcpy") == 0) return
      t = s
      gsub(/[ \t]*->[ \t]*/, "->", t)   # "w -> txt" is the same field
      # a name set from a field: "char *p = o->wstr[0]->txt;" or "p = w->txt;"
      if (t ~ /^[^;(]*[A-Za-z_][A-Za-z0-9_]*[ \t]*=[^=].*->txt[^;]*;$/ && t !~ /^(if|while|for|return)[ \t(]/) {
        eq = index(t, "="); lhs = substr(t, 1, eq - 1); sub(/[ \t]+$/, "", lhs)
        if (match(lhs, /[A-Za-z_][A-Za-z0-9_]*$/)) alias[++naliases] = substr(lhs, RSTART)
      }
      rest = t
      while (match(rest, r2fn)) {
        start = RSTART + RLENGTH
        n = arg_split(rest, start)
        for (k = 2; k <= n; k++) {
          hit = (ARGS[k] ~ /->txt([^A-Za-z0-9_]|$)/)
          for (a = 1; !hit && a <= naliases; a++)
            hit = (ARGS[k] ~ ("(^|[^A-Za-z0-9_>.])" alias[a] "([^A-Za-z0-9_]|$)"))
          if (hit) {
            finding("R2: " rel ":" ln ": a copy function reads a field\047s text; use e_opt_field_take")
            break
          }
        }
        rest = substr(rest, start)
      }
    }

    # rule3_stmt(s, ln) - the result of a refusing writer is dropped.
    function rule3_stmt(s, ln,   t) {
      if (index(s, "wpe_path_") == 0 && index(s, "wpe_field_take") == 0 &&
          index(s, "e_opt_field_take") == 0) return
      t = strip_heads(s)
      if (t ~ r3alone || t ~ r3forbody)
        finding("R3: " rel ":" ln ": the result of a call that can refuse is not read")
      else if (t ~ r3voided)
        finding("R3: " rel ":" ln ": the result of a call that can refuse is cast away")
    }

    # rule4_stmt(s, ln) - a call of e_add_arguments gives its buffer size.
    function rule4_stmt(s, ln) {
      if (index(s, "e_add_arguments") == 0) return
      if (s ~ /^(static[ \t]+)?int[ \t]+e_add_arguments/) return
      if (s ~ /e_add_arguments[ \t]*\(/ && s !~ /e_add_arguments[ \t]*\([^,]*,[ \t]*(sizeof|size[ \t]*,)/)
        finding("R4: " rel ":" ln ": e_add_arguments is called without the size of its buffer")
    }

    # rule10_stmt(s, ln) - the owner'"'"'s sentence and its number stay the
    # owner'"'"'s, over a whole statement (a call split over lines is one).
    # Whole words only: WPE_PATH_MAX_HOPS is another number.
    function rule10_stmt(s, ln,   rest, start, n, k) {
      if (index(s, "WPE_PATH_TOO_LONG_FMT") == 0 && index(s, "sprintf") == 0 &&
          index(s, "snprintf") == 0) return
      if (s ~ r10fmt)
        finding("R10: " rel ":" ln ": WPE_PATH_TOO_LONG_FMT is named outside " owner ": say wpe_path_say_too_long or wpe_path_say_name_too_long")
      rest = s
      while (match(rest, r10fn)) {
        start = RSTART + RLENGTH
        n = arg_split(rest, start)
        for (k = 1; k <= n; k++)
          if (ARGS[k] ~ r10numbers) {
            finding("R10: " rel ":" ln ": a path number is an argument of snprintf or sprintf: the sentence and its number are the owner\047s")
            break
          }
        rest = substr(rest, start)
      }
    }

    # line_rules(line) - rules 5 and 6, and rules 8 and the line half of 10,
    # over one comment blanked line.  A line that holds none of the words each
    # group needs cannot match it, and is not read further.
    function line_rules(line,   l, m, decl, nt, toks, k, i) {
      if (index(line, "[") || index(line, "MALLOC") || index(line, "WPE_PATHMAX") ||
          index(line, "strcpy") || index(line, "strncpy") || index(line, "E_X_SYSTEM")) {
        l = tight(line)
        if (l ~ /(^|[^A-Za-z0-9_])filen\[128\]/)
          finding("R5: " rel ":" NR ": a file name picked in the manager is kept in 128 bytes")
        if (l ~ /(^|[^A-Za-z0-9_])(search|replace)\[80\]/)
          finding("R5: " rel ":" NR ": a Find text is kept in 80 bytes under a field of 128")
        if (l ~ /\(strcpy\)[ \t]*\(/)
          finding("R5: " rel ":" NR ": a history entry is copied into a field without its capacity")
        if (l ~ /(^|[^A-Za-z0-9_])library\[80\]/)
          finding("R5: " rel ":" NR ": the project library is kept in 80 bytes")
        if (l ~ /e_run_src_(dirct|datnam)\[/ || l ~ /strncpy\(e_run_src/)
          finding("R5: " rel ":" NR ": the source Run keeps is held in a fixed array or cut")
        if (l ~ /MALLOC\(129\)/)
          finding("R5: " rel ":" NR ": a name offered for Move is kept in 129 bytes")
        if (l ~ /(^|[^A-Za-z0-9_])WPE_PATHMAX([^A-Za-z0-9_]|$)/ || l ~ /E_X_SYSTEM_PATH_MAX/)
          finding("R5: " rel ":" NR ": a private path number is back")
        if (l ~ /strncpy\(dirtmp/)
          finding("R5: " rel ":" NR ": the Directory field is cut by strncpy")
        for (m = 1; m <= nmoved; m++) {
          if (match(l, "(^|[^A-Za-z0-9_])char[^;(=]*[^A-Za-z0-9_]" moved[m] "\\[[^]]*\\]")) {
            decl = substr(l, RSTART, RLENGTH)
            if (decl !~ okname)
              finding("R6: " rel ":" NR ": " moved[m] " holds a path in a buffer the owner did not size")
          }
        }
      }
      if (!is_owner && (index(line, "_MAX") || index(line, "_SIZE") || index(line, "strncpy") ||
                        index(line, "path is") || index(line, "1024"))) {
        l = tight(line)
        if (l ~ /(^|[^A-Za-z0-9_])(PATH_MAX|_POSIX_PATH_MAX|NAME_MAX)([^A-Za-z0-9_]|$)/)
          finding("R8: " rel ":" NR ": the system number of a path or a name is named outside " owner)
        # the private numbers: one walk over the words of the line, not a
        # regular expression of its own for each of them
        nt = split(l, toks, /[^A-Za-z0-9_]+/)
        for (k = 1; k <= nt; k++) if (toks[k] in private_number) private_hit[toks[k]] = NR
        for (i = 1; i <= nid; i++)
          if (private_hit[ids[i]] == NR)
            finding("R10: " rel ":" NR ": a private path number is back: " ids[i])
        if (l ~ /(^|[^A-Za-z0-9_])char[ \t]+path\[1024\]/)
          finding("R10: " rel ":" NR ": a path from the model is held in 1024 bytes and cut: take it whole (strndup)")
        if (l ~ /strncpy[ \t]*\([ \t]*(g_lsp_file|g_dap_progdir|g_dap_stop_file|e_d_file|ck_dir)[^A-Za-z0-9_]/)
          finding("R10: " rel ":" NR ": a path is cut by strncpy into a buffer the owner sized")
        if (l ~ /"[^"]*(path is longer than|path is too long)[^"]*"/) {
          if (rel in sent) print "SU\t" rel
          else finding("R10: " rel ":" NR ": a sentence about a path\047s length of its own: say wpe_path_say_too_long or wpe_path_say_name_too_long")
        }
      }
    }

    # def_line(line) - the DEF fact of a #define line.
    function def_line(line,   s, nm, v) {
      if (line !~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/) return
      s = line; sub(/^[ \t]*#[ \t]*define[ \t]+/, "", s)
      nm = s; sub(/[^A-Za-z0-9_].*$/, "", nm)
      v = substr(s, length(nm) + 1); sub(/^[ \t]+/, "", v)
      print "DEF\t" rel "|" NR "|" nm "|" v
    }

    # r9_copy_line(line) - the copies of one line of a .c file, counted
    # against the function they are in.  A function starts at a line in column
    # 0 that names an identifier followed by "(" and does not end with ";" (a
    # prototype), as in check-start-plan.sh.
    function r9_copy_line(line,   s, nm) {
      if (line ~ /^[A-Za-z_]/ && line ~ /[(]/ && line !~ /;[ \t]*$/ && line !~ /^(typedef|struct[^(]*[{])/) {
        nm = line; sub(/[ \t]*[(].*/, "", nm); sub(/.*[^A-Za-z0-9_]/, "", nm)
        if (nm != "") fn_copy = nm
      }
      if (index(line, "str") == 0 && index(line, "sprintf") == 0) return
      s = line
      while (match(s, /(^|[^A-Za-z0-9_])(strcpy|strcat|strncpy|sprintf)[ \t]*[(]/)) {
        cnt_copy[fn_copy]++; s = substr(s, RSTART + RLENGTH)
      }
    }
    function r9_copy_end(   f) { for (f in cnt_copy) print "CPY\t" rel "|" f "|" cnt_copy[f] }

    # r9_scan_line(line) - the scans of one line, counted the same way.  A
    # call is read to the paren that closes it, however many lines it spans.
    function r9_scan_line(line,   s, nm) {
      if (!CALL_OPEN && line ~ /^[A-Za-z_]/ && line ~ /[(]/ && line !~ /;[ \t]*$/ && line !~ /^(typedef|struct[^(]*[{])/) {
        nm = line; sub(/[ \t]*[(].*/, "", nm); sub(/.*[^A-Za-z0-9_]/, "", nm)
        if (nm != "") fn_scan = nm
      }
      if (!CALL_OPEN && index(line, "scanf") == 0) return
      s = line
      while (1) {
        if (CALL_OPEN) {
          s = feed(s)
          if (CALL_OPEN) break
          cnt_scan[call_fn] += unbounded_in_call(CALL_BUF)
        } else {
          if (!match(s, /(^|[^A-Za-z0-9_])(scanf|fscanf|sscanf|vscanf|vfscanf|vsscanf)[ \t]*[(]/)) break
          s = substr(s, RSTART + RLENGTH)
          CALL_OPEN = 1; CALL_DEPTH = 1; CALL_BUF = ""; call_fn = fn_scan
        }
      }
    }
    function r9_scan_end(   f) { for (f in cnt_scan) if (cnt_scan[f] > 0) print "SCN\t" rel "|" f "|" cnt_scan[f] }

    # bare_err_name(code) - a guessing ERR_ name on its own, as in
    # "i = ERR_ACCFILE": the table entry is then reached through an index or an
    # alias of the table.
    function bare_err_name(code,   rest, nm) {
      if (index(code, "ERR_") == 0) return 0
      rest = code
      while (match(rest, /ERR_[A-Za-z0-9_]+/)) {
        nm = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
        if (nm in errname) return 1
      }
      return 0
    }
    # note_macro_classes(code) - a macro of a class is the same offence where
    # it is used: every word of the code that is the name of a classed macro
    # puts this function in that class.  A word of the code is a name the
    # macro could be, so the words are all the test needs.
    function note_macro_classes(code,   nt, toks, k, t, kk) {
      if (ntab == 0) return
      nt = split(code, toks, /[^A-Za-z0-9_]+/)
      for (k = 1; k <= nt; k++) {
        t = toks[k]
        if (!(t in mtab)) continue
        kk = mtab[t]
        if (index(kk, "t")) r11tab = 1
        if (index(kk, "c")) r11call = 1
        if (index(kk, "l")) r11lit = 1
        if (index(kk, "n")) r11named = 1
      }
    }
    # note_callees(code) - the names this function calls, once each.  Whether
    # one of them says a guessing cause is a question for the whole tree, so
    # the names are the fact.
    function note_callees(code,   rest, nm) {
      if (index(code, "(") == 0) return
      rest = code
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*[(]/)) {
        nm = substr(rest, RSTART, RLENGTH); sub(/[ \t]*[(]$/, "", nm)
        rest = substr(rest, RSTART + RLENGTH)
        if (nm != fn11 && !(nm in callee)) { callee[nm] = 1; callees = callees " " nm }
      }
    }
    # r11_line(line) - what rule 11 learns from one line of a .c file.  What a
    # #define holds counts where the macro is used, not here, so a define and
    # the lines it continues are not read as code.
    function r11_line(out,   code, nm, k) {
      if (indef) { indef = (out ~ /\\[ \t]*$/); return }
      if (out ~ /^[ \t]*#[ \t]*define/) { indef = (out ~ /\\[ \t]*$/); return }
      if (out ~ /^}/) { r11_emit(); fn11 = ""; return }
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
        nm = out; sub(/[ \t]*[(].*/, "", nm); sub(/.*[^A-Za-z0-9_]/, "", nm)
        if (nm != "") {
          r11_emit(); fn11 = nm
          r11call = 0; r11lit = 0; r11named = 0; r11proj = 0; r11tab = 0
          r11mention = 0; r11errbare = 0; callees = ""
          for (k in callee) delete callee[k]
        }
      }
      if (fn11 == "") return
      code = out; gsub(/"([^"\\]|\\.)*"/, "\"\"", code)
      if (has_path_call(code)) r11call = 1
      if (code ~ /(^|[^A-Za-z0-9_])(errno|strerror)([^A-Za-z0-9_]|$)/) r11named = 1
      if (out ~ /"[^"]*([Nn]ot found|Can[\\\047]*t open|[Cc]annot open)[^"]*"/) r11lit = 1
      if (out ~ /"[^"]*(Project file not found|Cannot open project)[^"]*"/) r11proj = 1
      if (table_entry_said(code)) r11tab = 1
      note_macro_classes(code)
      if (code ~ /(^|[^A-Za-z0-9_])(e_msg|e_p_msg|e_d_msg)([^A-Za-z0-9_]|$)/) r11mention = 1
      if (bare_err_name(code)) r11errbare = 1
      note_callees(code)
    }
    # r11_emit() - the FN fact of the function just read.  A function that
    # knows nothing and calls nothing can say nothing, so it is no fact.
    function r11_emit(   flags) {
      if (fn11 == "") return
      if (r11mention && r11errbare) r11tab = 1
      flags = ""
      if (r11call) flags = flags "c"
      if (r11lit) flags = flags "l"
      if (r11named) flags = flags "n"
      if (r11tab) flags = flags "t"
      if (r11proj) flags = flags "p"
      if (flags == "" && callees == "") return
      sub(/^ /, "", callees)
      print "FN\t" rel "\t" fn11 "\t" flags "\t" callees
    }

    # second_arg(s, p) - the second top level argument of the call whose "("
    # comes just before position @p of @s.
    function second_arg(s, p,   depth, i, n, c, in_s, arg, seen, ch) {
      depth = 0; in_s = 0; seen = 0; arg = ""
      n = split(s, ch, "")
      for (i = p; i <= n; i++) {
        c = ch[i]
        if (in_s) {
          if (seen == 1) arg = arg c
          if (c == "\\") { i++; if (seen == 1) arg = arg ch[i] }
          else if (c == "\"") in_s = 0
          continue
        }
        if (c == "\"") { in_s = 1; if (seen == 1) arg = arg c; continue }
        if (c == "(" || c == "{" || c == "[") depth++
        else if (c == ")" || c == "}" || c == "]") { if (depth == 0) break; depth-- }
        else if (c == "," && depth == 0) { seen++; if (seen == 2) break; continue }
        if (seen == 1) arg = arg c
      }
      return arg
    }
    # Is the first "(" of the text closed by its last ")" and by no earlier one?
    function wrapped_whole(a,   i, n, c, depth, ch) {
      n = split(a, ch, ""); depth = 0
      for (i = 1; i <= n; i++) {
        c = ch[i]
        if (c == "(") depth++
        else if (c == ")") { depth--; if (depth == 0 && i < n) return 0 }
      }
      return depth == 0
    }
    # The argument without the casts and parentheses around it.
    function bare_arg(a,   prev) {
      do {
        prev = a
        gsub(/^[ \t\n]+|[ \t\n]+$/, "", a)
        if (a ~ /^[(][ \t]*(const[ \t]+)?[A-Za-z_][A-Za-z0-9_ \t]*[*]+[ \t]*[)]/) sub(/^[(][^)]*[)][ \t]*/, "", a)
        else if (a ~ /^[(].*[)]$/ && wrapped_whole(a)) a = substr(a, 2, length(a) - 2)
      } while (a != prev)
      return a
    }
    # Does this argument name a window no file backs?  A literal other than
    # the views the editor knows, a macro that leads to one (a macro of a
    # macro, a cast or parentheses on the way), a name ending in _NAME, either
    # branch of a ?: , or a member of a structure the file fills with one.
    function names_a_window(a,   hops, q, i, t, n, depth, c, qpos, cpos, ch) {
      a = bare_arg(a)
      for (hops = 0; (a in objlit) && hops < 8; hops++) a = bare_arg(objlit[a])
      # x ? "A" : "B": each branch is judged
      n = split(a, ch, ""); depth = 0; qpos = 0; cpos = 0; q = 0
      for (i = 1; i <= n; i++) {
        c = ch[i]
        if (c == "\"") { q = !q; continue }
        if (q) continue
        if (c == "(" || c == "[") depth++
        else if (c == ")" || c == "]") depth--
        else if (depth == 0 && c == "?" && !qpos) qpos = i
        else if (depth == 0 && c == ":" && qpos && !cpos) cpos = i
      }
      if (qpos && cpos)
        return names_a_window(substr(a, qpos + 1, cpos - qpos - 1)) || names_a_window(substr(a, cpos + 1))
      if (a ~ /^"/) return a != "\"\"" && a != "\"Help\"" && a != "\"Messages\"" && a != "\"Watches\"" && a != "\"Stack\""
      if (a ~ /^[A-Z][A-Z0-9_]*_NAME$/) return 1
      if (a ~ /^[A-Za-z_][A-Za-z0-9_]*(\[[^]]*\])?(\.|->)[A-Za-z_]/) return member_holds_a_window_name(a)
      return 0
    }
    # a.f or a->f: does the file give a an initializer, or a.f an assignment,
    # that holds a literal which names a window?
    function member_holds_a_window_name(a,   base, rest, lit, seg, p) {
      base = a; sub(/(\[|\.|->).*/, "", base)
      rest = txt
      while (match(rest, "(^|[^A-Za-z0-9_])" base "[^;{]*=[ \t\n]*[{]")) {
        seg = substr(rest, RSTART + RLENGTH - 1)
        p = index(seg, ";"); if (p) seg = substr(seg, 1, p)
        if (a_literal_names_a_window(seg)) return 1
        rest = substr(rest, RSTART + RLENGTH)
      }
      rest = txt
      while (match(rest, "(^|[^A-Za-z0-9_])" base "(\\.|->)[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*\"")) {
        seg = substr(rest, RSTART + RLENGTH - 1)
        p = index(seg, ";"); if (p) seg = substr(seg, 1, p)
        if (a_literal_names_a_window(seg)) return 1
        rest = substr(rest, RSTART + RLENGTH)
      }
      return 0
    }
    function a_literal_names_a_window(seg,   rest, lit) {
      rest = seg
      while (match(rest, /"([^"\\]|\\.)*"/)) {
        lit = substr(rest, RSTART, RLENGTH); rest = substr(rest, RSTART + RLENGTH)
        if (lit != "\"\"" && lit != "\"Help\"" && lit != "\"Messages\"" && lit != "\"Watches\"" && lit != "\"Stack\"") return 1
      }
      return 0
    }
    # r14_end() - rule 14 over the whole text of one .c file: a window the
    # editor writes opens through e_window_view_open.  A macro that wraps
    # e_edit is read as e_edit, a macro that is a string literal is read as
    # that literal, and a mention of e_edit that is not a call (a pointer to
    # it) is named, since the call through it cannot be read.
    function r14_end(   k, nm, rest, base, st, arg, pre, ln, plain) {
      for (k = 1; k <= nn; k++) {
        nm = names[k]
        rest = txt; base = 0
        while (match(rest, "(^|[^A-Za-z0-9_])" nm "[ \t]*[(]")) {
          st = RSTART + RLENGTH
          arg = second_arg(rest, st)
          pre = substr(txt, 1, base + st); ln = gsub(/\n/, "\n", pre) + 1
          if (names_a_window(arg))
            finding("R14: " rel ":" ln ": " arg " names a window the editor writes: open it with e_window_view_open, which never reads the disk")
          base += st - 1; rest = substr(rest, st)
        }
      }
      plain = txt; gsub(/"([^"\\]|\\.)*"/, "\"\"", plain); gsub(/e_edit[ \t]+[(]/, "e_edit(", plain)
      rest = plain; base = 0
      while (match(rest, /(^|[^A-Za-z0-9_.>])e_edit([^A-Za-z0-9_(]|$)/)) {
        st = RSTART + RLENGTH
        pre = substr(plain, 1, base + st); ln = gsub(/\n/, "\n", pre) + 1
        finding("R14: " rel ":" ln ": e_edit is named without a call (a pointer to it): the name of the window it opens cannot be read, call e_edit or e_window_view_open directly")
        base += st - 1; rest = substr(rest, st)
      }
    }

    # declare(name, size, fn) - a char array this file declares.
    function declare(name, size, fn,    tbl) {
      gsub(/[ \t]+/, "", size)
      if (fn == "(file scope)") { fsize[name] = size; ffn[name] = fn; return }
      lsize[name] = size; lfn[name] = fn
    }
    # declarations(t, fn) - the char arrays a declaration statement makes
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
    # check_args(s, from, fname, ids, ln) - the arguments of the call of
    # @fname whose "(" comes just before @from: an array this file declared is
    # a fact for the tree rule, which alone knows every define of the tree and
    # so alone can say whether the owner sized it.
    function check_args(s, from, fname, ids, ln,    n, k, nm, size, fn) {
      n = arg_split(s, from)
      for (k = 1; k <= n; k++) {
        if (index(ids, " " (k - 1) " ") == 0) continue
        nm = array_named(ARGS[k])
        if (nm == "") continue
        if (nm in lsize) { size = lsize[nm]; fn = lfn[nm] }
        else if (nm in fsize) { size = fsize[nm]; fn = ffn[nm] }
        else continue
        print "AH\t" rel "\t" ln "\t" fname "\t" nm "\t" size "\t" fn
      }
    }
    # sink_calls(s, ln) - every call of the statement @s whose name takes a
    # path: one walk over the words of the statement, not one walk per name.
    function sink_calls(s, ln,    rest, start, nm, before) {
      rest = s
      while (match(rest, /[A-Za-z_][A-Za-z0-9_]*[ \t]*[(]/)) {
        start = RSTART + RLENGTH
        nm = substr(rest, RSTART, RLENGTH); sub(/[ \t]*[(]$/, "", nm)
        before = (RSTART > 1) ? substr(rest, RSTART - 1, 1) : ""
        if (before !~ /[A-Za-z0-9_.>]/ && (nm in sink)) check_args(rest, start, nm, sink[nm], ln)
        rest = substr(rest, start)
      }
    }
    # rule12_stmt(s, ln) - the statements of one file are read in order with
    # the brace depth, so an array is known from its declaration to the end of
    # its function (a file scope array to the end of the file).  R13 is in the
    # same walk.
    function rule12_stmt(s, ln,    t, opens, closes, k) {
      t = s; sub(/^[ \t]+/, "", t); sub(/[ \t]+$/, "", t)
      if (t ~ /^#/) return
      opens = (t ~ /\{$/); closes = (t ~ /\}$/)
      if (r12depth == 0) {
        if (opens && t !~ /=/ && t ~ /\(/) {
          r12fn = t; sub(/[ \t]*\(.*/, "", r12fn); sub(/.*[^A-Za-z0-9_]/, "", r12fn)
          print "FD\t" rel "\t" r12fn
          for (k in lsize) delete lsize[k]
          r12infn = 1
        } else if (t == "{" && r12pending != "") {
          r12fn = r12pending
          print "FD\t" rel "\t" r12fn
          for (k in lsize) delete lsize[k]
          r12infn = 1
        } else if (!opens) {
          if (t ~ /\(/ && t !~ /=/ && t !~ /^typedef/) {
            r12pending = t; sub(/[ \t]*\(.*/, "", r12pending); sub(/.*[^A-Za-z0-9_]/, "", r12pending)
          }
          declarations(t, "(file scope)")
        }
      } else if (r12infn) {
        declarations(t, r12fn)
        # The cutting quoter: a path quoted by it into a buffer too small is
        # cut into the path of another file.  Only ws_quote_whole and the
        # tools (which quote the words of the model, not a path) call it.
        if (t ~ /(^|[^A-Za-z0-9_.>])wpe_ai_shell_quote[ \t]*\(/ && r12fn != "ws_quote_whole" && r12fn !~ /^ai_tool_/) {
          if ((rel "|" r12fn) in qok) print "QU\t" rel "\t" r12fn
          else finding("R13: " rel ":" ln ": " r12fn " calls wpe_ai_shell_quote, which cuts: quote a path with wpe_ai_shell_quote_whole, or list the function as a QUOTE line")
        }
        if (t ~ /\(/ && (length(lsize) > 0 || length(fsize) > 0)) sink_calls(t, ln)
      }
      if (opens) r12depth++
      else if (closes) { r12depth--; if (r12depth <= 0) { r12depth = 0; r12infn = 0 } }
    }

    BEGIN {
      is_c = (rel ~ /\.c$/)
      is_owner = (rel == owner)
      rel_names_r2 = (index(rel, "->txt") || index(rel, "str") || index(rel, "mem") ||
                      index(rel, "sprintf") || index(rel, "stpcpy"))
      r2fn = "(^|[^A-Za-z0-9_>.])(strcpy|strcat|strncpy|strncat|sprintf|memcpy|memmove|stpcpy)[ \t]*\\("
      r3names = "(wpe_path_copy|wpe_path_join|wpe_path_cat|wpe_field_take|e_opt_field_take)"
      r3alone = "^" r3names "[ \t]*\\("
      r3voided = "^\\([ \t]*void[ \t]*\\)[ \t]*" r3names "[ \t]*\\("
      # the body of a for: the header is cut at its semicolons, so the body
      # follows the ")" that closes it, with no "(" before
      r3forbody = "^[^(]*\\)[ \t]*" r3names "[ \t]*\\("
      r10word = "(^|[^A-Za-z0-9_])"
      r10endw = "([^A-Za-z0-9_]|$)"
      r10numbers = r10word "(WPE_PATH_MAX|WPE_NAME_MAX|WPE_PLATFORM_PATH_MAX|WPE_PLATFORM_NAME_MAX)" r10endw
      r10fmt = r10word "WPE_PATH_TOO_LONG_FMT" r10endw
      r10fn = r10word "(snprintf|sprintf)[ \t]*\\("
      # the sizes a moved path buffer may be declared with
      okname = "(WPE_PATH_SIZE|WPE_NAME_SIZE)"
      nmoved = split("filen prj_path cwd_saved saved_cwd xdgfile legacy libpath dirtmp exe", moved, " ")
      nid = split("WPE_BUILD_PATH_MAX E_EXECUTE_PATH_MAX E_D_COMPILE_PATH_MAX E_LSP_PATH_MAX E_DAP_FRAME_PATH_MAX HOST_PATH_MAX E_GDBMI_PATH_MAX E_SCALA_SETUP_DIR_MAX E_LSP_ROOT_MAX WPE_SYNTAX_PATH_MAX AI_TRANSCRIPT_PATH_MAX CONTAIN_TOKEN_MAX WLF_NAME_SIZE", ids, " ")
      for (i = 1; i <= nid; i++) private_number[ids[i]] = 1
      # the libc calls that take a path, and which arguments (0 based) are one
      split("stat:0 lstat:0 open:0 openat:1 fopen:0 freopen:0 access:0 chdir:0 mkdir:0 rmdir:0 unlink:0 remove:0 rename:0,1 link:0,1 symlink:0,1 readlink:0,1 realpath:0,1 getcwd:0 opendir:0 mkstemp:0 mkostemp:0 mkdtemp:0 truncate:0 chmod:0 execv:0 execve:0 execvp:0 statvfs:0 fstatat:1 wpe_path_copy:0 wpe_path_join:0 wpe_path_cat:0 wpe_path_search_in:2 wpe_read_open:0 wpe_read_stream_open:0 wpe_read_text:0 wpe_read_folder:0 wpe_read_name_ok:0", sk, " ")
      for (i in sk) {
        split(sk[i], kv, ":"); gsub(/,/, " ", kv[2])
        sink[kv[1]] = " " kv[2] " "
      }
      fn_copy = "(file scope)"; fn_scan = "(file scope)"; CALL_OPEN = 0
      naliases = 0; r12depth = 0; r12infn = 0; r12pending = ""
      # the arrays rule 12 fills: named as arrays before any length() of them
      split("", lsize); split("", fsize)
      # the entries whose words guess a cause, and the name of each
      load_set(tw, tabw)
      for (k in tabw) { en = k; sub(/^[^|]*[|]/, "", en); errname[en] = 1 }
      # the macro classes of rule 11, as the letters of one string a name
      ntab = 0
      while ((getline mline < mc) > 0) {
        mk = mline; sub(/[|].*/, "", mk); mn = substr(mline, length(mk) + 2)
        letter = (mk == "tab") ? "t" : (mk == "call") ? "c" : (mk == "lit") ? "l" : "n"
        if (!(mn in mtab)) ntab++
        mtab[mn] = mtab[mn] letter
      }
      close(mc)
      # what rule 14 reads a macro as: an object-like macro stands for its
      # body, a macro that wraps e_edit is read as e_edit
      names[1] = "e_edit"; nn = 1
      while ((getline eline < em) > 0) {
        split(eline, ep, "\t")
        if (ep[1] == "obj") objlit[ep[2]] = ep[3]
        else if (ep[1] == "wrap") names[++nn] = ep[2]
      }
      close(em)
      # the allow file: what rules 10, 12 and 13 let stand.  A line that does
      # not parse is the tree rule'"'"'s to name, once.
      while ((getline aline < allowfile) > 0) {
        na = split(aline, af, "|")
        if (af[1] == "SENTENCE" && na >= 2) sent[af[2]] = 1
        else if (af[1] == "PATHFN" && na >= 5 && af[4] ~ /^[0-9]+$/ && af[5] != "")
          sink[af[3]] = sink[af[3]] " " af[4] " "
        else if (af[1] == "QUOTE" && na >= 4 && af[4] != "") qok[af[2] "|" af[3]] = 1
      }
      close(allowfile)
    }
    FNR == 1 { blank_incom = 0 }
    {
      out = blank_c($0, 1)
      line_rules(out)
      def_line(out)
      feed_statements(out)
      r9_scan_line(out)
      if (is_c) {
        r9_copy_line(out)
        r11_line(out)
        txt = txt out "\n"
      }
    }
    END {
      stmt_emit()
      r9_scan_end()
      if (is_c) { r9_copy_end(); r11_emit(); r14_end() }
    }' "$1"
}

# The rules that look at the whole tree, over the facts of every file: one
# definition, no other path number, the counts against the allow file, the
# sayer rounds of rule 11, and the arrays rule 12 saw handed to a path.
AGGREGATE='
# owned(name, depth) - is the number @name stands for one of the owners?
function owned(name, depth,    v, w) {
  v = val[name]
  if (v ~ /(^|[^A-Za-z0-9_])WPE_(PATH_MAX|PATH_SIZE|NAME_MAX|NAME_SIZE|PATH_FIELD_MAX|NAME_FIELD_MAX)([^A-Za-z0-9_]|$)/)
    return 1
  w = v; gsub(/[ \t()]/, "", w)
  if (depth < 8 && w ~ /^[A-Za-z_][A-Za-z0-9_]*$/ && (w in val))
    return owned(w, depth + 1)
  return 0
}
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
# rule1() - WPE_PATH_MAX and WPE_NAME_MAX are each defined once, in the
# owner'"'"'s header, as a number.
function rule1(    j, n1, n2, f) {
  for (j = 1; j <= ndef; j++) {
    split(defrec[j], f, "|")
    if (f[3] == "WPE_PATH_MAX") n1++
    else if (f[3] == "WPE_NAME_MAX") n2++
  }
  if (n1 + 0 != 1) print "R1: WPE_PATH_MAX is defined " n1 + 0 " times, not once"
  if (n2 + 0 != 1) print "R1: WPE_NAME_MAX is defined " n2 + 0 " times, not once"
  for (j = 1; j <= ndef; j++) {
    split(defrec[j], f, "|")
    if (f[3] != "WPE_PATH_MAX" && f[3] != "WPE_NAME_MAX") continue
    if (f[1] != owner) { print "R1: " f[3] " is defined in " f[1] ":" f[2] ", not in " owner; continue }
    if (f[4] !~ /^[0-9]+([ \t].*)?$/) print "R1: " f[3] " in " owner " is not a number: " f[4]
  }
}
# rule7() - no other path number.  The names that end like one and are not a
# path are listed in the allow file with their reason, and a line that matches
# no define is stale.
function rule7(    j, f, rel7, ln7, nm7, k) {
  for (j = 1; j <= ndef; j++) {
    split(defrec[j], f, "|"); rel7 = f[1]; ln7 = f[2]; nm7 = f[3]
    if (rel7 == owner) continue
    if (nm7 == "PATH_MAX" || nm7 == "NAME_MAX" || nm7 == "_POSIX_PATH_MAX") {
      print "R7: " rel7 ":" ln7 ": " nm7 " is defined outside " owner; continue
    }
    if (nm7 !~ /(PATH_MAX|PATHMAX|PATH_SIZE|DIR_MAX|FOLDER_MAX|NAME_MAX)$/) continue
    if (nm7 in ok7) { used7[nm7] = 1; continue }
    if (!owned(nm7, 0))
      print "R7: " rel7 ":" ln7 ": " nm7 " is a path number of its own: derive it from WPE_PATH_SIZE, WPE_PATH_MAX or WPE_NAME_MAX, or list it as not a path"
  }
  for (k in ok7) if (!(k in used7))
    print "R7: stale allow line: no define of " k " ends like a path number"
}
# rule9copies() - the copies counted per function, against the allow file.  A
# count may only fall, and a line the scan does not meet is stale.
function rule9copies(    j, f, key, k) {
  for (j = 1; j <= ncpy; j++) {
    split(cpy[j], f, "|")
    key = f[1] "|" f[2]; got9[key] = 1
    if (listpre != "") print cpy[j] > (listpre ".copies")
    if (!(key in al9))
      print "R9: " f[1] ": " f[2] " has " f[3] " strcpy, strcat, strncpy or sprintf call(s) nobody has judged: move them onto the path owner, or list them with their kind"
    else if (f[3] + 0 > al9[key] + 0)
      print "R9: " f[1] ": " f[2] " has " f[3] " call(s), the allow file says " al9[key]
    else if (f[3] + 0 < al9[key] + 0)
      print "R9: " f[1] ": " f[2] " has " f[3] " call(s), lower the allow line from " al9[key]
  }
  for (k in al9) if (!(k in got9)) print "R9: stale allow line, delete it: " k
}
# rule9scans() - the same for a scan that reads a word with %s or %[ and no
# width, against the SCAN lines.
function rule9scans(    j, f, key, k) {
  for (j = 1; j <= nscn; j++) {
    split(scn[j], f, "|")
    key = f[1] "|" f[2]; got9s[key] = 1
    if (listpre != "") print scn[j] > (listpre ".scans")
    if (!(key in al9s))
      print "R9: " f[1] ": " f[2] " reads " f[3] " word(s) with %s or %[ and no width: bound the scan by its buffer (a width, or a line reader and wpe_field_take), or list it as a SCAN line with its reason"
    else if (f[3] + 0 > al9s[key] + 0)
      print "R9: " f[1] ": " f[2] " reads " f[3] " unbounded word(s), the allow file says " al9s[key]
    else if (f[3] + 0 < al9s[key] + 0)
      print "R9: " f[1] ": " f[2] " reads " f[3] " unbounded word(s), lower the allow line from " al9s[key]
  }
  for (k in al9s) if (!(k in got9s)) print "R9: stale allow line, delete it: " k
}
# rule10stale() - a SENTENCE line no file used.
function rule10stale(    k) {
  for (k in sent) if (!(k in sentused))
    print "R10: stale allow line: " k " has no sentence about a path\047s length of its own"
}
# rule11() - the sayers: the functions that say a guessing table entry,
# wherever they are (a helper in another file that says it after the caller
# stat), and the functions that call one, however many levels up, minus those
# an allow line has judged.  Each round reads every function with the sayers
# the round before found, until no new sayer appears; the last round'"'"'s rows
# are the words.
function rule11(    rounds, i, k, saycall, tabhit, key, changed, nnew, tmp) {
  for (i = 1; i <= nfn; i++) {
    fcall[i] = (index(fflags[i], "c") > 0)
    flit[i] = (index(fflags[i], "l") > 0)
    fnamed[i] = (index(fflags[i], "n") > 0)
    ftab[i] = (index(fflags[i], "t") > 0)
    fproj[i] = (index(fflags[i], "p") > 0)
    ncal[i] = split(fcal[i], tmp, " ")
    for (k = 1; k <= ncal[i]; k++) cal[i, k] = tmp[k]
  }
  nsayer = 0; rounds = 0
  while (rounds < 6) {
    rounds++
    for (k in sayernew) delete sayernew[k]
    nnew = 0
    for (i = 1; i <= nfn; i++) {
      saycall = 0
      for (k = 1; k <= ncal[i]; k++) if (cal[i, k] in sayer) { saycall = 1; break }
      tabhit = ftab[i] || saycall
      rowS[i] = tabhit
      rowP[i] = fproj[i]
      rowG[i] = (fcall[i] && flit[i] && !fnamed[i])
      rowT[i] = (fcall[i] && tabhit)
      # a sayer'"'"'s rows stay in the words, so an allow line that judges one is
      # not stale; only the callers it has are named
      if (tabhit && !(ffn[i] in gsfn)) { sayernew[ffn[i]] = 1; nnew++ }
    }
    changed = (nnew != nsayer)
    if (!changed) for (k in sayernew) if (!(k in sayer)) { changed = 1; break }
    for (k in sayer) delete sayer[k]
    for (k in sayernew) sayer[k] = 1
    nsayer = nnew
    if (!changed) break
  }
  for (i = 1; i <= nfn; i++) {
    key = frel[i] "|" ffn[i]
    if (rowG[i]) {
      if (listpre != "") print "G|" key > (listpre ".words")
      gsseen[key] = 1
      if (!(key in gs))
        print "R11b: " frel[i] ": " ffn[i] " asks the system about a path and then says a cause it guessed (not found, cannot open) without the system\047s errno or strerror"
    }
    if (rowP[i]) {
      if (listpre != "") print "P|" key > (listpre ".words")
      pwseen[key] = 1
      if (!(key in pw))
        print "R11a: " frel[i] ": " ffn[i] " says the project did not open in words of its own: the reporter e_prj_say_not_opened is the one place for them"
    }
    if (rowS[i]) {
      if (listpre != "") print "S|" key > (listpre ".words")
      gsseen[key] = 1
    }
    if (rowT[i]) {
      if (listpre != "") print "T|" key > (listpre ".words")
      gsseen[key] = 1
      if (!(key in gs))
        print "R11c: " frel[i] ": " ffn[i] " asks the system about a path and says it with a message table entry whose words guess a cause: say the system\047s reason (e_say_path_refused, e_say_read_refused)"
    }
  }
  for (k in pw) if (!(k in pwseen)) print "R11a: stale allow line, delete it: PROJECTWORDS " k
  for (k in gs) if (!(k in gsseen)) print "R11b: stale allow line, delete it: GUESS " k
}
# rule12() - the arrays the walk saw handed to a path, judged now that every
# define of the tree is known, and the allow lines that no longer match.
function rule12(    j, f, size, key, k, p) {
  for (j = 1; j <= nah; j++) {
    split(ahrec[j], f, "\t")
    size = f[6]
    if (sized_by_owner(size)) continue
    key = f[2] "|" f[7] "|" f[5]
    if (key in ex) { exused[key] = 1; continue }
    print "R12: " f[2] ":" f[3] ": " f[5] "[" size "] is passed to " f[4] " as a path: size it WPE_PATH_SIZE, WPE_PATH_MAX or WPE_NAME_SIZE, or list it as an ARRAY line"
  }
  for (k in pathfn) {
    split(k, p, "|")
    if (!(k in defs)) print "R12x: stale allow line, delete it: PATHFN " p[1] " " p[2] " (no such function in the file)"
  }
  for (k in ex) if (!(k in exused)) print "R12x: stale allow line, delete it: ARRAY " k
  for (k in qok) if (!(k in qused)) print "R12x: stale allow line, delete it: QUOTE " k
}
BEGIN {
  FS = "\t"
  if (listpre != "") {
    printf "" > (listpre ".copies")
    printf "" > (listpre ".scans")
    printf "" > (listpre ".words")
  }
  # the allow file: what rules 7, 9, 10, 11 and 12 let stand
  while ((getline aline < allowfile) > 0) {
    na = split(aline, f, "|")
    if (f[1] == "NUMBER") ok7[f[2]] = 1
    else if (f[1] == "COPY") {
      if (na < 6 || f[4] !~ /^[0-9]+$/) { print "R9: malformed allow line: " aline; continue }
      if (f[5] !~ /^(sized|text|checked)$/) {
        print "R9: allow line for " f[2] " " f[3] " is of kind " f[5] ", not sized, text or checked: a path goes through the owner"; continue
      }
      al9[f[2] "|" f[3]] = f[4]; kind9[f[2] "|" f[3]] = f[5]
    } else if (f[1] == "SCAN") {
      if (na < 5 || f[4] !~ /^[0-9]+$/ || f[5] == "") { print "R9: malformed allow line: " aline; continue }
      al9s[f[2] "|" f[3]] = f[4]
    } else if (f[1] == "PROJECTWORDS") {
      if (na < 4) { print "R11a: malformed allow line: " aline; continue }
      pw[f[2] "|" f[3]] = 1
    } else if (f[1] == "GUESS") {
      if (na < 5 || f[4] == "") { print "R11b: malformed allow line: " aline; continue }
      gs[f[2] "|" f[3]] = 1; gsfn[f[3]] = 1
    } else if (f[1] == "SENTENCE") {
      sent[f[2]] = 1
    } else if (f[1] == "PATHFN") {
      if (na < 5 || f[4] !~ /^[0-9]+$/ || f[5] == "") { print "R12x: malformed allow line: " aline; continue }
      pathfn[f[2] "|" f[3]] = 1
    } else if (f[1] == "QUOTE") {
      if (na < 4 || f[4] == "") { print "R12x: malformed allow line: " aline; continue }
      qok[f[2] "|" f[3]] = 1
    } else if (f[1] == "ARRAY") {
      if (na < 5 || f[5] == "") { print "R12x: malformed allow line: " aline; continue }
      ex[f[2] "|" f[3] "|" f[4]] = 1
    }
  }
  close(allowfile)
  # a problem of reading the message tables is a finding: a scan that cannot
  # see the tables fails instead of passing blind
  while ((getline pline < twproblems) > 0) print pline
  close(twproblems)
}
{
  if ($1 == "F") { print substr($0, 3); next }
  if ($1 == "DEF") {
    rec = substr($0, 5)
    ndef++; defrec[ndef] = rec
    split(rec, f, "|")
    v = rec; sub(/^[^|]*\|[^|]*\|[^|]*\|?/, "", v)
    if (!(f[3] in val)) val[f[3]] = v
    next
  }
  if ($1 == "CPY") { ncpy++; cpy[ncpy] = substr($0, 5); next }
  if ($1 == "SCN") { nscn++; scn[nscn] = substr($0, 5); next }
  if ($1 == "SU") { sentused[$2] = 1; next }
  if ($1 == "FN") { nfn++; frel[nfn] = $2; ffn[nfn] = $3; fflags[nfn] = $4; fcal[nfn] = $5; next }
  if ($1 == "AH") { nah++; ahrec[nah] = $0; next }
  if ($1 == "FD") { defs[$2 "|" $3] = 1; next }
  if ($1 == "QU") { qused[$2 "|" $3] = 1; next }
}
END { rule1(); rule7(); rule9copies(); rule9scans(); rule10stale(); rule11(); rule12() }
'

# scan_tree DIR OUT [ALLOW] - run the rules on the top level sources under
# DIR; the findings go to OUT, one per line, each starting with its rule.
# ALLOW is the file that lists what rules 7, 9, 10, 12 and 13 let stand; a
# canary's tree has none, so anything the rule meets is an offender there.
# With --list the copy calls, the unbounded scans and the guessing functions
# found are left in $WORK/last-copies, last-scans and last-words.
scan_tree() {
  dir=$1; out=$2; allow=${3:-/dev/null}
  t=$(mktemp -d "$WORK/scan.XXXXXX")
  mkdir "$t/f"
  : > "$out"
  # TABLES_FROM is where rule 11 reads the message tables; a canary points it
  # at the real tree, and the answer is kept per folder.
  table_words "${TABLES_FROM:-$dir}"
  macro_facts "$dir" "$TW" "$t/macroclass" "$t/editmacros"
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    scan_file "$f" "$rel" "$allow" "$t/macroclass" "$t/editmacros" "$TW" > "$t/f/$rel"
  done
  [ -f "$dir/$OWNER_HEADER" ] ||
    echo "R1: $OWNER_HEADER is missing (the scan is blind or the owner is gone)" >> "$out"
  st_list=
  if [ "$LIST" -eq 1 ]; then st_list=$t/list; fi
  awk -v allowfile="$allow" -v owner="$OWNER_HEADER" -v twproblems="$TW_PROBLEMS" \
      -v listpre="$st_list" "$AGGREGATE" "$t/f"/*.c "$t/f"/*.h >> "$out"
  if [ "$LIST" -eq 1 ]; then
    sort "$t/list.copies" > "$WORK/last-copies"
    sort "$t/list.scans" > "$WORK/last-scans"
    sort "$t/list.words" > "$WORK/last-words"
  fi
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
  # a second source, for a helper that lives in another file than its caller
  [ -z "$PLANT_CODE2" ] || printf '%s\n' "$PLANT_CODE2" > "$c/we_planted2.c"
  : > "$c/allow"
  [ -z "$lines" ] || printf '%s\n' "$lines" > "$c/allow"
  # The planted tree has no message tables: rule 11 reads the real ones, so
  # a planted e_msg[ERR_ACCFILE] is judged by what the table says of it.
  TABLES_FROM=$SRCDIR
  scan_tree "$c" "$c/out" "$c/allow"
  TABLES_FROM=
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
# Rule 11, the chdir of a folder and the words of the message tables.
canary R11b 'void planted(FENSTER *f, const char *p) { if (chdir(p) != 0) e_error("Can\'"'"'t open X", 0, f->fb); }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_error(e_msg[ERR_ACCFILE], 0, f->fb); }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { if (chdir(p) != 0) e_error(e_msg[ERR_WORKDIRACCESS], 0, f->fb); }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) { int e = errno; e_error(e_msg[ERR_ACCFILE], 0, f->fb); } }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { if (access(p, 0) != 0) e_error(e_d_msg[ERR_CANTFILE], 0, f->fb); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_say_path_refused(f, "file", p, errno); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_error(e_msg[ERR_LOWMEM], 0, f->fb); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { if (access(p, 0) != 0) e_error(e_msg[ERR_COMMAND], 0, f->fb); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f) { e_error(e_msg[ERR_ACCFILE], 0, f->fb); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_error(e_msg[ERR_ACCFILE], 0, f->fb); }' 'GUESS|we_planted.c|planted|keep|the word is about something else' || blind=1
canary R11b 'void planted(FENSTER *f, const char *p) { if (stat(p, 0) != 0) e_error(e_msg[ERR_ACCFILE], 0, f->fb); }' 'GUESS|we_planted.c|gone|keep|stale' || blind=1
# Rule 14: a window the editor writes opens through e_window_view_open.
canary R14 'void planted(ECNT *cn) { e_edit(cn, "Doctor"); }' || blind=1
canary R14 'void planted(ECNT *cn) { if (e_edit(cn, AI_PANE_NAME)) return; }' || blind=1
canary R14 'void planted(ECNT *cn) { e_edit(
   cn,
   "Report"); }' || blind=1
quiet_canary R14 'void planted(ECNT *cn) { e_window_view_open(cn, WPE_PANE_REPORT, "Doctor", NULL); e_window_view_open(cn, WPE_PANE_COMPOSER, PROMPT_NAME, NULL); }' || blind=1
quiet_canary R14 'void planted(ECNT *cn) { e_edit(cn, "Messages"); e_edit(cn, "Help"); e_edit(cn, ""); e_edit(cn, "Watches"); e_edit(cn, "Stack"); }' || blind=1
quiet_canary R14 'void planted(ECNT *cn, char *w) { e_edit(cn, w); e_edit(cn, wpe_listsel_name(cn->df, cn->nf, "x")); }' || blind=1
quiet_canary R14 'void planted(struct json_object *resp) { lsp_apply_workspace_edit(obj_obj(resp, "result"), "x"); }' || blind=1
quiet_canary R14 'void planted(ECNT *cn) { /* e_edit(cn, "Doctor") */ }' || blind=1
# Rule 14: a cast or parentheses, a macro around the call, a pointer to it,
# a macro that is the literal.
canary R14 'void planted(ECNT *cn) { e_edit(cn, (char *)"Doctor"); }' || blind=1
canary R14 'void planted(ECNT *cn) { e_edit(cn, ("Doctor")); }' || blind=1
canary R14 'void planted(ECNT *cn) { e_edit(cn, (char *)("Doctor")); }' || blind=1
canary R14 '#define OPEN_WIN(c, n) e_edit(c, n)
void planted(ECNT *cn) { OPEN_WIN(cn, "Doctor"); }' || blind=1
canary R14 'void planted(ECNT *cn) { int (*op)(ECNT *, char *) = e_edit; op(cn, "Doctor"); }' || blind=1
canary R14 '#define DOCTOR_TITLE "Doctor"
void planted(ECNT *cn) { e_edit(cn, DOCTOR_TITLE); }' || blind=1
quiet_canary R14 'void planted(ECNT *cn, char *title) { e_edit(cn, (char *)title); e_edit(cn, (char *)"Messages"); e_edit(cn, ("Stack")); }' || blind=1
quiet_canary R14 '#define OPEN_WIN(c, n) e_edit(c, n)
void planted(ECNT *cn, char *w) { OPEN_WIN(cn, w); OPEN_WIN(cn, "Help"); }' || blind=1
# Rule 11c: the bypasses of the table scan: a macro that says the entry, a
# macro with the whole check in it, a pointer to chdir, the pointer form of
# the table entry, and a helper in another file that says it after the stat.
canary R11c '#define NOACC e_msg[ERR_ACCFILE]
void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_error(NOACC, 0, f->fb); }' || blind=1
canary R11c '#define FAIL_OPEN(f, p) do { struct stat st_; if (stat(p, &st_)) e_error(e_msg[ERR_ACCFILE], 0, (f)->fb); } while (0)
void planted(FENSTER *f, const char *p) { FAIL_OPEN(f, p); }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { int (*go)(const char *) = chdir; if (go(p) != 0) e_error(e_msg[ERR_WORKDIRACCESS], 0, f->fb); }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_error(*(e_msg + ERR_ACCFILE), 0, f->fb); }' || blind=1
PLANT_CODE2='void say_noaccess(FENSTER *f) { e_error(e_msg[ERR_ACCFILE], 0, f->fb); }'
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) say_noaccess(f); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) say_noaccess(f); }' 'GUESS|we_planted2.c|say_noaccess|keep|a helper the allow file has judged' || blind=1
PLANT_CODE2=
# Second-order bypasses of rule 11c: a macro of a macro, a chain of helpers,
# a table indexed through a variable, a table reached through an alias.
canary R11c '#define NOACC e_msg[ERR_ACCFILE]
#define NOACC_TOO NOACC
void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) e_error(NOACC_TOO, 0, f->fb); }' || blind=1
PLANT_CODE2='void deep_say(FENSTER *f) { e_error(e_msg[ERR_ACCFILE], 0, f->fb); }
void mid_say(FENSTER *f) { deep_say(f); }'
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) mid_say(f); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; if (stat(p, &st) != 0) mid_say(f); }' 'GUESS|we_planted2.c|deep_say|keep|judged' || blind=1
PLANT_CODE2=
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; int i = ERR_ACCFILE; if (stat(p, &st) != 0) e_error(e_msg[i], 0, f->fb); }' || blind=1
canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; char **m = e_msg; if (stat(p, &st) != 0) e_error(m[ERR_ACCFILE], 0, f->fb); }' || blind=1
quiet_canary R11c 'void planted(FENSTER *f, const char *p) { struct stat st; char **m = e_msg; if (stat(p, &st) != 0) e_error(m[ERR_LOWMEM], 0, f->fb); }' || blind=1
# Second-order bypasses of rule 14: a ?: , a macro of a literal macro, a
# structure that holds the literal.
canary R14 'void planted(ECNT *cn, int x) { e_edit(cn, x ? "Doctor" : "Messages"); }' || blind=1
canary R14 'void planted(ECNT *cn, int x) { e_edit(cn, x ? "Stack" : "Doctor"); }' || blind=1
canary R14 '#define BASE_TITLE "Doctor"
#define DOCTOR_TITLE BASE_TITLE
void planted(ECNT *cn) { e_edit(cn, DOCTOR_TITLE); }' || blind=1
canary R14 'static const struct { const char *name; } win = { "Doctor" };
void planted(ECNT *cn) { e_edit(cn, win.name); }' || blind=1
canary R14 'struct W { const char *name; };
void planted(ECNT *cn, struct W *w) { w->name = "Doctor"; e_edit(cn, w->name); }' || blind=1
quiet_canary R14 'void planted(ECNT *cn, int x) { e_edit(cn, x ? "Stack" : "Messages"); }' || blind=1
quiet_canary R14 'struct W { const char *name; };
void planted(ECNT *cn, struct W *w) { e_edit(cn, w->name); }' || blind=1
# The owner of the file manager's change of folder says the system's reason:
# put back the guessed words and the scan names it.
canary R11c 'static void planted(ECNT *cn, FENSTER *f, const char *dir) { if (chdir(dir) == 0) return; e_error(e_msg[ERR_WORKDIRACCESS], 0, f->fb); }' || blind=1
quiet_canary R11c 'static void planted(ECNT *cn, FENSTER *f, const char *dir) { if (chdir(dir) == 0) return; e_say_path_refused(f, "folder", dir, errno); }' || blind=1
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
