#!/bin/sh
# check-chain-outcome.sh -- how a debugger request ended has one owner.
#
# The request engine (we_debreq.[ch]) decides how each step of a chain ended
# (answered, refused before it went out, given up on), hands the record to
# the step's owner, writes the words that say it, and tells the chain's
# owner once, with a record.  A step table, a writer or a chain starter that
# goes around it is how a start ended with nothing said.
#
#   check-chain-outcome.sh [--srcdir DIR]
#
# Four rules, each with a canary: the script plants an offender in a copy of
# the sources and fails when the rule does not name it, so a scanner gone
# blind fails instead of passing.  The rules are numbered as in the design;
# the ones about the sentences, the other prompts and the keyword rule join
# with the pass that moves them.
#
#  2. A step that owns a verdict names its failed.  Every row of an E_D_STEP
#     table whose command is e_d_gb_add_cmd, e_d_gsdb_add_cmd or e_d_cl_cmd
#     has three members, the third not NULL.  The two commands that write a
#     file name too and are left out are listed below with their reason.
#  5. No chain ends unsaid.  In we_debreq.c every call of chain_end whose
#     second argument is 0 is in chain_answered (whose ends the engine said,
#     or the step's owner took) or in a function that calls
#     chain_say_not_sent.
#  6. No chain is finished around the owner.  Outside we_debreq.c every call
#     of a chain's finished function is preceded, within two lines, by
#     e_d_chain_end_unstarted: the starters that could not run a chain.
#  7. The debugger's input has one gated writer.  In we_debug.c
#     e_d_pipe_write and e_d_gdbmi_send_lines are called only by the raw
#     writer e_d_write_cmd (and e_d_gdbmi_send_lines by e_d_pipe_write's
#     caller above it), e_d_write_cmd only by e_d_send_cmd (the gate),
#     e_d_ga_send (the engine, which refuses a command itself) and
#     e_d_send_quit, and the write to the debugger's descriptor appears only
#     in e_d_pipe_write; a raw write(2) to it appears nowhere.  A table row
#     written over several lines is read as one.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-chain-outcome: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

ENGINE=we_debreq.c
DEBUG=we_debug.c

# Rule 2: the commands whose rows name a failed, and the two left out.
VERDICT_CMDS='e_d_gb_add_cmd|e_d_gsdb_add_cmd|e_d_cl_cmd'
# e_d_ga_cursor_set_cmd and e_d_gg_start_cmd write a file name under dbx and
# xdb too, but own no mark: a refusal ends the go chain not ok, the chain's
# notice says it, and e_d_gg_finished drops the resume, so the program does
# not run past a cursor or a start the debugger was not given.
LEFT_OUT='e_d_ga_cursor_set_cmd e_d_gg_start_cmd'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/chain-outcome.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

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

# calls_in FILE CALLEE... - "line TAB function TAB callee TAB rest" for each
# call of a CALLEE (an identifier, or an identifier and the start of its
# arguments, as given) in the comment-stripped FILE, outside the callee's own
# definition line.  The function is the last one whose header, a line from
# column 0 holding a "(" and not ending in ";", was seen.
calls_in() {
  file=$1; shift
  strip_comments "$file" | awk -v list="$*" '
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
        pat = want[j]; plen = length(pat); text = $0; base = 0
        while ((at = index(text, pat)) > 0) {
          before = at > 1 ? substr(text, at - 1, 1) : ""
          bare = (pat ~ /^[A-Za-z0-9_]+$/)
          rest = substr(text, at + plen)
          left_ok = (before !~ /[A-Za-z0-9_]/)
          right_ok = 1
          if (bare) right_ok = (rest ~ /^[ \t]*\(/)
          if (left_ok && right_ok && !(is_header && fn == pat))
            printf "%d\t%s\t%s\t%s\n", NR, fn, pat, rest
          text = substr(text, at + plen)
        }
      }
    }'
}

