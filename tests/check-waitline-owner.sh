#!/bin/sh
# check-waitline-owner.sh -- the waiting lines at the foot of Messages have one
# owner.
#
# A build, a starting debugger, a compile for debug and a scala-cli setup each
# show one line at the foot of Messages.  The owner (we_waitline.c) lays, turns
# and takes out those rows; it is also the only code that empties Messages, and
# the output writers take the foot out before they append and put it back after
# (wpe_wait_lift, wpe_wait_lay), so output never lands below a waiting line.
# Code that deletes "the last row" or empties Messages by itself is how two
# waiters took each other's line, and how a build's start took the line of a
# debugger that was still starting.
#
#   check-waitline-owner.sh [--allow FILE] [--srcdir DIR] [--selftest]
#
# Rules, each with a canary: the script plants an offender in a copy of the
# sources and fails when the rule does not name it, so a scanner gone blind
# fails instead of passing.  Scope: the top-level sources, never tests/;
# comments are blanked.
#
#  W1 Messages handles.  A function "touches Messages" when it names
#     e_p_m_buffer, e_build_messages(, ai_messages_window(,
#     ai_messages_open_buffer( or WPE_PANE_MESSAGES, or the allow file lists
#     it.  Default deny: such a function that calls a row mutator (the calls
#     of MUTATORS, or a write to ->bf[..].s or .len) is in the allow file with
#     a kind and a count that may only fall.  we_waitline.c is the owner and
#     is not scanned for this rule.
#  W2 One clear.  e_buffer_clear( in a function that touches Messages, outside
#     we_waitline.c, is an offence: Messages is emptied by
#     wpe_wait_messages_clear.
#  W3 Primitives bracket.  Each "primitive" allow line's function calls
#     wpe_wait_lift( and wpe_wait_lay(.
#  W4 The handle.  A WpeWait is made with WPE_WAIT( (a struct member is made
#     by the struct's initializer), and nothing outside we_waitline.c writes
#     or reads a field of a handle (.live, .laid, .text of a name holding
#     "wait").
#  W5 Gone for good.  The functions and the flag the waiters had before the
#     owner (GONE below) are named by no source.
#  W6 A waiter has a claim.  Every WPE_WAIT("x") names the "what" of a row of
#     the claim table in we_cancel.c.
#  W7 An allow line is alive.  Its function exists, and (but for a primitive)
#     still calls a row mutator, as many times as the line says.
#
# The allow file, tests/waitline-owner.allow, has lines
#   kind|function|count|reason
# where kind is primitive (an output writer: it brackets its append), inner (a
# per-character step called only inside a primitive's bracket), block (the
# assistant's checklist rows, found by their wording), notice (the one row
# written into an empty Messages, which has no foot), listing (a window that
# is not Messages but names a Messages handle) or user (the user's own edit).
#
# Limit, said plainly: a mutator reached through a pointer or a BUFFER *
# parameter in a function that names no Messages handle is not seen; the
# primitives are listed by name, and the rows of tests/test_waitline_owner.py
# are the net for that.
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
    *) echo "check-waitline-owner: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/waitline-owner.allow

OWNER=we_waitline.c
CLAIMS=we_cancel.c
HANDLES='e_p_m_buffer e_build_messages ai_messages_window ai_messages_open_buffer WPE_PANE_MESSAGES'
MUTATORS='e_buffer_delete_lines e_buffer_splice e_buffer_detach_lines e_buffer_set_line e_buffer_set_line_ended e_buffer_replace_rows e_buffer_replace_all e_buffer_clear e_ins_nchar e_eol_append_rows e_eol_append_text e_buffer_append_rows e_buffer_append_text e_buffer_append_lines e_buffer_insert_blank'
GONE='e_build_drop_last_row e_build_wait_line_put e_build_wait_line_drop e_d_start_wait_line_put e_d_start_wait_line_drop e_d_start_wait_line_turn wait_row'

. "$(dirname "$0")/workdir.sh"
work_dir waitline-owner $BAD
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

# The one blanker of comments and literals (tests/blank-c.awk).
BLANK_C=$(cat "$HERE/blank-c.awk")

