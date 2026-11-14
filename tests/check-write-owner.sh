#!/bin/sh
# check-write-owner.sh -- the user's text is written by one owner.
#
# When a write of the user's text fails the user must be told once, with the
# path and the reason, and the text must stay where it is.  That holds only
# while one piece of code (we_write.c) opens the file, judges the write and
# builds the sentence, and every caller reads the outcome it returns.  A
# second piece that opens a file for writing, drops the result of a save, or
# spells the old "Can't open File %s" is how a failed save went unseen, one
# site after another.
#
#   check-write-owner.sh [--allow FILE] [--srcdir DIR] [--nm NM] [--list]
#                        [--selftest] [OBJECT...]
#
# Rules (the numbers are the design's):
#  1. Symbols (nm -u of the editor's objects): e_write and e_eol_write are
#     undefined only in we_write.o.
#  2. Every way to open a file for writing outside we_write.c: fopen, freopen
#     and fdopen with a mode that starts with w or a; open and openat whose
#     flags name O_WRONLY, O_RDWR, O_CREAT or O_TRUNC; creat.  Comments are
#     stripped and a call split over lines is joined.
#  4. A statement that is a save (e_save, e_write, wpe_write_file, _range,
#     _save, _text, _fd) alone or under (void): its outcome is dropped.
#  5. A save compared with numbers: on the call (== / != WPE_ESC, 0 or
#     E_SAVE_DECLINED) or through the variable that took its result, and any
#     E_SAVE_DECLINED.  A caller tests WPE_WRITE_WRITTEN, DECLINED or FAILED.
#  6. The old message: ERR_FOPEN and the literal "Can't open File" outside
#     we_write.c.
# Rule 3 (the companion names) and rule 7 (the crash handler's order) come
# with the pass that moves them.
#
# Offenders still to go are lines of tests/write-owner.allow:
#   file|rN:function|count|pass|reason
# The count may only fall, a line that matches nothing is stale and fails,
# and a line with a pass is deleted by that pass.  --list prints what was
# found as allow lines.  --selftest feeds a fixture with one of each
# offence to the scan and fails unless the scan names every one (a scanner
# that sees nothing is not a check).
#
# Environment (set by `make check`): NM, ONE_OWNER_OBJECTS, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
HERE=$(dirname "$0")
ALLOW=
LIST=0
SELFTEST=0
NMTOOL=${NM:-nm}
OBJECTS=
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --list) LIST=1 ;;
    --selftest) SELFTEST=1 ;;
    -*) echo "check-write-owner: unknown argument $1" >&2; exit $BAD ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS
[ -n "$ALLOW" ] || ALLOW=$HERE/write-owner.allow