# Rule 2.
rule2() {
  src=$1; out=$2
  # A row written over several lines is joined onto its first line (the
  # lines it took are left blank, so line numbers hold).
  strip_comments "$src/$DEBUG" | awk -v cmds="$VERDICT_CMDS" '
    BEGIN { re = "\\{[ \t]*(" cmds ")[ \t]*,"; held = "" }
    held != "" {
      held = held " " $0; blanks++
      if ($0 ~ /\}/) { print held; for (k = 0; k < blanks; k++) print ""; held = "" }
      next
    }
    $0 ~ re && $0 !~ /\}/ { held = $0; blanks = 0; next }
    { print }
    END { if (held != "") print held }' > "$WORK/dbg.stripped"
  grep -n -E "\{[[:space:]]*($VERDICT_CMDS)[[:space:]]*,[[:space:]]*[A-Za-z0-9_]+[[:space:]]*\}" \
    "$WORK/dbg.stripped" | while IFS= read -r hit; do
      echo "R2: a verdict step has no failed: $DEBUG:${hit%%:*}"
    done >> "$out"
  grep -n -E "\{[[:space:]]*($VERDICT_CMDS)[[:space:]]*,[[:space:]]*[A-Za-z0-9_]+[[:space:]]*,[[:space:]]*(NULL|0)[[:space:]]*\}" \
    "$WORK/dbg.stripped" | while IFS= read -r hit; do
      echo "R2: a verdict step's failed is NULL: $DEBUG:${hit%%:*}"
    done >> "$out"
  good=$(grep -c -E "\{[[:space:]]*($VERDICT_CMDS)[[:space:]]*,[[:space:]]*[A-Za-z0-9_]+[[:space:]]*,[[:space:]]*[A-Za-z_][A-Za-z0-9_]*[[:space:]]*\}" "$WORK/dbg.stripped")
  [ "$good" -gt 0 ] || echo "R2: no row names a failed (the scan is blind or the steps are gone)" >> "$out"
  for name in $LEFT_OUT; do
    grep -q "$name" "$WORK/dbg.stripped" ||
      echo "R2: $name is listed as left out and no longer exists: delete it from the script" >> "$out"
  done
}

# Rule 5.
rule5() {
  src=$1; out=$2
  calls_in "$src/$ENGINE" chain_end chain_say_not_sent > "$WORK/r5.calls"
  awk -F'\t' '
    $3 == "chain_say_not_sent" { says[$2] = 1; next }
    $3 == "chain_end" { seen++
      if ($4 ~ /^\([^,]*,[ \t]*0[ \t]*[,)]/) zero[NR] = $1 "\t" $2 }
    END {
      if (seen == 0) print "R5: no call of chain_end in the engine (the scan is blind)"
      for (k in zero) { split(zero[k], f, "\t")
        if (f[2] != "chain_answered" && !(f[2] in says))
          print "R5: a chain ends not ok and says nothing: " f[2] " (line " f[1] ")" } }' \
    "$WORK/r5.calls" >> "$out"
}

