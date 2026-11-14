#!/bin/sh
# check-chain-outcome.sh -- how a debugger request ended has one owner.
#
# The request engine (we_debreq.[ch]) decides how each step of a chain ended
# (answered, refused before it went out, given up on), hands the record to
# the step's owner, writes the words that say it, and tells the chain's
# owner once, with a record.  A step table, a writer or a chain starter that
# goes around it is how a start ended with nothing said.
#
#   check-chain-outcome.sh [--allow FILE] [--srcdir DIR]
#
# Seven rules, each with a canary: the script plants an offender in a copy of
# the sources and fails when the rule does not name it, so a scanner gone
# blind fails instead of passing.  The rules are numbered as in the design.
#
#  1. The sentences of a request's end live in the owner.  Outside
#     we_debreq.h no string literal of a top-level source holds "did not
#     answer", "was not given", "with its ... prompt" or "Debug > Quit ends",
#     except the lines of the allow file, each a sentence of another owner
#     (an engine's own deadline, the input-stall line, the run's refusal)
#     with its reason.  The count of a line may only fall; a line that
#     matches nothing fails as stale.
#  3. Every debugger the request engine runs names its other prompts.  The
#     initializer of each e_d_ga_<x>_backend holds e_d_<x>_other_prompts,
#     e_d_no_other_prompts or e_d_unmeasured_other_prompts, and the two shared
#     tables are used only by the backends the allow file names.
#  4. The keyword rule stays gone.  STARTPLAN_PDB_COMMANDS and the literal
#     "(a commands block)" appear in no top-level source.
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
ALLOW=
HERE=$(cd "$(dirname "$0")" && pwd)

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --allow) shift; ALLOW=$1 ;;
    *) echo "check-chain-outcome: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/chain-outcome.allow

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

# Rule 1.  PHRASES are matched inside a string literal; the allow file holds
# R1|file|phrase|count|reason lines, parsed into $WORK/allow.r1.
PHRASES='did not answer|was not given|Debug > Quit ends'
rule1() {
  src=$1; out=$2; allowed=$3
  : > "$WORK/r1.found"
  for f in "$src"/*.c "$src"/*.h; do
    [ -f "$f" ] || continue
    base=${f##*/}
    [ "$base" = we_debreq.h ] && continue
    strip_comments "$f" | awk -v file="$base" -v phrases="$PHRASES" '
      BEGIN { n = split(phrases, ph, "|") }
      {
        s = $0
        while (match(s, /"([^"\\]|\\.)*"/)) {
          lit = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
          for (k = 1; k <= n; k++) if (index(lit, ph[k]) > 0) cnt[ph[k]]++
          if (lit ~ /with its [^"]*prompt/) cnt["with its prompt"]++
        }
      }
      END { for (p in cnt) print file "\t" p "\t" cnt[p] }' >> "$WORK/r1.found"
  done
  awk -F'\t' -v allowed="$allowed" '
    BEGIN { while ((getline line < allowed) > 0) { split(line, a, "\t"); al[a[1] "\t" a[2]] = a[3] } }
    { key = $1 "\t" $2; got[key] = $3
      if (!(key in al)) print "R1: a sentence of a request'"'"'s end outside we_debreq.h: " $1 " holds " $2 " (" $3 "x)"
      else if ($3 > al[key]) print "R1: " $1 " holds " $2 " " $3 "x, the allow file says " al[key]
      else if ($3 < al[key]) print "R1: " $1 " holds " $2 " " $3 "x, lower the allow line from " al[key] }
    END { for (k in al) if (!(k in got)) { gsub(/\t/, " ", k); print "R1: stale allow line, delete it: " k } }' \
    "$WORK/r1.found" >> "$out"
}

# Rule 3.  The allow file holds R3|token|backends|reason lines, parsed into
# $WORK/allow.r3 as "token<TAB>backends".
rule3() {
  src=$1; out=$2; allowed=$3
  strip_comments "$src/$DEBUG" | awk -F'\t' -v allowed="$allowed" '
    BEGIN { while ((getline line < allowed) > 0) { split(line, a, "\t"); who[a[1]] = " " a[2] " " } }
    function finish(   tok, shared, k, name) {
      if (name_x == "") return
      tok = "e_d_" name_x "_other_prompts"
      found = 0
      if (index(body, tok) > 0) found = 1
      for (shared in who) if (index(body, shared) > 0) {
        found = 1
        if (index(who[shared], " " name_x " ") == 0)
          print "R3: backend " name_x " uses " shared ", which the allow file does not grant it"
      }
      if (!found) print "R3: backend " name_x " names no other-prompt table (" tok ", e_d_no_other_prompts or e_d_unmeasured_other_prompts)"
      name_x = ""; body = ""
    }
    /^static const E_D_GA_BACKEND e_d_ga_[a-z0-9]+_backend/ {
      finish()
      line = $0; sub(/^static const E_D_GA_BACKEND e_d_ga_/, "", line); sub(/_backend.*/, "", line)
      name_x = line; body = ""; seen++; next
    }
    name_x != "" { body = body " " $0; if ($0 ~ /^};/) finish() }
    END { finish(); if (seen == 0) print "R3: no backend initializer found in '"$DEBUG"' (the scan is blind)" }' \
    >> "$out"
  for tok in $(awk -F'\t' '{print $1}' "$allowed"); do
    grep -q "$tok" "$src/$DEBUG" || echo "R3: $tok is in the allow file and no longer exists in $DEBUG" >> "$out"
  done
}

