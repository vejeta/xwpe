#!/bin/sh
# check-debug-command.sh -- how long a command to a debugger may be has one owner.
#
# The request engine (we_debreq.h) holds the number, E_D_REQ_CMD_MAX, and
# the one test of a command against it (e_d_req_cmd_fits).  Every buffer
# that holds a command to a debugger is sized from that number, so the
# engine never refuses what a site built and no site cuts what the engine
# would take.  A site that sizes a debugger command by a number of its own is
# how one limit became five that disagreed.
#
#   check-debug-command.sh [--allow FILE] [--srcdir DIR] [--list]
#
# Five rules, each with a canary: the script plants an offender in a copy of
# the sources and fails when the rule does not name it, so a scanner gone
# blind fails instead of passing.
#
#  1. One definition.  E_D_REQ_CMD_MAX is defined once, in we_debreq.h, from
#     E_D_REQ_PATH_MAX and E_D_REQ_PATH_ESCAPE_MAX: a literal there fails.
#  2. No other cap.  A #define whose name ends in CMD_MAX or COMMAND_MAX and
#     whose value does not name E_D_REQ_CMD_MAX, in a top level source or in
#     tests/, is counted per file against the allow file, which gives each
#     one's reason (none of them is a line to a debugger).  The count may only
#     fall; a line that matches nothing fails as stale.
#  3. No comparison outside the engine.  A comparison operator next to
#     E_D_REQ_CMD_MAX outside we_debreq.c fails, with no allow line possible.
#     A compile-time check (a typedef of a negative size, whose line ends
#     "? 1 : -1]") is not a comparison the program makes, and is the one
#     thing the rule leaves alone.
#  4. The workarounds stay gone.  The identifiers of the cut-and-split
#     workarounds appear in no top level source (comments are not read).
#  5. The known command buffers derive from the owner.  The define of each
#     buffer listed below names what it is built from; a missing define is
#     fine (the buffer went), one that does not name its source fails.
#
# --list prints what rule 2 found, as allow lines.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
ALLOW=
LIST=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    *) echo "check-debug-command: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/debug-command.allow

OWNER_HEADER=we_debreq.h
OWNER_SOURCE=we_debreq.c
OWNER_MACRO=E_D_REQ_CMD_MAX

# The identifiers of the workarounds the owner made unnecessary.
GONE='startplan_program_load_in_folder|e_d_gp_short_path|E_D_GP_PLACE_ROOM|STARTPLAN_COMMAND_MAX|STARTPLAN_LINE_MAX'

# Rule 5: "file name source", the buffer's define and what it derives from.
DERIVED='we_debug.c E_D_RUN_AGAIN_MAX E_D_REQ_CMD_MAX
we_debug.c E_D_GDBMI_QUOTED_MAX E_D_REQ_CMD_MAX
we_debug.c E_D_GDBMI_LINE_MAX E_D_GDBMI_QUOTED_MAX
we_debug.c E_D_GA_LINE_MAX E_D_REQ_CMD_MAX
we_debug.c E_D_WATCH_CMD_MAX E_D_REQ_CMD_MAX
we_debug.c E_D_BRK_CMD_MAX E_D_REQ_CMD_MAX
we_debug.c E_D_GO_CMD_MAX E_D_REQ_CMD_MAX'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/debug-command.XXXXXX") || exit $BAD
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