# Rule 6.
rule6() {
  src=$1; out=$2
  for f in "$src"/*.c "$src"/*.h; do
    [ -f "$f" ] || continue
    base=${f##*/}
    case $base in we_debreq.c|we_debreq.h) continue ;; esac
    strip_comments "$f" > "$WORK/r6.stripped"
    awk -v file="$base" '
      { line[NR] = $0 }
      END {
        for (i = 1; i <= NR; i++) {
          s = line[i]
          if (s !~ /(^|[^A-Za-z0-9_])finished[ \t]*\(/) continue
          if (s ~ /^[A-Za-z_].*\(.*\)[ \t]*$/ && s !~ /;/) continue
          ok = 0
          for (k = i - 2; k < i; k++)
            if (k >= 1 && line[k] ~ /e_d_chain_end_unstarted[ \t]*\(/) ok = 1
          if (!ok) print "R6: a chain is finished without the owner'"'"'s record: " file ":" i
        }
      }' "$WORK/r6.stripped" >> "$out"
  done
}

# Rule 7.
rule7() {
  src=$1; out=$2
  calls_in "$src/$DEBUG" e_d_pipe_write e_d_gdbmi_send_lines e_d_write_cmd \
    'wpe_fd_write(rfildes[1]' 'write(rfildes[1]' > "$WORK/r7.calls"
  awk -F'\t' '
    BEGIN {
      allow["e_d_pipe_write"] = " e_d_write_cmd e_d_gdbmi_send_lines "
      allow["e_d_gdbmi_send_lines"] = " e_d_write_cmd "
      allow["e_d_write_cmd"] = " e_d_send_cmd e_d_ga_send e_d_send_quit "
      allow["wpe_fd_write(rfildes[1]"] = " e_d_pipe_write "
    }
    $3 == "write(rfildes[1]" {
      print "R7: the debugger'"'"'s input is written raw, in " $2 " (line " $1 ")"
      next
    }
    {
      seen[$3]++
      if (index(allow[$3], " " $2 " ") == 0)
        print "R7: " $3 " is called outside the gated writer, in " $2 " (line " $1 ")"
    }
    END {
      for (c in allow) if (!(c in seen))
        print "R7: no call of " c " (the scan is blind or the writer is gone)"
    }' "$WORK/r7.calls" >> "$out"
}

# scan_tree DIR OUT - run the four rules on the sources under DIR.
scan_tree() {
  : > "$2"
  rule2 "$1" "$2"
  rule5 "$1" "$2"
  rule6 "$1" "$2"
  rule7 "$1" "$2"
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
  scan_tree "$WORK/copy" "$WORK/canary.out"
  grep -q "^$rule:" "$WORK/canary.out" ||
    problem "canary $rule: a planted offender in $file was not named (the rule is blind)"
}

if [ ! -f "$SRCDIR/$ENGINE" ] || [ ! -f "$SRCDIR/$DEBUG" ]; then
  problem "$ENGINE or $DEBUG not found under $SRCDIR (the scan is blind)"
else
  scan_tree "$SRCDIR" "$WORK/real.out"
  cat "$WORK/real.out" >> "$WORK/problems"
  canary R2 "$DEBUG" 'static const E_D_STEP planted_steps[] =\n{\n { e_d_gb_add_cmd, e_d_gb_add_answered },\n { NULL, NULL }\n};'
  canary R2 "$DEBUG" 'static const E_D_STEP planted_null[] =\n{\n { e_d_cl_cmd, e_d_cl_answered, NULL },\n { NULL, NULL }\n};'
  canary R5 "$ENGINE" 'static int planted_end(E_D_CHAIN *ch)\n{\n\treturn chain_end(ch, 0, NULL, 0);\n}'
  canary R6 "$DEBUG" 'static void planted_finish(e_d_chain_done_fn finished, void *ctx)\n{\n finished(NULL, ctx);\n}'
  canary R7 "$DEBUG" 'static void planted_write(void)\n{\n e_d_write_cmd("x");\n}'
  canary R7 "$DEBUG" 'static void planted_pipe(void)\n{\n e_d_pipe_write("x", 1);\n}'
  canary R7 "$DEBUG" 'static void planted_raw(void)\n{\n wpe_fd_write(rfildes[1], "x", 1, NULL, NULL);\n}'
  canary R7 "$DEBUG" 'static void planted_write(void)\n{\n write(rfildes[1], "x", 1);\n}'
  canary R2 "$DEBUG" 'static const E_D_STEP planted_split[] =\n{\n { e_d_gsdb_add_cmd,\n   e_d_gsdb_add_answered },\n { NULL, NULL }\n};'
fi

if [ -s "$WORK/problems" ]; then
  echo "check-chain-outcome: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
echo "check-chain-outcome: ok (4 rules, 9 canaries)"
exit 0