# Rule 4.
rule4() {
  src=$1; out=$2
  for f in "$src"/*.c "$src"/*.h; do
    [ -f "$f" ] || continue
    base=${f##*/}
    strip_comments "$f" | awk -v file="$base" '
      /STARTPLAN_PDB_COMMANDS/ { print "R4: the keyword rule is back: STARTPLAN_PDB_COMMANDS in " file ":" NR }
      /\(a commands block\)/ { print "R4: the keyword rule is back: \"(a commands block)\" in " file ":" NR }' >> "$out"
  done
}

# scan_tree DIR OUT R1ALLOW R3ALLOW - run the seven rules on the sources under DIR.
scan_tree() {
  : > "$2"
  rule1 "$1" "$2" "$3"
  rule2 "$1" "$2"
  rule3 "$1" "$2" "$4"
  rule4 "$1" "$2"
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
  scan_tree "$WORK/copy" "$WORK/canary.out" "$WORK/allow.r1" "$WORK/allow.r3"
  grep -q "^$rule:" "$WORK/canary.out" ||
    problem "canary $rule: a planted offender in $file was not named (the rule is blind)"
}

# The allow file: R1|file|phrase|count|reason, R3|token|backends|reason.
: > "$WORK/allow.r1"
: > "$WORK/allow.r3"
if [ -f "$ALLOW" ]; then
  awk -F'|' -v r1="$WORK/allow.r1" -v r3="$WORK/allow.r3" '
    /^#/ || /^[ \t]*$/ { next }
    $1 == "R1" && NF >= 5 && $4 ~ /^[0-9]+$/ && $5 != "" { print $2 "\t" $3 "\t" $4 > r1; next }
    $1 == "R3" && NF >= 4 && $4 != "" { print $2 "\t" $3 > r3; next }
    { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1 }
    END { exit bad }' "$ALLOW" ||
    problem "the allow file $ALLOW has malformed lines"
else
  problem "no allow file at $ALLOW"
fi

if [ ! -f "$SRCDIR/$ENGINE" ] || [ ! -f "$SRCDIR/$DEBUG" ]; then
  problem "$ENGINE or $DEBUG not found under $SRCDIR (the scan is blind)"
else
  scan_tree "$SRCDIR" "$WORK/real.out" "$WORK/allow.r1" "$WORK/allow.r3"
  cat "$WORK/real.out" >> "$WORK/problems"
  canary R1 "$DEBUG" 'static const char *planted_say = "gdb did not answer `x`";'
  canary R1 "$DEBUG" 'static const char *planted_not_given = "gdb was not given `x`";'
  canary R3 "$DEBUG" 'static const E_D_GA_BACKEND e_d_ga_planted_backend =\n{\n NULL\n};'
  canary R3 "$DEBUG" 'static const E_D_GA_BACKEND e_d_ga_plantedshared_backend =\n{\n e_d_no_other_prompts\n};'
  canary R4 we_startplan.c '#define STARTPLAN_PDB_COMMANDS "commands"'
  canary R4 we_startplan.c 'static const char *planted_refusal = "a .pdbrc line (a commands block)";'
  # A line of the allow file that matches nothing must be named as stale.
  cp "$WORK/allow.r1" "$WORK/allow.r1.stale"
  printf 'we_debug.c\tdid not answer, no such sentence\t1\n' >> "$WORK/allow.r1.stale"
  scan_tree "$SRCDIR" "$WORK/stale.out" "$WORK/allow.r1.stale" "$WORK/allow.r3"
  grep -q '^R1: stale allow line' "$WORK/stale.out" ||
    problem "canary R1: a stale allow line was not named"
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
echo "check-chain-outcome: ok (7 rules, 15 canaries)"
exit 0