WORK=$(mktemp -d "${TMPDIR:-/tmp}/write-owner.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"
problem() { echo "$*" >> "$WORK/problems"; }

# scan FILE - one line "rule file function count" for each offence of the
# rules 2, 4, 5 and 6 in FILE.  Comments are blanked, string literals kept;
# a function starts at a line in column 0 that names an identifier followed
# by "(" and does not end with ";" (a prototype); a statement is the text up
# to the next ; { or }.
scan() {
  awk -v file="$(basename "$1")" '
    function ident_end(s, i) { while (i <= length(s) && substr(s, i, 1) ~ /[A-Za-z0-9_]/) i++; return i }
    # the index just past the ")" that closes the "(" at position p
    function close_paren(s, p,   depth, i, c) {
      depth = 0
      for (i = p; i <= length(s); i++) {
        c = substr(s, i, 1)
        if (c == "\"") { i++; while (i <= length(s) && substr(s, i, 1) != "\"") { if (substr(s, i, 1) == "\\") i++; i++ } continue }
        if (c == "(") depth++
        else if (c == ")") { depth--; if (depth == 0) return i + 1 }
      }
      return 0
    }
    function note(rule, n) { cnt[rule] += n }
    function flush_fn(   r) {
      if (fn == "") return
      for (r in cnt) if (cnt[r] > 0) print r, file, fn, cnt[r]
      delete cnt; delete taken
    }
    function prev_char(s, p,   i, c) {
      for (i = p - 1; i >= 1; i--) { c = substr(s, i, 1); if (c != " " && c != "\t") return c }
      return ""
    }
    function prev_word_else(s, p,   head) {
      head = substr(s, 1, p - 1); sub(/[ \t]+$/, "", head)
      return head ~ /(^|[^A-Za-z0-9_])else$/
    }
    function statement(s,   t, p, q, e, rest, name, v, c, m, ident) {
      # rule 2: every way to open a file for writing
      t = s
      while (match(t, /(^|[^A-Za-z0-9_])(fopen|freopen|fdopen)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        if (e > 0 && substr(t, p, e - p) ~ /"[wa]/) note("r2", 1)
        t = substr(t, p + 1)
      }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_])(open|openat)[ \t]*[(]/)) {
        p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        if (e > 0 && substr(t, p, e - p) ~ /(O_WRONLY|O_RDWR|O_CREAT|O_TRUNC)/) note("r2", 1)
        t = substr(t, p + 1)
      }
      t = s
      while (match(t, /(^|[^A-Za-z0-9_])creat[ \t]*[(]/)) {
        note("r2", 1)
        t = substr(t, RSTART + RLENGTH)
      }
      # rules 4 and 5: the saves
      t = s; gsub(/\([ \t]*void[ \t]*\)/, " ", t)
      while (match(t, /(^|[^A-Za-z0-9_])(e_save|e_write|wpe_write_file|wpe_write_range|wpe_write_save|wpe_write_text|wpe_write_fd)[ \t]*[(]/)) {
        name = substr(t, RSTART, RLENGTH); sub(/^[^A-Za-z]/, "", name); sub(/[ \t]*[(]$/, "", name)
        q = RSTART; p = RSTART + RLENGTH - 1; e = close_paren(t, p)
        np = q
        if (substr(t, q, 1) !~ /[A-Za-z_]/) np = q + 1
        if (e > 0) {
          rest = substr(t, e)
          c = prev_char(t, np)
          if (rest ~ /^[ \t]*;/ && (c == "" || c == ";" || c == "{" || c == "}" || c == ")" || prev_word_else(t, np))) note("r4", 1)
          # the int returns of the owner (0 or -1) may be compared with 0; the
          # saves that return a status or a key are read by name
          status_call = (name ~ /^(e_save|e_write|wpe_write_save)$/)
          if (status_call && rest ~ /^[ \t]*(==|!=)[ \t]*(WPE_ESC|E_SAVE_DECLINED|0)([^A-Za-z0-9_]|$)/) note("r5", 1)
          head = substr(t, 1, q); if (status_call && head ~ /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[ \t]*[(]?[ \t]*$/ && head !~ /(==|!=|<=|>=)[ \t]*[(]?[ \t]*$/) {
            v = head; sub(/[ \t]*=[ \t]*[(]?[ \t]*$/, "", v); sub(/.*[^A-Za-z0-9_]/, "", v)
            if (v != "") taken[v] = 1
          }
        }
        t = substr(t, p + 1)
      }
      for (v in taken)
        if (s ~ ("(^|[^A-Za-z0-9_])" v "[ \t]*(==|!=)[ \t]*(WPE_ESC|E_SAVE_DECLINED|0)([^A-Za-z0-9_]|$)")) note("r5", 1)
      m = s
      while (match(m, /(^|[^A-Za-z0-9_])E_SAVE_DECLINED([^A-Za-z0-9_]|$)/)) { note("r5", 1); m = substr(m, RSTART + RLENGTH) }
      # rule 6: the old message
      m = s
      while (match(m, /(^|[^A-Za-z0-9_])ERR_FOPEN([^A-Za-z0-9_]|$)/)) { note("r6", 1); m = substr(m, RSTART + RLENGTH) }
      m = s
      while (match(m, /Can[\\\047]*t open File/)) { note("r6", 1); m = substr(m, RSTART + RLENGTH) }
    }
    BEGIN { incom = 0; fn = "-"; stmt = "" }
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
        if (d == "/*") { incom = 1; out = out " "; i += 2; continue }
        if (d == "//") break
        if (c == "\047") {
          j = index(substr(line, i + 1), "\047")
          if (substr(line, i + 1, 1) == "\\") j = index(substr(line, i + 3), "\047") + 2
          out = out substr(line, i, j + 1); i += j + 1; continue
        }
        if (c == "\"") instr = 1
        out = out c; i++
      }
      if (out ~ /^#/) next
      # a table of strings at file scope is named by its array, not by the
      # function above it
      if (out ~ /^(static[ \t]+)?(const[ \t]+)?char[ \t]*[*][ \t]*[A-Za-z_0-9]+\[\][ \t]*=/) {
        name = out; sub(/\[\].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        statement(stmt); stmt = ""; flush_fn(); fn = name
      }
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{]|enum|static[ \t]+[A-Za-z_ *]*=)/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        if (name != "") { statement(stmt); stmt = ""; flush_fn(); fn = name }
      }
      if (out ~ /^[ \t]*$/) next
      stmt = stmt " " out
      if (out ~ /[;{}][ \t]*$/) { statement(stmt); stmt = "" }
    }
    END { statement(stmt); flush_fn() }' "$1"
}

# --selftest: each rule must name its offence in a fixture.
if [ $SELFTEST -eq 1 ]; then
  cat > "$WORK/we_fake.c" <<'EOF'
/* one of each offence, and what is no offence */
int fake_open(FENSTER *f)
{
 FILE *fp = fopen("/tmp/x", "w");
 int fd = open("/tmp/y",
               O_WRONLY | O_CREAT, 0600);
 FILE *in = fopen("/tmp/z", "r");
 int rd = open("/tmp/z", O_RDONLY);
 return(fd);
}

int fake_dropped(FENSTER *f)
{
 e_save(f);
 (void)e_save(f);
 if (f->save)
  e_save(f);
 wpe_write_range(f, "p",
                 0, 0, 1, 1, 0, &o);
 return(e_save(f));
}

int fake_numbers(FENSTER *f)
{
 int ret;

 if (e_save(f) == WPE_ESC)
  return(1);
 ret = e_save(f);
 if (ret == WPE_ESC)
  return(2);
 if (ret == WPE_WRITE_FAILED)
  return(3);
 return(0);
}

int fake_message(FENSTER *f)
{
 e_error(e_msg[ERR_FOPEN], 0, f->fb);
 return(E_SAVE_DECLINED);
}
EOF
  scan "$WORK/we_fake.c" | sort > "$WORK/fake.found"
  for want in "r2 we_fake.c fake_open 2" "r4 we_fake.c fake_dropped 4" \
              "r5 we_fake.c fake_numbers 2" "r5 we_fake.c fake_message 1" \
              "r6 we_fake.c fake_message 1"; do
    grep -qx "$want" "$WORK/fake.found" || {
      echo "check-write-owner: FAIL (selftest: the scan does not name \"$want\"; it found:)"
      cat "$WORK/fake.found"
      exit $BAD
    }
  done
  echo "check-write-owner: selftest ok"
  exit 0
fi

# Rule 1: the symbols.
: > "$WORK/symfound"
if command -v "$NMTOOL" >/dev/null 2>&1 && [ -n "$OBJECTS" ]; then
  for o in $OBJECTS; do
    [ -f "$o" ] || [ -f "$SRCDIR/$o" ] || continue
    [ -f "$o" ] || o=$SRCDIR/$o
    base=$(basename "$o")
    [ "$base" = we_write.o ] && continue
    "$NMTOOL" -u "$o" 2>/dev/null | awk -v obj="$base" '
      NF >= 2 && $(NF-1) == "U" {
        n = $NF; sub(/@.*/, "", n)
        if (n == "e_write" || n == "e_eol_write") print "r1", obj, n, 1
      }'
  done > "$WORK/symfound"
fi

# Rules 2, 4, 5, 6: the source.
: > "$WORK/srcfound"
for f in "$SRCDIR"/we_*.c "$SRCDIR"/We*.c; do
  [ -f "$f" ] || continue
  case ${f##*/} in we_write.c) continue ;; esac
  scan "$f" >> "$WORK/srcfound"
done
{
  awk '{ print $2 "|" $1 ":" $3 "|" $4 }' "$WORK/srcfound"
  awk '{ print $2 "|" $1 ":" $3 "|" $4 }' "$WORK/symfound"
} | sort > "$WORK/found"

[ -f "$SRCDIR/we_write.c" ] || problem "canary: we_write.c not found under $SRCDIR"
sh "$0" --selftest > /dev/null 2>&1 || problem "canary: the scan is blind (run $0 --selftest)"

if [ $LIST -eq 1 ]; then
  awk -F'|' '{ print $1 "|" $2 "|" $3 "|PASS|reason" }' "$WORK/found"
  exit 0
fi

: > "$WORK/allowed"
if [ -f "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || $4 == "" || $5 == "") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 "|" $2
      if (key in seen) { print "duplicate allow line for " key > "/dev/stderr"; bad = 1; next }
      seen[key] = 1
      print $1 "|" $2 "|" $3 "|" $4
    }
    END { exit bad }' "$ALLOW" > "$WORK/allowed" ||
    problem "the allow file $ALLOW has malformed or duplicate lines"
else
  problem "no allow file at $ALLOW"
fi

awk -F'|' '
  FILENAME == ARGV[1] { al[$1 "|" $2] = $3; pass[$1 "|" $2] = $4; next }
  {
    key = $1 "|" $2; got[key] = $3
    if (!(key in al)) {
      print "offender: " $1 " " $2 ": the user'"'"'s text is written outside the owner (we_write.c): move it onto the owner, or list it with its pass"; bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 " " $2 " has " $3 ", the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 " " $2 " has " $3 ", lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in got)) { print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-write-owner: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
echo "check-write-owner: ok ($(wc -l < "$WORK/found" | tr -d ' ') counted site(s), all in the allow file)"
exit 0
