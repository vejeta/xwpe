#!/bin/sh
# check-line-ends.sh -- what a line break is, in bytes, has one owner.
#
# Saving a file must change only the lines the user changed.  That holds
# only while one piece of code (we_lineend.c) turns a file's bytes into rows
# and rows into a file's bytes, and keeps the file's line-ending style with
# its buffer.  A second piece that does either is how a mixed file was
# rewritten and a stray carriage return deleted, one site after another.
#
#   check-line-ends.sh [--allow FILE] [--srcdir DIR] [--list] [--selftest]
#
# Rules:
#  1. DTMD_MSDOS (the retired "this window is a DOS file" kind) appears only
#     in edit.h and we_lineend.c.
#  2. A function that reads buffer rows (it names bf[, or takes them as a
#     "char *const *lines" list) and puts a line break
#     byte on a stream or stores one in a byte string (putc/fputc of WPE_WR,
#     WPE_CR, '\n' or '\r'; an element assigned '\n', '\r', WPE_WR, WPE_CR,
#     10 or 13 that is not a row's own terminator s[...]; a string literal
#     with \n or \r handed to fputs, fprintf, strcat, memcpy, fwrite and the
#     like) is a row serialiser.  A helper handed one row (STRING *) that
#     puts the break for its caller is not seen: the caller names bf[ but
#     puts nothing itself.  Outside we_lineend.c each one is counted in
#     tests/line-ends.allow with the pass that moves it onto the owner, or
#     the reason it is a protocol and stays.  Counts may only fall; a line
#     that matches nothing is stale and fails.
#  3. e_buffer_rows_push and buffer_lines_push (bytes to rows) are called
#     only from we_buffer.c and we_lineend.c.
#  4. e_buffer_replace_all, e_buffer_append_text, e_buffer_append_rows and
#     e_buffer_lines_from_text, which split text with no policy, are called
#     only from we_buffer.c and we_lineend.c; the rest use e_eol_replace_all /
#     e_eol_append_text / e_eol_append_rows / e_eol_lines and say whether the
#     text is
#     the buffer's own rows (E_EOL_ROWS) or comes from outside
#     (E_EOL_NORMALIZE).
#
# --selftest feeds a fixture with a blind serialiser to the scan and fails
# unless the scan names it (a scanner that sees nothing is not a check).
# --list prints what rule 2 found, as allow lines.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
ALLOW=
LIST=0
SELFTEST=0
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    *) echo "check-line-ends: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/line-ends.allow

WORK=$(mktemp -d "${TMPDIR:-/tmp}/line-ends.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

# serialisers FILE - "file function count" for each function of FILE that
# names bf[ and puts or stores a line break byte.  Comments are blanked,
# string literals kept.  A function starts at a line in column 0 that names
# an identifier followed by "(" and does not end with ";" (a prototype).
serialisers() {
  awk -v file="$(basename "$1")" '
    function flush() {
      if (fn != "" && has_bf && stores > 0) print file, fn, stores
    }
    BEGIN { incom = 0; fn = ""; has_bf = 0; stores = 0 }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
          if (c == "\"" || c == "\047") instr = 0
          i++; continue
        }
        if (d == "/*") { incom = 1; i += 2; continue }
        if (d == "//") break
        if (c == "\047") {
          # a character constant: copy it whole, its quote is no string
          j = index(substr(line, i + 1), "\047")
          if (substr(line, i + 1, 1) == "\\") j = index(substr(line, i + 3), "\047") + 2
          out = out substr(line, i, j + 1); i += j + 1; continue
        }
        if (c == "\"") instr = 1
        out = out c; i++
      }
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        # a function handed the rows as a list of lines reads them as well
        if (name != "") { flush(); fn = name; has_bf = (out ~ /[*]const[ \t]*[*][ \t]*lines/); stores = 0 }
      }
      if (out ~ /bf\[/) has_bf = 1
      s = out
      while (match(s, /(putc|fputc)[ \t]*[(][ \t]*(WPE_WR|WPE_CR|\047\\n\047|\047\\r\047)/)) {
        stores++; s = substr(s, RSTART + RLENGTH)
      }
      s = out
      while (match(s, /\][ \t]*=[ \t]*(\047\\n\047|\047\\r\047)/)) {
        stores++; s = substr(s, RSTART + RLENGTH)
      }
      s = out
      while (match(s, /[*][A-Za-z_]+[+][+][ \t]*=[ \t]*(\047\\n\047|\047\\r\047)/)) {
        stores++; s = substr(s, RSTART + RLENGTH)
      }
      # a break stored by name or number into an output; a row terminator
      # (s[len] = WPE_WR, the buffer marking a hard row) is not one
      s = out
      while (match(s, /[A-Za-z_0-9>.)]+[[][^]]*\][ \t]*=[ \t]*(WPE_WR|WPE_CR|10|13)([^A-Za-z0-9_]|$)/)) {
        m = substr(s, RSTART, RLENGTH)
        if (m !~ /(^|[>.])s[[]/) stores++
        s = substr(s, RSTART + RLENGTH)
      }
      # a break inside a string literal handed to a put or copy call
      s = out
      while (match(s, /(^|[^A-Za-z0-9_])(fputs|fprintf|strcat|strncat|strcpy|memcpy|fwrite|sprintf|snprintf|write)[ \t]*[(][^;]*"([^"\\]|\\[^nr])*\\[rn]/)) {
        stores++; s = substr(s, RSTART + RLENGTH)
      }
    }
    END { flush() }' "$1"
}

