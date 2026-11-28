#!/bin/sh
# check-question-typeahead.sh -- which keys may answer a question has one owner.
#
# A question (the review of a run's changes, the approval of a command or a
# write, the consent to enable the assistant) is answered only by a key pressed
# after it was shown.  The screen's owner (we_paint.c) reads a question's
# answer with wpe_paint_ask_key: on the first read it lets the display settle
# and drops what was typed before, through one entry of the display table.  A
# place to type or move (a menu, a picker, a field, the File Manager) keeps
# wpe_paint_modal_getch and its type-ahead.  A reader that picks neither is how
# a key typed while a run ended reverted the run.
#
#   check-question-typeahead.sh [--allow FILE] [--srcdir DIR] [--selftest]
#
# Rules, each with a canary: the script plants an offender in a copy of the
# sources and fails when the rule does not name it, so a scanner gone blind
# fails instead of passing.  Scope: the top-level sources, never tests/;
# comments are blanked.
#
#  a. Default deny.  A call of wpe_paint_modal_getch outside we_paint.c, of
#     the owner's reader for a box of either kind (wpe_paint_box_key), or of a
#     wrapper that reads for others (e_opt_getch, e_opt_read_key), is in a
#     function the allow file classifies.  A new reader fails the build until
#     its author says which kind it is.
#  b. A question reads only through the owner.  A function that holds a
#     WpeAsk (a variable or a parameter) or calls wpe_paint_ask_key calls no
#     other reader of keys: not the four of rule a, not wpe_paint_take_key, not
#     any function the allow file classifies (keeps, wraps or pending).  The
#     set follows the allow file, so no list is kept here.
#  c. The drop has one caller.  e_u_type_ahead_drop is called only by the desk
#     entry e_paint_settle_and_drop (we_edit.c); the three display functions
#     (e_t_, e_x_ and e_w_type_ahead_drop) are never called, only assigned to
#     the table; tcflush( and flushinp( appear nowhere: they go around the
#     terminal's one reader (a paste's end marker and a resize are lost).
#  d. A question is made, and measured, by its owner.  A WpeAsk is declared
#     with WPE_ASK( or WPE_ASK_SERIES( (a pointer is no declaration; a series
#     is a row of boxes the user answers on purpose and keeps its type-ahead,
#     so it is declared by name and only e_opt_kst_series does);
#     a variable named ask never
#     has .shown, .dropped or .pasted written outside we_paint.c.
#  e. Every display has the entry.  A top-level source that assigns a
#     display's key taker (e_u_key_take = e_t_key_take, e_x_..., e_w_...) also
#     assigns e_u_type_ahead_drop.
#  f. An allow line is alive.  The function an allow line names still calls
#     a reader.
#
# The allow file, tests/question-typeahead.allow, has lines
#   kind|function|reason
# where kind is keeps (a place to type or move: it keeps its type-ahead),
# wraps (reads for others; its callers are classified instead, or, at the end
# of one engine's chain, the reader below says whose key it is) or pending (a
# question that a later pass moves onto the owner; it leaves the file then).
# A line that matches nothing is stale and fails.
#
# How it is made fast: one awk pass per source file (scan_file) blanks the
# comments and applies the per-file rules, and writes the file's "facts" to
# the work folder: the reader calls it holds, the offences of rules b to e, and
# whether it is a display.  The facts of the real tree are made once.  A canary
# changes one file, so only that file is scanned again and its facts replace
# the cached ones (aggregate).  The rules that need the whole tree (a, e's
# count, f) read the facts, never the sources.
#
# Limits, said plainly: a scan of the source does not see a read reached
# through a function pointer; a WpeAsk variable not named ask escapes rule d's
# second half, and the rows of tests/test_question_typeahead.py are the net
# for that.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1
ALLOW=
SELFTEST=0
HERE=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-question-typeahead: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/question-typeahead.allow

OWNER=we_paint.c
OWNER_HEADER=we_paint.h
DESK_ENTRY_FILE=we_edit.c
DESK_ENTRY=e_paint_settle_and_drop
READERS='wpe_paint_modal_getch wpe_paint_box_key e_opt_getch e_opt_read_key'

