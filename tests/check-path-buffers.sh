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
#   check-path-buffers.sh [--srcdir DIR]
#
# Five rules, each with a canary: the script plants an offender in a copy of
# the sources and fails when the rule does not name it, so a scanner gone
# blind fails instead of passing.  This is the memory-safety part of the
# fence; the rules that count every path number and copy in the editor come
# with the passes that move those sites.
#
#  1. One definition.  WPE_PATH_MAX and WPE_NAME_MAX are each defined once,
#     in we_path.h, as a number.
#  2. A field's text leaves through the owner.  No strcpy, strcat or
#     sprintf takes ->wstr[k]->txt as its source (a bounded strncpy of one
#     is a cut, which belongs to the pass that moves the cuts).
#  3. A refusal is read.  A call of wpe_path_copy, wpe_path_join,
#     wpe_path_cat, wpe_field_take or e_opt_field_take standing alone as a
#     statement, or cast to void, fails.
#  4. The size is asked of the caller.  Every call of e_add_arguments passes
#     the size of its buffer (sizeof, or a variable named size).
#  5. The overflows stay gone.  A file manager name of 128 bytes, a Find
#     text of 80 bytes and the unbounded history copy appear in no top level
#     source (comments are not read).
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-path-buffers: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

OWNER_HEADER=we_path.h

WORK=$(mktemp -d "${TMPDIR:-/tmp}/path-buffers.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

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

# scan_tree DIR OUT - run the rules on the top level sources under DIR; the
# findings go to OUT, one per line, each starting with its rule.
scan_tree() {
  dir=$1; out=$2
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

  # Rule 2: a string function with a field's text as its source.
  awk -F'|' '
    {
      s = $3
      if (s ~ /(^|[^A-Za-z0-9_])\(?(strcpy|strcat|sprintf)\)?[ \t]*\(/ && s ~ /->wstr\[[^]]*\]->txt/)
        print "R2: " $1 ":" $2 ": a string function reads a field\047s text; use e_opt_field_take"
    }' "$t/stmts" >> "$out"

  # Rule 3: the result of a refusing writer is dropped.
  awk -F'|' '
    BEGIN { names = "(wpe_path_copy|wpe_path_join|wpe_path_cat|wpe_field_take|e_opt_field_take)" }
    {
      s = $3
      alone = "^(else[ \t]+)?" names "[ \t]*\\("
      voided = "\\([ \t]*void[ \t]*\\)[ \t]*" names "[ \t]*\\("
      if (s ~ alone) print "R3: " $1 ":" $2 ": the result of a call that can refuse is not read"
      else if (s ~ voided) print "R3: " $1 ":" $2 ": the result of a call that can refuse is cast away"
    }' "$t/stmts" >> "$out"

  # Rule 4: a call of e_add_arguments gives its buffer's size.
  awk -F'|' '
    {
      s = $3
      if (s ~ /^(static[ \t]+)?int[ \t]+e_add_arguments/) next
      if (s ~ /e_add_arguments[ \t]*\(/ && s !~ /e_add_arguments[ \t]*\([^,]*,[ \t]*(sizeof|size[ \t]*,)/)
        print "R4: " $1 ":" $2 ": e_add_arguments is called without the size of its buffer"
    }' "$t/stmts" >> "$out"

  # Rule 5: the overflows of the first pass stay gone.
  for f in "$t"/s/*.c "$t"/s/*.h; do
    [ -f "$f" ] || continue
    rel=${f#"$t"/s/}
    awk -v rel="$rel" '
      /char[ \t]+filen\[128\]/ { print "R5: " rel ":" NR ": a file name picked in the manager is kept in 128 bytes" }
      /char[ \t]+(search|replace)\[80\]/ { print "R5: " rel ":" NR ": a Find text is kept in 80 bytes under a field of 128" }
      /\(strcpy\)[ \t]*\(/ { print "R5: " rel ":" NR ": a history entry is copied into a field without its capacity" }' "$f" >> "$out"
  done
}

# canary RULE CODE - plant CODE in a copy of the sources, scan, and fail
# unless RULE names it.
canary() {
  rule=$1; code=$2
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  cp "$SRCDIR"/*.c "$SRCDIR"/*.h "$c"/ 2>/dev/null
  printf '%s\n' "$code" >> "$c/we_mouse.c"
  scan_tree "$c" "$c/out"
  if ! grep -q "^$rule:" "$c/out"; then
    echo "canary $rule: a planted offender was not found; the scan is blind"
    return 1
  fi
  return 0
}

blind=0
canary R1 '#define WPE_PATH_MAX 1024' || blind=1
canary R2 'void planted(char *buf, W_OPTSTR *o) { strcpy(buf, o->wstr[0]->txt); }' || blind=1
canary R3 'void planted(char *a, const char *b) { wpe_path_copy(a, 4, b); }' || blind=1
canary R3 'void planted(char *a, const char *b) { (void)wpe_path_join(a, 4, b, b); }' || blind=1
canary R4 'void planted(FENSTER *f) { char s[9]; e_add_arguments(s, "x", f, 0, 0, NULL); }' || blind=1
canary R5 'void planted(void) { char filen[128]; }' || blind=1

scan_tree "$SRCDIR" "$WORK/problems"
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