# defines_of FILE REL - "rel|line|name|value" for each #define of the
# (comment stripped) file, a line continued by a backslash joined to its first.
defines_of() {
  awk -v rel="$2" '
    function flush() {
      if (logical ~ /^[ \t]*#[ \t]*define[ \t]+[A-Za-z_]/) {
        s = logical
        sub(/^[ \t]*#[ \t]*define[ \t]+/, "", s)
        name = s; sub(/[^A-Za-z0-9_].*$/, "", name)
        value = substr(s, length(name) + 1); sub(/^[ \t]+/, "", value)
        print rel "|" start "|" name "|" value
      }
      logical = ""
    }
    {
      if (logical == "") start = NR
      if ($0 ~ /\\[ \t]*$/) { sub(/\\[ \t]*$/, "", $0); logical = logical $0 " "; next }
      logical = logical $0
      flush()
    }
    END { flush() }' "$1"
}

# scan_tree DIR OUT ALLOWED - run the five rules on the sources under DIR; the
# findings go to OUT, one per line; rule 2's counts to OUT.found.  ALLOWED is
# a file of "file name count" lines.
scan_tree() {
  dir=$1; out=$2; allowed=$3
  t=$(mktemp -d "$WORK/scan.XXXXXX")
  mkdir "$t/s" "$t/s/tests"
  : > "$out"; : > "$t/defines"; : > "$t/top"
  for f in "$dir"/*.c "$dir"/*.h "$dir"/tests/*.c "$dir"/tests/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$dir"/}
    strip_comments "$f" > "$t/s/$rel"
    defines_of "$t/s/$rel" "$rel" >> "$t/defines"
    case $rel in */*) ;; *) echo "$rel" >> "$t/top" ;; esac
  done

  # 1. one definition
  [ -f "$dir/$OWNER_HEADER" ] && grep -q "^$OWNER_HEADER|[0-9]*|E_D_REQ_PATH_MAX|" "$t/defines" ||
    echo "R1: $OWNER_HEADER does not define E_D_REQ_PATH_MAX (the scan is blind or the owner is gone)" >> "$out"
  awk -F'|' -v owner="$OWNER_HEADER" -v macro="$OWNER_MACRO" '
    $3 == macro { n++; where = $1 ":" $2
      if ($1 != owner) print "R1: " macro " is defined outside " owner ": " $1 ":" $2
      else if ($4 !~ /E_D_REQ_PATH_MAX/ || $4 !~ /E_D_REQ_PATH_ESCAPE_MAX/)
        print "R1: " macro " in " owner " is not built from E_D_REQ_PATH_MAX and E_D_REQ_PATH_ESCAPE_MAX: " $4 }
    END { if (n == 0) print "R1: " macro " is not defined anywhere"
          else if (n > 1) print "R1: " macro " is defined " n " times" }' "$t/defines" >> "$out"

  # 2. no other cap, counted against the allow file
  awk -F'|' -v owner="$OWNER_HEADER" -v macro="$OWNER_MACRO" '
    $1 != owner && $3 ~ /(CMD|COMMAND)_MAX$/ && $4 !~ macro { cnt[$1 " " $3]++ }
    END { for (k in cnt) print k, cnt[k] }' "$t/defines" | sort > "$out.found"
  awk '
    FILENAME == ARGV[1] { al[$1 " " $2] = $3; next }
    {
      key = $1 " " $2; got[key] = 1
      if (!(key in al))
        print "R2: " $1 ": " $2 " is a cap of its own (" $3 " define(s)): size it from E_D_REQ_CMD_MAX, or list it with its reason"
      else if ($3 + 0 > al[key] + 0)
        print "R2: " $1 ": " $2 " has " $3 " define(s), the allow file says " al[key]
      else if ($3 + 0 < al[key] + 0)
        print "R2: " $1 ": " $2 " has " $3 " define(s), lower the allow line from " al[key]
    }
    END { for (k in al) if (!(k in got)) print "R2: stale allow line, delete it: " k }' \
    "$allowed" "$out.found" >> "$out"

  # 3. no comparison outside the engine (a compile-time check is no comparison)
  while IFS= read -r rel; do
    case $rel in "$OWNER_SOURCE"|"$OWNER_HEADER") continue ;; esac
    grep -n -E "([<>=!]=|[<>])[[:space:]]*$OWNER_MACRO|$OWNER_MACRO[[:space:]]*([<>=!]=|[<>])" "$t/s/$rel" |
      grep -v -F '? 1 : -1]' | while IFS= read -r hit; do
        echo "R3: a comparison with $OWNER_MACRO outside the engine: $rel:${hit%%:*}"
      done >> "$out"
  done < "$t/top"

  # 4. the workarounds stay gone
  while IFS= read -r rel; do
    grep -n -E "(^|[^A-Za-z0-9_])($GONE)([^A-Za-z0-9_]|\$)" "$t/s/$rel" | while IFS= read -r hit; do
      echo "R4: a workaround the owner made unnecessary is back: $rel:${hit%%:*}"
    done >> "$out"
  done < "$t/top"

  # 5. the known command buffers derive from their source
  echo "$DERIVED" | while read -r file name source; do
    awk -F'|' -v file="$file" -v name="$name" -v source="$source" '
      $1 == file && $3 == name && $4 !~ source {
        print "R5: " name " in " file ":" $2 " is not built from " source ": " $4 }' "$t/defines"
  done >> "$out"
  rm -rf "$t"
}

