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
#  a. Default deny.  A call of wpe_paint_modal_getch outside we_paint.c, or of
#     a wrapper that reads for others (wpe_box_getch, e_opt_getch,
#     e_opt_read_key), is in a function the allow file classifies.  A new
#     reader fails the build until its author says which kind it is.
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
#     with WPE_ASK( (a pointer is no declaration); a variable named ask never
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
# wraps (reads for others; its callers are classified instead) or pending (a
# question that the message engine's pass moves onto the owner; it leaves the
# file then).  A line that matches nothing is stale and fails.
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
DESK_ENTRY_FILE=we_edit.c
DESK_ENTRY=e_paint_settle_and_drop
READERS='wpe_paint_modal_getch wpe_box_getch e_opt_getch e_opt_read_key'

. "$(dirname "$0")/workdir.sh"
work_dir question-typeahead $BAD
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")

# strip_comments FILE - the file's lines with comments blanked, string
# literals kept, one output line per input line.
strip_comments() {
  awk "$BLANK_C"'
    FNR == 1 { blank_incom = 0 }
    { print blank_c($0, 1) }' "$1"
}

# calls_in FILE CALLEE... - "line TAB function TAB callee TAB rest" for each
# call of a CALLEE in the comment-stripped FILE, outside the callee's own
# definition line.  The function is the last one whose header, a line from
# column 0 holding a "(" and not ending in ";", was seen.
calls_in() {
  calls_file=$1; shift
  strip_comments "$calls_file" | awk -v list="$*" '
    BEGIN { n = split(list, want, " "); fn = "" }
    function header_name(line,   head, parts, k) {
      head = line
      sub(/\(.*/, "", head)
      sub(/[^A-Za-z0-9_]+$/, "", head)
      k = split(head, parts, /[^A-Za-z0-9_]+/)
      return parts[k]
    }
    {
      is_header = 0
      if ($0 ~ /^[A-Za-z_]/ && $0 ~ /\(/ && $0 !~ /;[ \t]*$/ &&
          $0 !~ /^(typedef|struct|union|enum|extern)[ \t]/) {
        fn = header_name($0); is_header = 1
      }
      for (j = 1; j <= n; j++) {
        pat = want[j]; plen = length(pat); text = $0
        while ((at = index(text, pat)) > 0) {
          before = at > 1 ? substr(text, at - 1, 1) : ""
          rest = substr(text, at + plen)
          if (before !~ /[A-Za-z0-9_]/ && rest ~ /^[ \t]*\(/ && !(is_header && fn == pat))
            printf "%d\t%s\t%s\t%s\n", NR, fn, pat, rest
          text = substr(text, at + plen)
        }
      }
    }'
}

# Rule a, and the facts rule f needs: "file TAB line TAB function TAB reader"
# for every reader call outside the owner, in $WORK/reads.
find_reads() {
  reads_dir=$1; reads_out=$2
  : > "$reads_out"
  for reads_f in "$reads_dir"/*.c; do
    [ -f "$reads_f" ] || continue
    reads_base=${reads_f##*/}
    [ "$reads_base" = "$OWNER" ] && continue
    calls_in "$reads_f" $READERS | awk -F'\t' -v file="$reads_base" '{ print file "\t" $1 "\t" $2 "\t" $3 }' >> "$reads_out"
  done
}

rule_a() {
  src=$1; out=$2; allowed=$3
  find_reads "$src" "$WORK/reads"
  awk -F'\t' -v allowed="$allowed" '
    BEGIN { while ((getline line < allowed) > 0) { split(line, a, "\t"); known[a[1]] = a[2] } }
    { seen++
      if (!($3 in known))
        print "A: " $1 ":" $2 " " $3 " reads keys with " $4 " and the allow file does not say which kind it is" }
    END { if (seen == 0) print "A: no reader call found outside the owner (the scan is blind)" }' \
    "$WORK/reads" >> "$out"
}

rule_f() {
  src=$1; out=$2; allowed=$3
  find_reads "$src" "$WORK/reads"
  awk -F'\t' -v allowed="$allowed" '
    { have[$3] = 1 }
    END { while ((getline line < allowed) > 0) { split(line, a, "\t")
            if (!(a[1] in have)) print "F: stale allow line, delete it: " a[2] " " a[1] } }' \
    "$WORK/reads" >> "$out"
}

# Rule b.  A function that holds a question (it has a WpeAsk, as a variable
# or a parameter, or it calls wpe_paint_ask_key) takes its key only from
# wpe_paint_ask_key.  Any call from it to another reader of keys is named: the
# four readers of rule a, wpe_paint_take_key, and every function the allow
# file classifies, whatever its kind, so a new entry of the allow file is
# covered without a list kept here.
rule_b() {
  src=$1; out=$2; allowed=$3
  awk -F'\t' '{ print $1 }' "$allowed" > "$WORK/b.readers"
  b_list="$READERS wpe_paint_take_key $(tr '\n' ' ' < "$WORK/b.readers")"
  for f in "$src"/*.c; do
    [ -f "$f" ] || continue
    base=${f##*/}
    [ "$base" = "$OWNER" ] && continue
    strip_comments "$f" | awk -v file="$base" -v list="$b_list" '
      BEGIN { n = split(list, want, " ") }
      function header_name(line,   head, parts, k) {
        head = line
        sub(/\(.*/, "", head)
        sub(/[^A-Za-z0-9_]+$/, "", head)
        k = split(head, parts, /[^A-Za-z0-9_]+/)
        return parts[k]
      }
      function note_calls(line, own,   j, pat, text, at, before, rest) {
        for (j = 1; j <= n; j++) {
          pat = want[j]; text = line
          while ((at = index(text, pat)) > 0) {
            before = at > 1 ? substr(text, at - 1, 1) : ""
            rest = substr(text, at + length(pat))
            if (before !~ /[A-Za-z0-9_]/ && rest ~ /^[ \t]*\(/ && pat != own) {
              calls[fn, ++ncalls[fn]] = NR "\t" pat
              fns[fn] = 1
            }
            text = substr(text, at + length(pat))
          }
        }
      }
      {
        own = ""
        if ($0 ~ /^[A-Za-z_]/ && $0 ~ /\(/ && $0 !~ /;[ \t]*$/ &&
            $0 !~ /^(typedef|struct|union|enum|extern)[ \t]/) {
          fn = header_name($0); own = fn
        }
        if (fn == "") next
        if ($0 ~ /(^|[^A-Za-z0-9_])(WpeAsk[ \t*]|wpe_paint_ask_key[ \t]*\()/)
          holds[fn] = 1
        note_calls($0, own)
      }
      END {
        for (g in fns) {
          if (!(g in holds)) continue
          for (i = 1; i <= ncalls[g]; i++) {
            split(calls[g, i], parts, "\t")
            print "B: " file ":" parts[1] " " g " holds a question and reads a key with " parts[2] " instead of wpe_paint_ask_key"
          }
        }
      }' >> "$out"
  done
}

# Rule c.
rule_c() {
  src=$1; out=$2
  for f in "$src"/*.c; do
    [ -f "$f" ] || continue
    base=${f##*/}
    strip_comments "$f" | awk -v file="$base" -v entry_file="$DESK_ENTRY_FILE" -v entry="$DESK_ENTRY" '
      function header_name(line,   head, parts, k) {
        head = line
        sub(/\(.*/, "", head)
        sub(/[^A-Za-z0-9_]+$/, "", head)
        k = split(head, parts, /[^A-Za-z0-9_]+/)
        return parts[k]
      }
      {
        if ($0 ~ /^[A-Za-z_]/ && $0 ~ /\(/ && $0 !~ /;[ \t]*$/ &&
            $0 !~ /^(typedef|struct|union|enum|extern)[ \t]/)
          fn = header_name($0)
        if ($0 ~ /(^|[^A-Za-z0-9_])(tcflush|flushinp)[ \t]*\(/)
          print "C: tcflush or flushinp goes around the terminal reader: " file ":" NR
        if ($0 ~ /e_u_type_ahead_drop/ && $0 !~ /^[ \t]*(extern[ \t]+)?int[ \t]*\(\*e_u_type_ahead_drop\)/ &&
            $0 !~ /e_u_type_ahead_drop[ \t]*=[^=]/ && $0 ~ /e_u_type_ahead_drop\)?[ \t]*\(/ &&
            !(file == entry_file && fn == entry))
          print "C: the display drop is called outside " entry ": " file ":" NR
        if ($0 ~ /e_[txw]_type_ahead_drop[ \t]*\(/ && $0 !~ /^(static[ \t]+)?int[ \t]+e_[txw]_type_ahead_drop[ \t]*\(/)
          print "C: a display drop function is called, not assigned to the table: " file ":" NR
      }' >> "$out"
  done
}

# Rule d.
rule_d() {
  src=$1; out=$2
  for f in "$src"/*.c "$src"/*.h; do
    [ -f "$f" ] || continue
    base=${f##*/}
    [ "$base" = we_paint.h ] && continue
    strip_comments "$f" | awk -v file="$base" -v owner="$OWNER" '
      /(^|[^A-Za-z0-9_])WpeAsk[ \t]+[A-Za-z_]/ && $0 !~ /WpeAsk[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*WPE_ASK\(/ \
        { print "D: a WpeAsk is not made with WPE_ASK: " file ":" NR }
      file != owner && /(^|[^A-Za-z0-9_])ask(\.|->)(shown|dropped|pasted)[ \t]*(=[^=]|\+\+|--|\+=|-=)/ \
        { print "D: a question'"'"'s fields are written outside " owner ": " file ":" NR }' >> "$out"
  done
}

# Rule e.
rule_e() {
  src=$1; out=$2
  : > "$WORK/displays"
  for f in "$src"/*.c; do
    [ -f "$f" ] || continue
    base=${f##*/}
    strip_comments "$f" > "$WORK/e.stripped"
    if grep -q -E 'e_u_key_take[[:space:]]*=[[:space:]]*e_[a-z]_key_take' "$WORK/e.stripped"; then
      echo "$base" >> "$WORK/displays"
      grep -q -E 'e_u_type_ahead_drop[[:space:]]*=[^=]' "$WORK/e.stripped" ||
        echo "E: $base is a display (it assigns e_u_key_take) and does not assign e_u_type_ahead_drop" >> "$out"
    fi
  done
  [ "$(wc -l < "$WORK/displays")" -ge 3 ] ||
    echo "E: fewer than three displays found (the scan is blind)" >> "$out"
}

# scan_tree DIR OUT ALLOWTSV - run the rules on the sources under DIR.
scan_tree() {
  : > "$2"
  [ -f "$1/$OWNER" ] || echo "blind: $OWNER not found under $1" >> "$2"
  rule_a "$1" "$2" "$3"
  rule_b "$1" "$2" "$3"
  rule_c "$1" "$2"
  rule_d "$1" "$2"
  rule_e "$1" "$2"
  rule_f "$1" "$2" "$3"
}

copy_tree() {
  mkdir -p "$1"
  for f in "$SRCDIR"/*.c "$SRCDIR"/*.h; do [ -f "$f" ] && cp "$f" "$1/"; done
}

# canary RULE FILE TEXT - the rule must name the offender planted in a copy.
canary() {
  rule=$1; file=$2; text=$3
  rm -rf "$WORK/copy"
  copy_tree "$WORK/copy"
  if [ ! -f "$WORK/copy/$file" ]; then
    problem "canary $rule: $file not found under $SRCDIR (the scan is blind)"
    return
  fi
  printf '%b\n' "$text" >> "$WORK/copy/$file"
  scan_tree "$WORK/copy" "$WORK/canary.out" "$WORK/allow.tsv"
  grep -q "^$rule:" "$WORK/canary.out" ||
    problem "canary $rule: a planted offender in $file was not named (the rule is blind)"
}

# canary_cut RULE FILE PATTERN - the rule must name the absence of the
# lines of a copy of FILE that hold PATTERN.
canary_cut() {
  rule=$1; file=$2; pattern=$3
  rm -rf "$WORK/copy"
  copy_tree "$WORK/copy"
  if [ ! -f "$WORK/copy/$file" ]; then
    problem "canary $rule: $file not found under $SRCDIR (the scan is blind)"
    return
  fi
  grep -v -E "$pattern" "$WORK/copy/$file" > "$WORK/copy/$file.cut"
  mv "$WORK/copy/$file.cut" "$WORK/copy/$file"
  scan_tree "$WORK/copy" "$WORK/canary.out" "$WORK/allow.tsv"
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

if [ ! -f "$SRCDIR/$OWNER" ]; then
  problem "$OWNER not found under $SRCDIR (the scan is blind)"
else
  if [ "$SELFTEST" = 0 ]; then
    scan_tree "$SRCDIR" "$WORK/real.out" "$WORK/allow.tsv"
    cat "$WORK/real.out" >> "$WORK/problems"
  fi
  canary A we_ai_ui.c 'static int planted_reader(void)\n{\n return wpe_paint_modal_getch();\n}'
  canary A we_ai_ui.c 'static int planted_wrapper(WpeBox *b)\n{\n return wpe_box_getch(b);\n}'
  canary A we_opt.c 'static int planted_dialog_key(void)\n{\n return e_opt_getch();\n}'
  canary B we_ai_ui.c 'static int planted_yes_no(WpeAsk *ask)\n{\n int c = ask ? e_prefix_read_letter(0) : wpe_paint_take_key(0);\n return c;\n}'
  canary B we_ai_ui.c 'static int planted_take(void)\n{\n WpeAsk ask = WPE_ASK("planted");\n (void)ask;\n return wpe_paint_take_key(0);\n}'
  canary C we_ai_ui.c 'static void planted_flush(void)\n{\n tcflush(0, TCIFLUSH);\n}'
  canary C we_ai_ui.c 'static void planted_flushinp(void)\n{\n flushinp();\n}'
  canary C we_ai_ui.c 'static void planted_drop(void)\n{\n int p;\n (*e_u_type_ahead_drop)(&p);\n}'
  canary C we_term.c 'static void planted_call(void)\n{\n int p;\n e_t_type_ahead_drop(&p);\n}'
  canary D we_ai_ui.c 'static void planted_ask(void)\n{\n WpeAsk ask;\n (void)ask;\n}'
  canary D we_ai_ui.c 'static void planted_field(WpeAsk *ask)\n{\n ask->shown = 1;\n}'
  canary_cut E we_xterm.c 'e_u_type_ahead_drop[[:space:]]*='
  canary_cut E we_wayland.c 'e_u_type_ahead_drop[[:space:]]*='
  # A line of the allow file that matches nothing must be named as stale.
  cp "$WORK/allow.tsv" "$WORK/allow.stale"
  printf 'no_such_function_anywhere\tkeeps\n' >> "$WORK/allow.stale"
  scan_tree "$SRCDIR" "$WORK/stale.out" "$WORK/allow.stale"
  grep -q '^F: stale allow line' "$WORK/stale.out" ||
    problem "canary F: a stale allow line was not named"
  # A scanner without the owner under it is blind, and says so.
  rm -rf "$WORK/blind"
  mkdir -p "$WORK/blind"
  scan_tree "$WORK/blind" "$WORK/blind.out" "$WORK/allow.tsv"
  grep -q '^blind:' "$WORK/blind.out" ||
    problem "canary blind: a tree without $OWNER was not called blind"
  # A clean copy is quiet.
  rm -rf "$WORK/copy"
  copy_tree "$WORK/copy"
  scan_tree "$WORK/copy" "$WORK/clean.out" "$WORK/allow.tsv"
  if [ "$SELFTEST" = 1 ] && [ -s "$WORK/clean.out" ]; then
    problem "the clean copy is not quiet:"
    cat "$WORK/clean.out" >> "$WORK/problems"
  fi
fi

if [ -s "$WORK/problems" ]; then
  echo "check-question-typeahead: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
echo "check-question-typeahead: ok (6 rules, 15 canaries)"
exit 0