# --selftest: a blind serialiser must be named.
if [ $SELFTEST -eq 1 ]; then
  cat > "$WORK/we_fake.c" <<'EOF'
/* a serialiser outside the owner */
int fake_rows_to_bytes(BUFFER *b, char *out)
{
 int y, n = 0;

 for (y = 0; y < b->mxlines; y++)
 {
  memcpy(out + n, b->bf[y].s, b->bf[y].len);
  n += b->bf[y].len;
  out[n++] = '\n';
 }
 return(n);
}
EOF
  cat > "$WORK/we_fake2.c" <<'EOF'
/* the same, with the break by name and in a string */
int fake_rows_by_name(BUFFER *b, char *out)
{
 int y, n = 0;

 for (y = 0; y < b->mxlines; y++)
 {
  memcpy(out + n, b->bf[y].s, b->bf[y].len);
  n += b->bf[y].len;
  out[n++] = WPE_WR;
 }
 return(n);
}

int fake_rows_by_string(BUFFER *b, FILE *fp)
{
 int y;

 for (y = 0; y < b->mxlines; y++)
  fprintf(fp, "%.*s\r\n", b->bf[y].len, b->bf[y].s);
 return(0);
}

int fake_row_terminator(BUFFER *b, int y)
{
 b->bf[y].s[b->bf[y].len] = WPE_WR;
 return(0);
}
EOF
  serialisers "$WORK/we_fake.c" | grep -q '^we_fake.c fake_rows_to_bytes 1$' || {
    echo "check-line-ends: FAIL (selftest: the scan does not see a blind serialiser)"
    exit $BAD
  }
  serialisers "$WORK/we_fake2.c" > "$WORK/fake2"
  grep -q '^we_fake2.c fake_rows_by_name 1$' "$WORK/fake2" &&
  grep -q '^we_fake2.c fake_rows_by_string 1$' "$WORK/fake2" &&
  ! grep -q 'fake_row_terminator' "$WORK/fake2" || {
    echo "check-line-ends: FAIL (selftest: a break by name or in a string is not seen, or a row terminator is)"
    exit $BAD
  }
  echo "check-line-ends: selftest ok"
  exit 0
fi

# Rule 1.
for f in "$SRCDIR"/*.c "$SRCDIR"/*.h; do
  [ -f "$f" ] || continue
  case ${f##*/} in edit.h|we_lineend.c) continue ;; esac
  grep -n 'DTMD_MSDOS' "$f" | while IFS= read -r hit; do
    echo "DTMD_MSDOS outside the owner: ${f##*/}:${hit%%:*}"
  done >> "$WORK/problems"
done

# Rule 4.
for f in "$SRCDIR"/we_*.c; do
  [ -f "$f" ] || continue
  case ${f##*/} in we_buffer.c|we_lineend.c) continue ;; esac
  grep -n -e 'e_buffer_replace_all[ \t]*(' -e 'e_buffer_lines_from_text[ \t]*(' -e 'e_buffer_append_text[ \t]*(' -e 'e_buffer_append_rows[ \t]*(' "$f" | while IFS= read -r hit; do
    echo "text to rows without saying where the text came from: ${f##*/}:${hit%%:*}"
  done >> "$WORK/problems"
done

# Rule 3.
for f in "$SRCDIR"/we_*.c; do
  [ -f "$f" ] || continue
  case ${f##*/} in we_buffer.c|we_lineend.c) continue ;; esac
  grep -n -e 'e_buffer_rows_push[ \t]*(' -e 'buffer_lines_push[ \t]*(' "$f" | while IFS= read -r hit; do
    echo "bytes to rows outside the owner: ${f##*/}:${hit%%:*}"
  done >> "$WORK/problems"
done

# Rule 2, with its canary: the self test shows the scan sees a blind
# serialiser; the allow file's stale-entry rule shows it still sees the
# listed ones.
: > "$WORK/scan"
for f in "$SRCDIR"/we_*.c; do
  [ -f "$f" ] || continue
  case ${f##*/} in we_lineend.c) continue ;; esac
  serialisers "$f" >> "$WORK/scan"
done
sort "$WORK/scan" > "$WORK/found"
[ -f "$SRCDIR/we_lineend.c" ] || problem "canary: we_lineend.c not found under $SRCDIR"
sh "$0" --selftest > /dev/null 2>&1 || problem "canary: the scan does not see a blind serialiser (run $0 --selftest)"

if [ $LIST -eq 1 ]; then
  awk '{ print $1 "|" $2 "|" $3 "|PASS|reason" }' "$WORK/found"
  exit 0
fi

: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || $4 == "") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 " " $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print $1, $2, $3, $4
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  problem "no allow file at $ALLOW"
fi

awk '
  FILENAME == ARGV[1] { al[$1 " " $2] = $3; pass[$1 " " $2] = $4; next }
  {
    key = $1 " " $2; got[key] = 1
    if (!(key in al)) {
      print "offender: " $1 ": " $2 " turns rows into bytes outside we_lineend.c: route it through e_eol_text, or list it with its pass"; bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 ": " $2 " has " $3 ", the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 ": " $2 " has " $3 ", lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in got)) { print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-line-ends: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
echo "check-line-ends: ok ($(awk '{ n += $3 } END { print n + 0 }' "$WORK/found") serialiser line(s) in $(wc -l < "$WORK/found" | tr -d ' ') listed function(s))"
exit 0