. "$(dirname "$0")/workdir.sh"
work_dir question-typeahead $BAD
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")

# scan_file FILE BASE LIST - write to stdout the facts of one source file, in
# one pass.  BASE is the file's name (a canary scans a changed copy that keeps
# the name of the original), LIST the readers rule b forbids in a question.
# Records, tab separated:
#   R TAB file TAB line TAB function TAB reader   a reader call (rule a, f)
#   DISP TAB file                                  the file is a display (rule e)
#   X: text                                        an offence of rule b, c, d or e
# The function is the last one whose header, a line from column 0 holding a
# "(" and not ending in ";", was seen.  Comments are blanked, string literals
# kept, one output line per input line.
scan_file() {
  awk -v file="$2" -v owner="$OWNER" -v owner_header="$OWNER_HEADER" \
      -v entry_file="$DESK_ENTRY_FILE" -v entry="$DESK_ENTRY" \
      -v readers="$READERS" -v blist="$3" "$BLANK_C"'
    BEGIN {
      FS = " "
      n_a = split(readers, want_a, " ")
      n_b = split(blist, want_b, " ")
      is_c = (substr(file, length(file)) == "c")
      is_owner = (file == owner)
      fn = ""
    }
    FNR == 1 { blank_incom = 0 }

    # header_name - the function a header line defines.
    function header_name(line,   head, parts, k) {
      head = line
      sub(/\(.*/, "", head)
      sub(/[^A-Za-z0-9_]+$/, "", head)
      k = split(head, parts, /[^A-Za-z0-9_]+/)
      return parts[k]
    }

    # note_header - when the line is a function header, make it the current
    # function; return its name, or "" for any other line.
    function note_header(line) {
      if (line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ &&
          line !~ /^(typedef|struct|union|enum|extern)[ \t]/) {
        fn = header_name(line)
        return fn
      }
      return ""
    }

    # note_reader - a call of a reader (rule a and f facts).
    function note_reader(pat, rest) {
      printf "R\t%s\t%d\t%s\t%s\n", file, NR, fn, pat
    }

    # note_question_call - a call from the current function of a reader that a
    # question must not use (rule b); reported in END if the function holds a
    # question.
    function note_question_call(pat, rest) {
      if (fn == "") return
      calls[fn, ++ncalls[fn]] = NR "\t" pat
      fns[fn] = 1
    }

    # calls_of - call the handler (1 = reader, 2 = question call) for each
    # call of a name of LIST in the line, but not the definition of the
    # called function itself (own).
    function calls_of(line, want, n, own, handler,   j, pat, text, at, before, rest) {
      for (j = 1; j <= n; j++) {
        pat = want[j]; text = line
        while ((at = index(text, pat)) > 0) {
          before = at > 1 ? substr(text, at - 1, 1) : ""
          rest = substr(text, at + length(pat))
          if (before !~ /[A-Za-z0-9_]/ && rest ~ /^[ \t]*\(/ && pat != own) {
            if (handler == 1) note_reader(pat, rest)
            else note_question_call(pat, rest)
          }
          text = substr(text, at + length(pat))
        }
      }
    }

    # rule_b_line - a function holds a question when it has a WpeAsk, as a
    # variable or a parameter, or calls wpe_paint_ask_key.
    function rule_b_line(line, own) {
      if (fn == "") return
      if (line ~ /(^|[^A-Za-z0-9_])(WpeAsk[ \t*]|wpe_paint_ask_key[ \t]*\()/)
        holds[fn] = 1
      calls_of(line, want_b, n_b, own, 2)
    }

    # rule_c_line - the drop has one caller; tcflush and flushinp go around
    # the terminal reader.
    function rule_c_line(line) {
      if (line ~ /(^|[^A-Za-z0-9_])(tcflush|flushinp)[ \t]*\(/)
        print "C: tcflush or flushinp goes around the terminal reader: " file ":" NR
      if (line ~ /e_u_type_ahead_drop/ && line !~ /^[ \t]*(extern[ \t]+)?int[ \t]*\(\*e_u_type_ahead_drop\)/ &&
          line !~ /e_u_type_ahead_drop[ \t]*=[^=]/ && line ~ /e_u_type_ahead_drop\)?[ \t]*\(/ &&
          !(file == entry_file && fn == entry))
        print "C: the display drop is called outside " entry ": " file ":" NR
      if (line ~ /e_[txw]_type_ahead_drop[ \t]*\(/ && line !~ /^(static[ \t]+)?int[ \t]+e_[txw]_type_ahead_drop[ \t]*\(/)
        print "C: a display drop function is called, not assigned to the table: " file ":" NR
    }

    # rule_d_line - a question is made, and measured, by its owner.
    function rule_d_line(line) {
      if (line ~ /(^|[^A-Za-z0-9_])WpeAsk[ \t]+[A-Za-z_]/ && line !~ /WpeAsk[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*WPE_ASK(_SERIES)?\(/)
        print "D: a WpeAsk is not made with WPE_ASK or WPE_ASK_SERIES: " file ":" NR
      if (line ~ /WPE_ASK_SERIES[ \t]*\(/ && !(file == "we_opt.c" && fn == "e_opt_kst_series"))
        print "D: a series question is made outside e_opt_kst_series: " file ":" NR
      if (!is_owner && line ~ /(^|[^A-Za-z0-9_])ask(\.|->)(shown|dropped|pasted)[ \t]*(=[^=]|\+\+|--|\+=|-=)/)
        print "D: a question'"'"'s fields are written outside " owner ": " file ":" NR
    }

    # rule_e_line - remember whether the file assigns a display key taker and
    # the drop entry.
    function rule_e_line(line) {
      if (line ~ /e_u_key_take[ \t]*=[ \t]*e_[a-z]_key_take/) is_display = 1
      if (line ~ /e_u_type_ahead_drop[ \t]*=[^=]/) has_drop = 1
    }

    {
      line = blank_c($0, 1)
      own = note_header(line)
      if (is_c) {
        if (!is_owner) {
          calls_of(line, want_a, n_a, own, 1)
          rule_b_line(line, own)
        }
        rule_c_line(line)
        rule_e_line(line)
      }
      if (file != owner_header) rule_d_line(line)
    }

    END {
      if (is_display) {
        print "DISP\t" file
        if (!has_drop)
          print "E: " file " is a display (it assigns e_u_key_take) and does not assign e_u_type_ahead_drop"
      }
      for (g in fns) {
        if (!(g in holds)) continue
        for (i = 1; i <= ncalls[g]; i++) {
          split(calls[g, i], parts, "\t")
          print "B: " file ":" parts[1] " " g " holds a question and reads a key with " parts[2] " instead of wpe_paint_ask_key"
        }
      }
    }' "$1"
}

# aggregate FACTSDIR REPLACED_BASE REPLACED_FACTS ALLOWTSV OUT - run the rules
# that look at the whole tree over the facts of every file in FACTSDIR, except
# that the facts of REPLACED_BASE (when given) are taken from REPLACED_FACTS.
# Lines of rule a, b, c, d, e, f are written to OUT in that order.
#   a. a reader call in a function the allow file does not classify.
#   e. fewer than three displays.
#   f. an allow line whose function calls no reader.
aggregate() {
  agg_dir=$1; agg_base=$2; agg_facts=$3; agg_allow=$4; agg_out=$5
  : > "$agg_out"
  [ -f "$agg_dir/$OWNER" ] || echo "blind: $OWNER not found in the scanned tree" >> "$agg_out"
  {
    for agg_f in "$agg_dir"/*; do
      [ -f "$agg_f" ] || continue
      [ "${agg_f##*/}" = "$agg_base" ] && continue
      cat "$agg_f"
    done
    [ -n "$agg_facts" ] && cat "$agg_facts"
  } | awk -F'\t' -v allowed="$agg_allow" '
    function add(rule, text) { out[rule, ++count[rule]] = text }
    BEGIN {
      while ((getline line < allowed) > 0) {
        split(line, a, "\t"); known[a[1]] = 1
        ++nallow; allow_fn[nallow] = a[1]; allow_kind[nallow] = a[2]
      }
    }
    $1 == "R" {
      seen++; have[$4] = 1
      if (!($4 in known))
        add("A", "A: " $2 ":" $3 " " $4 " reads keys with " $5 " and the allow file does not say which kind it is")
      next
    }
    $1 == "DISP" { displays++; next }
    { add(substr($0, 1, 1), $0) }
    END {
      if (seen == 0) add("A", "A: no reader call found outside the owner (the scan is blind)")
      if (displays < 3) add("E", "E: fewer than three displays found (the scan is blind)")
      for (k = 1; k <= nallow; k++)
        if (!(allow_fn[k] in have))
          add("F", "F: stale allow line, delete it: " allow_kind[k] " " allow_fn[k])
      nrules = split("A B C D E F", order, " ")
      for (r = 1; r <= nrules; r++)
        for (k = 1; k <= count[order[r]]; k++) print out[order[r], k]
    }' >> "$agg_out"
}

# scan_tree DIR FACTSDIR - write the facts of every top-level source of DIR
# into FACTSDIR, one file per source, named like the source.
scan_tree() {
  mkdir -p "$2"
  for st_f in "$1"/*.c "$1"/*.h; do
    [ -f "$st_f" ] || continue
    st_base=${st_f##*/}
    scan_file "$st_f" "$st_base" "$B_LIST" > "$2/$st_base"
  done
}

# plant_text FILE TEXT - copy FILE to the work folder with TEXT appended, the
# changed file of a canary.
plant_text() {
  mkdir -p "$WORK/plant"
  cp "$SRCDIR/$1" "$WORK/plant/$1"
  printf '%b\n' "$2" >> "$WORK/plant/$1"
}

# plant_cut FILE PATTERN - copy FILE to the work folder without the lines that
# hold PATTERN.
plant_cut() {
  mkdir -p "$WORK/plant"
  grep -v -E "$2" "$SRCDIR/$1" > "$WORK/plant/$1"
}

# scan_changed FILE - the rules on the real tree with the changed copy of
# FILE (in the work folder) in the place of the original.
scan_changed() {
  scan_file "$WORK/plant/$1" "$1" "$B_LIST" > "$WORK/plant.facts"
  aggregate "$WORK/facts" "$1" "$WORK/plant.facts" "$WORK/allow.tsv" "$WORK/canary.out"
}

# canary RULE FILE TEXT - the rule must name the offender planted in a copy.
canary() {
  rule=$1; file=$2; text=$3
  if [ ! -f "$SRCDIR/$file" ]; then
    problem "canary $rule: $file not found under $SRCDIR (the scan is blind)"
    return
  fi
  plant_text "$file" "$text"
  scan_changed "$file"
  grep -q "^$rule:" "$WORK/canary.out" ||
    problem "canary $rule: a planted offender in $file was not named (the rule is blind)"
}

# canary_cut RULE FILE PATTERN - the rule must name the absence of the
# lines of a copy of FILE that hold PATTERN.
canary_cut() {
  rule=$1; file=$2; pattern=$3
  if [ ! -f "$SRCDIR/$file" ]; then
    problem "canary $rule: $file not found under $SRCDIR (the scan is blind)"
    return
  fi
  plant_cut "$file" "$pattern"
  scan_changed "$file"
  grep -q "^$rule:" "$WORK/canary.out" ||
    problem "canary $rule: cutting $pattern from $file was not named (the rule is blind)"
}

# The allow file: kind|function|reason, parsed into "function TAB kind".
: > "$WORK/allow.tsv"
if [ -f "$ALLOW" ]; then
  awk -F'|' -v out="$WORK/allow.tsv" '
    /^#/ || /^[ \t]*$/ { next }
    ($1 == "keeps" || $1 == "wraps" || $1 == "pending") && NF >= 3 && $2 != "" && $3 != "" \
      { print $2 "\t" $1 > out; next }
    { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1 }
    END { exit bad }' "$ALLOW" ||
    problem "the allow file $ALLOW has malformed lines"
else
  problem "no allow file at $ALLOW"
fi

# Rule b's list: the readers, the taker of the owner, and every function the
# allow file classifies, whatever its kind.
B_LIST="$READERS wpe_paint_take_key $(awk -F'\t' '{ print $1 }' "$WORK/allow.tsv" | tr '\n' ' ')"

if [ ! -f "$SRCDIR/$OWNER" ]; then
  problem "$OWNER not found under $SRCDIR (the scan is blind)"
else
  scan_tree "$SRCDIR" "$WORK/facts"
  aggregate "$WORK/facts" "" "" "$WORK/allow.tsv" "$WORK/real.out"
  if [ "$SELFTEST" = 0 ]; then
    cat "$WORK/real.out" >> "$WORK/problems"
  fi
  canary A we_ai_ui.c 'static int planted_reader(void)\n{\n return wpe_paint_modal_getch();\n}'
  canary A we_ai_ui.c 'static int planted_box_key(void)\n{\n return wpe_paint_box_key(0);\n}'
  canary A we_opt.c 'static int planted_dialog_key(void)\n{\n return e_opt_getch();\n}'
  canary B we_ai_ui.c 'static int planted_yes_no(WpeAsk *ask)\n{\n int c = ask ? e_prefix_read_letter(0) : wpe_paint_take_key(0);\n return c;\n}'
  canary B we_ai_ui.c 'static int planted_take(void)\n{\n WpeAsk ask = WPE_ASK("planted");\n (void)ask;\n return wpe_paint_take_key(0);\n}'
  canary C we_ai_ui.c 'static void planted_flush(void)\n{\n tcflush(0, TCIFLUSH);\n}'
  canary C we_ai_ui.c 'static void planted_flushinp(void)\n{\n flushinp();\n}'
  canary C we_ai_ui.c 'static void planted_drop(void)\n{\n int p;\n (*e_u_type_ahead_drop)(&p);\n}'
  canary C we_term.c 'static void planted_call(void)\n{\n int p;\n e_t_type_ahead_drop(&p);\n}'
  canary D we_ai_ui.c 'static void planted_ask(void)\n{\n WpeAsk ask;\n (void)ask;\n}'
  canary D we_ai_ui.c 'static void planted_field(WpeAsk *ask)\n{\n ask->shown = 1;\n}'
  canary D we_ai_ui.c 'static void planted_series(void)\n{\n WpeAsk keeps = WPE_ASK_SERIES("planted");\n (void)keeps;\n}'
  canary_cut E we_xterm.c 'e_u_type_ahead_drop[[:space:]]*='
  canary_cut E we_wayland.c 'e_u_type_ahead_drop[[:space:]]*='
  # A line of the allow file that matches nothing must be named as stale.
  cp "$WORK/allow.tsv" "$WORK/allow.stale"
  printf 'no_such_function_anywhere\tkeeps\n' >> "$WORK/allow.stale"
  aggregate "$WORK/facts" "" "" "$WORK/allow.stale" "$WORK/stale.out"
  grep -q '^F: stale allow line' "$WORK/stale.out" ||
    problem "canary F: a stale allow line was not named"
  # A scanner without the owner under it is blind, and says so.
  mkdir -p "$WORK/blind"
  aggregate "$WORK/blind" "" "" "$WORK/allow.tsv" "$WORK/blind.out"
  grep -q '^blind:' "$WORK/blind.out" ||
    problem "canary blind: a tree without $OWNER was not called blind"
  # A clean copy is quiet.
  if [ "$SELFTEST" = 1 ] && [ -s "$WORK/real.out" ]; then
    problem "the clean copy is not quiet:"
    cat "$WORK/real.out" >> "$WORK/problems"
  fi
fi

if [ -s "$WORK/problems" ]; then
  echo "check-question-typeahead: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
echo "check-question-typeahead: ok (6 rules, 16 canaries)"
exit 0
