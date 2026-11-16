#!/bin/sh
# check-brk-bounds.sh -- the bracket matcher reads the text through its readers.
#
# Run > Matching BracKet walks the rows of a buffer.  A row holds len+2 bytes
# from s[0] and nothing before it, and the buffer has exactly mxlines rows.
# The matcher stays inside that only while four small functions at the top of
# we_brkmatch.c (the readers) are the only code that indexes a row, indexes
# its bytes or knows how many rows there are; every walk and every skip asks
# them, and a position outside the text reads as "no character".  A step
# that indexes a row itself is how the matcher once read the row above the
# first one, and the byte before a row, and the bytes past a shorter row.
#
#   check-brk-bounds.sh [--srcdir DIR] [--selftest] [FILE]
#
# Rules, applied after comments, string literals and character literals are
# blanked:
#  1. Outside the readers no line subscripts an array ("["), names "bf" or
#     "mxlines", or takes a ".s" or "->s" field.
#  2. A preprocessor line (and the lines a macro continues over) is scanned
#     everywhere, readers included, so a macro cannot carry a raw read out of
#     a reader.
#  3. brk_row_text is called only as the argument of ispif, ispelse or
#     ispendif, so nobody does pointer arithmetic on the text it returns.
#  4. No "##" token pasting anywhere, and no #include other than the four
#     the file has (ctype.h, string.h, messages.h, edit.h): a header or a
#     pasted name could bring a raw reader in under another name.
# A function starts at the "{" in column 0, named by the last line before it
# that has IDENT(; it ends at the "}" in column 0, or at the next column-0
# line that starts an identifier (bodies are indented), so a reader closed by
# an indented "}" does not pass its exemption to the next function.
#
# Limits of a scan of the text: a BUFFER pointer computed from another (the
# rows reached through a pointer the scan never sees named) and a typedef'd
# row pointer taken from the first field are out of its reach.  The unit
# test under ASan, with its hostile frame, is the check for those.
#
# --selftest plants one offence of each kind in a copy of the file and fails
# unless the scan names every one of them.
SRCDIR=${srcdir:-.}
SELFTEST=0
FILE=
READERS="brk_has_row brk_row_len brk_char brk_row_text"
INCLUDES="ctype.h string.h messages.h edit.h"

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    --selftest) SELFTEST=1 ;;
    -*) echo "check-brk-bounds: unknown argument $1" >&2; exit 1 ;;
    *) FILE=$1 ;;
  esac
  shift
done
[ -n "$FILE" ] || FILE=$SRCDIR/we_brkmatch.c