# plant TREE FILE LINE - a copy of the sources with one line added to FILE.
copy_tree() {
  mkdir -p "$1/tests"
  for f in "$SRCDIR"/*.c "$SRCDIR"/*.h; do [ -f "$f" ] && cp "$f" "$1/"; done
  for f in "$SRCDIR"/tests/*.c "$SRCDIR"/tests/*.h; do [ -f "$f" ] && cp "$f" "$1/tests/"; done
}

# canary RULE FILE LINE - the rule must name the offender planted in a copy.
canary() {
  rule=$1; file=$2; line=$3
  rm -rf "$WORK/copy"
  copy_tree "$WORK/copy"
  if [ ! -f "$WORK/copy/$file" ]; then
    problem "canary $rule: $file not found under $SRCDIR (the scan is blind)"
    return
  fi
  printf '%s\n' "$line" >> "$WORK/copy/$file"
  scan_tree "$WORK/copy" "$WORK/canary.out" "$WORK/allowed"
  grep -q "^$rule:" "$WORK/canary.out" ||
    problem "canary $rule: a planted offender in $file was not named (the rule is blind)"
}

# --- the allow file ---
: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 4 || $3 !~ /^[0-9]+$/ || $4 == "") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 " " $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print $1, $2, $3
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  problem "no allow file at $ALLOW"
fi

if [ ! -f "$SRCDIR/$OWNER_HEADER" ] || [ ! -f "$SRCDIR/we_debug.c" ]; then
  problem "canary: $OWNER_HEADER or we_debug.c not found under $SRCDIR (the scan is blind)"
else
  scan_tree "$SRCDIR" "$WORK/real.out" "$WORK/allowed"
  cat "$WORK/real.out" >> "$WORK/problems"
  if [ $LIST -eq 1 ]; then
    awk '{ print $1 "|" $2 "|" $3 "|reason" }' "$WORK/real.out.found"
    exit 0
  fi
  canary R1 we_debug.c '#define E_D_REQ_CMD_MAX 256'
  canary R2 we_startplan.c '#define OWN_CMD_MAX 255'
  canary R3 we_debug.c 'static int own_fits(const char *c) { return strlen(c) >= E_D_REQ_CMD_MAX; }'
  canary R3 we_debug.c 'static int own_sign(const char *c) { return strlen(c) < E_D_REQ_CMD_MAX ? 1 : -1; }'
  canary R3 we_debug.c 'static int own_full(size_t n) { return n == E_D_REQ_CMD_MAX; }'
  canary R4 we_debug.c 'static int e_d_gp_short_path(void) { return 0; }'
  canary R5 we_debug.c '#define E_D_RUN_AGAIN_MAX 300'
  # A line of the allow file that matches nothing must be named as stale.
  cp "$WORK/allowed" "$WORK/allowed.stale"
  echo "we_debug.c NO_SUCH_CMD_MAX 1" >> "$WORK/allowed.stale"
  scan_tree "$SRCDIR" "$WORK/stale.out" "$WORK/allowed.stale"
  grep -q '^R2: stale allow line' "$WORK/stale.out" ||
    problem "canary R2: an allow line that matches nothing was not named stale"
fi

if [ -s "$WORK/problems" ]; then
  echo "check-debug-command: FAIL"
  cat "$WORK/problems"
  exit $BAD
fi
echo "check-debug-command: ok ($(wc -l < "$WORK/real.out.found" | tr -d ' ') listed cap(s), 5 rules, 8 canaries)"
exit 0