# scan_file FILE BASE - write to stdout the facts of one source file, in one
# pass.  BASE is the file's name (a canary scans a changed copy that keeps the
# name of the original).  Records, tab separated:
#   FN TAB file TAB function TAB touch TAB mut TAB clears TAB lifts TAB lays
#   W TAB file TAB line TAB what        a WPE_WAIT("what")
#   CLAIM TAB what                      a row of the claim table
#   X: text                             an offence of rule W4 or W5
# The function is the last one whose header, a line from column 0 holding a
# "(" and not ending in ";", was seen.  Comments are blanked, string literals
# kept, one output line per input line.
scan_file() {
  awk -v file="$2" -v owner="$OWNER" -v claims="$CLAIMS" \
      -v handles="$HANDLES" -v mutators="$MUTATORS" -v gone="$GONE" \
      "$BLANK_C"'
    BEGIN {
      FS = " "
      n_h = split(handles, want_h, " ")
      n_m = split(mutators, want_m, " ")
      n_g = split(gone, want_g, " ")
      is_c = (substr(file, length(file)) == "c")
      is_owner = (file == owner)
      fn = ""
      in_struct = 0
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

    # count_calls - how many times the line calls NAME.
    function count_calls(line, name,   tmp, re) {
      if (!index(line, name)) return 0
      tmp = line
      re = "(^|[^A-Za-z0-9_])" name "[ \t]*[(]"
      return gsub(re, "&", tmp)
    }

    # has_word - 1 when the line holds NAME as a whole word.
    function has_word(line, name,   re) {
      if (!index(line, name)) return 0
      re = "(^|[^A-Za-z0-9_])" name "([^A-Za-z0-9_]|$)"
      return (line ~ re)
    }

    # flush_fn - write the facts of the function that just ended.
    function flush_fn() {
      if (fn == "") return
      printf "FN\t%s\t%s\t%d\t%d\t%d\t%d\t%d\n", file, fn, touch, mut, clr, lift, lay
    }

    # begin_fn - a header line: the previous function ends, a new one starts.
    function begin_fn(name) {
      flush_fn()
      fn = name; touch = 0; mut = 0; clr = 0; lift = 0; lay = 0
    }

    # note_function_facts - what the line says about the current function.
    function note_function_facts(line,   j) {
      if (fn == "") return
      for (j = 1; j <= n_h; j++) if (has_word(line, want_h[j])) { touch = 1; break }
      for (j = 1; j <= n_m; j++) mut += count_calls(line, want_m[j])
      if (line ~ /->bf\[[^]]*\]\.(s|len)[ \t]*=[^=]/ || line ~ /->bf\[[^]]*\]\.s\[[^]]*\][ \t]*=[^=]/) mut++
      clr += count_calls(line, "e_buffer_clear")
      lift += count_calls(line, "wpe_wait_lift")
      lay += count_calls(line, "wpe_wait_lay")
    }

    # note_waits - each WPE_WAIT("what") of the line, for rule W6.
    function note_waits(line,   rest, at, tail, what) {
      rest = line
      while (match(rest, /WPE_WAIT[ \t]*\("[^"]*"\)/)) {
        tail = substr(rest, RSTART, RLENGTH)
        sub(/^[^"]*"/, "", tail); sub(/".*$/, "", tail)
        printf "W\t%s\t%d\t%s\n", file, NR, tail
        rest = substr(rest, RSTART + RLENGTH)
      }
    }

    # rule_w4_line - a handle is made with WPE_WAIT( and never written or read
    # by hand outside the owner.
    function rule_w4_line(line) {
      if (line ~ /^[ \t]*#/) return
      if (line ~ /(^|[^A-Za-z0-9_])WpeWait[ \t]+[A-Za-z_]/) {
        if (line ~ /=/) {
          if (line !~ /=[ \t]*WPE_WAIT[ \t]*\(/)
            print "W4: a WpeWait is not made with WPE_WAIT(: " file ":" NR
        } else if (!in_struct) {
          print "W4: a WpeWait is declared without WPE_WAIT(: " file ":" NR
        }
      }
      if (line ~ /[A-Za-z0-9_]*[wW][aA][iI][tT][A-Za-z0-9_]*(\.|->)(live|laid|text)([^A-Za-z0-9_]|$)/)
        print "W4: a field of a waiting-line handle is used outside " owner ": " file ":" NR
    }

    # rule_w5_line - what the waiters had before the owner is gone for good.
    function rule_w5_line(line,   j) {
      for (j = 1; j <= n_g; j++)
        if (has_word(line, want_g[j]))
          print "W5: " want_g[j] " is back: " file ":" NR
    }

    {
      line = blank_c($0, 1)
      if (line ~ /^[A-Za-z_]/ && line ~ /\(/ && line !~ /;[ \t]*$/ &&
          line !~ /^(typedef|struct|union|enum|extern)[ \t]/)
        begin_fn(header_name(line))
      if (line ~ /^(typedef[ \t]+)?(static[ \t]+)?struct([ \t]+[A-Za-z_][A-Za-z0-9_]*)?[ \t]*(\{.*)?$/) in_struct = 1
      if (line ~ /^\}/) in_struct = 0
      if (is_c) {
        note_function_facts(line)
        note_waits(line)
        if (!is_owner) rule_w4_line(line)
        rule_w5_line(line)
        if (file == claims && match(line, /^[ \t]*\{[ \t]*"[^"]+"[ \t]*,/)) {
          tail = substr(line, RSTART, RLENGTH)
          sub(/^[^"]*"/, "", tail); sub(/".*$/, "", tail)
          print "CLAIM\t" tail
        }
      }
    }

    END { flush_fn() }' "$1"
}

# aggregate FACTSDIR REPLACED_BASE REPLACED_FACTS ALLOWTSV OUT - run the rules
# that look at the whole tree over the facts of every file in FACTSDIR, except
# that the facts of REPLACED_BASE (when given) are taken from REPLACED_FACTS.
# Lines of rule W1, W2, W3, W6, W7 are written to OUT, after the offences the
# scan itself named (W4, W5).
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
  } | awk -F'\t' -v allowed="$agg_allow" -v owner="$OWNER" '
    function add(rule, text) { out[rule, ++count[rule]] = text }
    BEGIN {
      while ((getline line < allowed) > 0) {
        split(line, a, "\t"); ++nallow
        allow_fn[nallow] = a[1]; allow_kind[nallow] = a[2]; allow_count[nallow] = a[3]
        listed[a[1]] = nallow
      }
    }
    $1 == "FN" {
      nfn++
      f = $2; g = $3
      seen[g] = 1; touch_of[g] = $4; mut_of[g] = $5; clr_of[g] = $6
      lift_of[g] = $7; lay_of[g] = $8; file_of[g] = f
      order[nfn] = g
      next
    }
    $1 == "W" { nwait++; wait_file[nwait] = $2; wait_line[nwait] = $3; wait_what[nwait] = $4; next }
    $1 == "CLAIM" { claim[$2] = 1; nclaim++; next }
    { add(substr($0, 1, 2), $0) }
    END {
      if (nfn == 0) add("W1", "W1: no function found in the scanned tree (the scan is blind)")
      if (nclaim == 0) add("W6", "W6: no claim found in the table (the scan is blind)")
      for (i = 1; i <= nfn; i++) {
        g = order[i]
        if (file_of[g] == owner) continue
        scope = (touch_of[g] == 1 || (g in listed))
        if (!scope) continue
        if (clr_of[g] > 0)
          add("W2", "W2: " file_of[g] ":" g " empties Messages itself; use wpe_wait_messages_clear")
        if (mut_of[g] > 0 && !(g in listed))
          add("W1", "W1: " file_of[g] ":" g " touches Messages and changes its rows " mut_of[g] " time(s); classify it in the allow file")
        if ((g in listed) && mut_of[g] > allow_count[listed[g]] + 0)
          add("W1", "W1: " file_of[g] ":" g " changes rows " mut_of[g] " time(s), the allow file says " allow_count[listed[g]])
      }
      for (k = 1; k <= nallow; k++) {
        g = allow_fn[k]
        if (!(g in seen)) { add("W7", "W7: stale allow line, delete it: " allow_kind[k] " " g); continue }
        if (allow_kind[k] == "primitive") {
          if (lift_of[g] == 0 || lay_of[g] == 0)
            add("W3", "W3: " file_of[g] ":" g " is an output primitive and does not call wpe_wait_lift and wpe_wait_lay")
          continue
        }
        if (mut_of[g] == 0)
          add("W7", "W7: stale allow line, " g " no longer changes rows: " allow_kind[k])
        else if (mut_of[g] < allow_count[k] + 0)
          add("W7", "W7: lower the count of " g " to " mut_of[g] " (it says " allow_count[k] ")")
      }
      for (k = 1; k <= nwait; k++)
        if (!(wait_what[k] in claim))
          add("W6", "W6: " wait_file[k] ":" wait_line[k] " WPE_WAIT(\"" wait_what[k] "\") names no row of the claim table")
      nrules = split("W1 W2 W3 W4 W5 W6 W7", ord, " ")
      for (r = 1; r <= nrules; r++)
        for (k = 1; k <= count[ord[r]]; k++) print out[ord[r], k]
    }' >> "$agg_out"
}

# scan_tree DIR FACTSDIR - write the facts of every top-level source of DIR
# into FACTSDIR, one file per source, named like the source.
scan_tree() {
  mkdir -p "$2"
  for st_f in "$1"/*.c "$1"/*.h; do
    [ -f "$st_f" ] || continue
    st_base=${st_f##*/}
    scan_file "$st_f" "$st_base" > "$2/$st_base"
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
  scan_file "$WORK/plant/$1" "$1" > "$WORK/plant.facts"
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
    grep -q "^$rule:" "$WORK/plant.facts" ||
    problem "canary $rule: a planted offender in $file was not named (the rule is blind)"
}

# canary_cut RULE FILE PATTERN - the rule must name the absence of the lines of
# a copy of FILE that hold PATTERN.
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

# The allow file: kind|function|count|reason, parsed into "function TAB kind
# TAB count".
: > "$WORK/allow.tsv"
if [ -f "$ALLOW" ]; then
  awk -F'|' -v out="$WORK/allow.tsv" '
    /^#/ || /^[ \t]*$/ { next }
    ($1 == "primitive" || $1 == "inner" || $1 == "block" || $1 == "notice" || $1 == "listing" || $1 == "user") \
      && NF >= 4 && $2 != "" && $3 ~ /^[0-9]+$/ && $4 != "" \
      { print $2 "\t" $1 "\t" $3 > out; next }
    { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1 }
    END { exit bad }' "$ALLOW" ||
    problem "the allow file $ALLOW has malformed lines"
else
  problem "no allow file at $ALLOW"
fi

if [ ! -f "$SRCDIR/$OWNER" ]; then
  problem "$OWNER not found under $SRCDIR (the scan is blind)"
else
  scan_tree "$SRCDIR" "$WORK/facts"
  aggregate "$WORK/facts" "" "" "$WORK/allow.tsv" "$WORK/real.out"
  if [ "$SELFTEST" = 0 ]; then
    cat "$WORK/real.out" >> "$WORK/problems"
  fi
  canary W1 we_prog.c 'static void planted_drop(void)\n{\n e_buffer_delete_lines(e_p_m_buffer, e_p_m_buffer->mxlines - 1, 1, E_BUFFER_STEP_NEW);\n}'
  canary W1 we_debug.c 'static void planted_set(BUFFER *b)\n{\n if (b == e_p_m_buffer)\n  e_buffer_set_line(b, 0, "x", 1);\n}'
  canary W1 we_ai_ui.c 'static void planted_poke(void)\n{\n e_p_m_buffer->bf[0].len = 0;\n}'
  canary W2 we_prog.c 'static void planted_clear(void)\n{\n e_buffer_clear(e_p_m_buffer);\n}'
  canary_cut W3 we_prog.c 'wpe_wait_lay[(]b[)];'
  canary W4 we_prog.c 'static WpeWait planted_wait;'
  canary W4 we_prog.c 'static WpeWait planted_wait = { "the build", 0, 0, "" };'
  canary W4 we_prog.c 'static void planted_field(void)\n{\n g_build_wait.laid = 1;\n}'
  canary W5 we_debug.c 'static int wait_row;'
  canary W5 we_prog.c 'static void e_build_wait_line_drop(void)\n{\n}'
  canary W6 we_prog.c 'static WpeWait planted_nothing = WPE_WAIT("the nothing");'
  # A line of the allow file that matches nothing must be named as stale.
  cp "$WORK/allow.tsv" "$WORK/allow.stale"
  printf 'no_such_function_anywhere\tblock\t1\n' >> "$WORK/allow.stale"
  aggregate "$WORK/facts" "" "" "$WORK/allow.stale" "$WORK/stale.out"
  grep -q '^W7: stale allow line' "$WORK/stale.out" ||
    problem "canary W7: a stale allow line was not named"
  # An allow count above what the function does is named, so the count falls.
  cp "$WORK/allow.tsv" "$WORK/allow.high"
  printf 'e_view_add_row\tinner\t9\n' >> "$WORK/allow.high"
  aggregate "$WORK/facts" "" "" "$WORK/allow.high" "$WORK/high.out"
  grep -q '^W7:' "$WORK/high.out" ||
    problem "canary W7: a count above the function's own was not named"
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
  echo "check-waitline-owner: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
echo "check-waitline-owner: ok (7 rules, 14 canaries)"
exit 0