# scan FILE: one "FILE:LINE: FUNCTION: TOKEN" per offence.
scan() {
  awk -v file="$1" -v readers="$READERS" -v includes="$INCLUDES" '
    function isident(ch) { return ch ~ /[A-Za-z0-9_]/ }
    function flag(token) { print file ":" NR ": " fn ": " token }
    BEGIN {
      n = split(readers, r, " ")
      for (k = 1; k <= n; k++) ok[r[k]] = 1
      n = split(includes, r, " ")
      for (k = 1; k <= n; k++) inc[r[k]] = 1
      incomment = 0; inmacro = 0; infn = 0
      fn = "(file scope)"; lastname = ""; prevcode = ""
    }
    {
      line = $0; out = ""; i = 1; len = length(line)
      while (i <= len) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incomment) {
          if (d == "*/") { incomment = 0; i += 2; out = out "  " }
          else { i++; out = out " " }
          continue
        }
        if (d == "/*") { incomment = 1; i += 2; out = out "  "; continue }
        if (d == "//") break
        if (c == "\"" || c == "\x27") {
          q = c; i++; out = out " "
          while (i <= len && substr(line, i, 1) != q) {
            if (substr(line, i, 1) == "\\") i++
            i++; out = out " "
          }
          i++; out = out " "; continue
        }
        out = out c; i++
      }
      code = out
      prep = (inmacro || code ~ /^[ \t]*#/)
      inmacro = (prep && line ~ /\\[ \t]*$/)

      if (prep && line ~ /^[ \t]*#[ \t]*include/) {
        target = line
        gsub(/^[ \t]*#[ \t]*include[ \t]*[<"]/, "", target)
        gsub(/[>"].*$/, "", target)
        if (!(target in inc)) flag("include")
      }
      if (code ~ /##/) flag("paste")

      if (!prep) {
        # A function body is indented, so a column-0 line that starts an
        # identifier is the next declaration: the function has ended even if
        # its closing brace was not in column 0.
        if (infn && code ~ /^[A-Za-z_]/) { infn = 0; fn = "(file scope)" }
        if (!infn && code ~ /^\{/) { fn = lastname; infn = 1 }
        else if (infn && code ~ /^\}/) { infn = 0; fn = "(file scope)" }
        else if (!infn && match(code, /[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) {
          lastname = substr(code, RSTART, RLENGTH)
          sub(/[ \t]*\($/, "", lastname)
        }
      }

      reader = ((fn in ok) && !prep)
      header = (!infn && !prep && code ~ /^[A-Za-z_][A-Za-z0-9_ *]*[ *]brk_row_text[ \t]*\(/)

      if (!reader) {
        if (code ~ /\[/) flag("subscript")
        if (code ~ /(^|[^A-Za-z0-9_])bf([^A-Za-z0-9_]|$)/) flag("bf")
        if (code ~ /(^|[^A-Za-z0-9_])mxlines([^A-Za-z0-9_]|$)/) flag("mxlines")
        if (code ~ /(\.|->)[ \t]*s([^A-Za-z0-9_]|$)/) flag("s-field")
      }

      if (!reader && !header) {
        base = 0; rest = code
        while ((p = index(rest, "brk_row_text")) > 0) {
          at = base + p
          after = substr(code, at + 12)
          prech = (at > 1) ? substr(code, at - 1, 1) : " "
          if (!isident(prech) && !isident(substr(after, 1, 1))) {
            before = substr(code, 1, at - 1)
            if (before ~ /^[ \t]*$/) before = prevcode
            if (after !~ /^[ \t]*\(/ ||
                before !~ /(^|[^A-Za-z0-9_])(ispif|ispelse|ispendif)[ \t]*\([ \t]*$/)
              flag("row-text")
          }
          base = at + 11
          rest = substr(code, base + 1)
        }
      }
      if (code !~ /^[ \t]*$/) prevcode = code
    }' "$1"
}

# plant FILE OUT: FILE with one offence of each kind appended.
plant() {
  {
    cat "$1"
    printf '\nstatic int planted(BUFFER *b, int i, int j)\n{\n'
    printf ' int c = b->bf[i].s[j];      /* bf[0] in a comment is fine */\n'
    printf ' if (i < b->mxlines) return c;\n return "[mxlines]"[0];\n}\n'
    printf '\nstatic int brk_char(BUFFER *b, int i, int j)\n{\n'
    printf '#define RAWC(b,i,j) ((b)->bf[(i)].s[(j)])\n return 0;\n}\n'
    printf '\nstatic int\nplanted_split(BUFFER *b, int i)\n{\n'
    printf ' return b->bf[i].len + b->mxlines;\n}\n'
    printf '\nstatic int planted_arith(BUFFER *b, int i, int j)\n{\n'
    printf ' const char *t = brk_row_text(b, i, 0);\n return *(t + j - 1);\n}\n'
    printf '\nstatic int brk_row_len(BUFFER *b, int i)\n{\n return 0;\n }\n'
    printf 'static int planted_inherit(BUFFER *b, int i)\n{\n'
    printf ' return b->bf[i].len;\n}\n'
    printf '\nstatic int planted_misc(BUFFER *b, int i)\n{\n'
    printf '#include <stdio.h>\n#define GLUE(a) x##a\n return 0;\n}\n'
    printf '\nstatic int planted_ok(BUFFER *b, int i)\n{\n'
    printf ' return ispelse(brk_row_text(b, i, 0));\n}\n'
  } > "$2"
}

if [ $SELFTEST -eq 1 ]; then
  WORK=$(mktemp -d "${TMPDIR:-/tmp}/brk-bounds.XXXXXX") || exit 1
  trap 'rm -rf "$WORK"' EXIT HUP INT TERM
  plant "$FILE" "$WORK/planted.c"
  scan "$WORK/planted.c" > "$WORK/found.txt"
  missing=0
  for want in "planted: subscript" "planted: bf" "planted: mxlines" \
              "planted: s-field" "brk_char: bf" "brk_char: s-field" \
              "planted_split: bf" "planted_split: mxlines" \
              "planted_arith: row-text" "planted_inherit: bf" \
              "planted_misc: include" "planted_misc: paste"; do
    if ! grep -q ": $want\$" "$WORK/found.txt"; then
      echo "check-brk-bounds: SELFTEST FAILED, not named: $want"
      missing=1
    fi
  done
  if grep -q ": planted_ok: " "$WORK/found.txt"; then
    echo "check-brk-bounds: SELFTEST FAILED, a legal call was named"
    missing=1
  fi
  if [ $missing -ne 0 ]; then
    cat "$WORK/found.txt"
    exit 1
  fi
  echo "check-brk-bounds: selftest ok ($(wc -l < "$WORK/found.txt" | tr -d ' ') planted offences named)"
fi

OFFENDERS=$(scan "$FILE")
if [ -n "$OFFENDERS" ]; then
  echo "check-brk-bounds: code other than the readers ($READERS) reads the text:"
  echo "$OFFENDERS"
  exit 1
fi
echo "check-brk-bounds: ok, only the readers index the rows of $FILE"
