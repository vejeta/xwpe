#!/bin/sh
# check-keymap.sh -- the key table owns what a key does.
#
# What a key does is a row of the table in we_bindings.c, read through
# wpe_binding_action(); the text that names a key to the user is made from the
# same rows.  Any other place that mentions a key constant or writes a key's
# name is a second table: it drifts, and the menu says Shift-F4 for a key that
# is bound to Shift-F5.  This script fails the build on every such place that is
# not named, counted and given its reason in tests/check-keymap.allow.
#
#   check-keymap.sh [--allow FILE] [--srcdir DIR] [--list] [--hits] [--self-test]
#
# It scans every we_*.c, every We*.c and every header except keys.h,
# unixkeys.h and we_bindings.[ch] for
#   * a use of a key constant in code, in any form: a case label (also in
#     parentheses or as a range), a comparison, a range test (i >= F1 && i <= F10),
#     a key in an initializer or array ({F2, AF2}), a key assigned to a variable
#     or returned, a macro in a header;
#   * a string literal that names a key (Alt-F2, Ctrl F4, Shift-F3, ^X).
# The key constants are read from keys.h and unixkeys.h when the script runs,
# so a new name is covered; a name belongs to the key family when it matches
# FAMILY below.  Comments are ignored.
#
# Allow file: one line per function and key, `file:function:NAME count reason`.
# NAME is a key constant or STR (key names in strings), or * for every key use
# of the function (not for a key loop, where each key is named); the function is
# `-` for a header or file-level use.  The count must be exact: more is a new use,
# fewer is a stale line, so the file shrinks as the offenders go.  The reason is
# one of the seven in REASONS.  --list prints what was found as allow-file lines to
# start from; --hits prints each use.
#
# What the counts do not see: a `*` line counts the key uses of a whole
# function, and a key loop's lines count each key.  Swapping one key for
# another inside a `*` function, or exchanging two keys of a key loop whose
# counts stay the same, keeps every count and passes.  The check finds a key
# added or removed, not a key changed in place; the table's own test
# (test_bindings) is what pins what each bound key does.
#
# The scanner proves itself first on a fixture with one line for each form it
# must catch and a comment and a string it must not; a scanner that misses a
# form, or finds what a comment says, fails there.
#
# Exit: 0 clean, 1 offenders, a stale allow or a blind scanner.

ALLOW=
SRCDIR=${srcdir:-.}
LIST=0
SELFTEST_ONLY=0
HERE=$(dirname "$0")
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --list) LIST=1 ;;
    --hits) LIST=2 ;;
    --self-test) SELFTEST_ONLY=1 ;;
    *) echo "check-keymap: unknown argument $1"; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/check-keymap.allow

# The names of the key family: F-keys, Alt and Ctrl letters, the cursor block,
# the Sun keys, the control codes the editor spells by name, the distinct codes
# of the decoder.
FAMILY='^(F[0-9]+|[SCA]F[0-9]+|Alt[A-Za-z0-9]+|Ctrl[A-Za-z]+|C(UP|DO|LE|RI|CLE|CRI|BUP|BDO|PS1|END)|POS1|ENDE|BUP|BDO|EINFG|ENTF|CENTF|CEINFG|ShiftDel|ShiftEin|SABS|SC(LE|RI|UP|DO)|WPE_(TAB|BTAB|CR|ESC|DC|CS_[A-Z]+|CTRL_[A-Z_]+|CA_[A-Z]+|SCROLL_UP|SCROLL_DOWN|PASTE_BEG|PASTE_END|LSP_MENU|AI_MENU|RESIZE)|STOP|AGAIN|PROPS|UNDO|FRONT|COPY|OPEN|PASTE|FID|CUT|HELP)$'

# Why a place may use a key.  The design's seven, and two honest debts that are
# not reasons but a list of what the table has not taken yet: "not yet in the
# table, context NAME (K-d1)" for a key a context of the table will decide, and
# "not yet generated from the table, K-d1" for a hand-written key name in a
# message.  The file manager and the editor's own keys are contexts, not reasons.
REASONS='dialog field editing|menu navigation|AI pane text keys|compose|help viewer paging|key-codes view names keys|self-test table'

# The key loops: the functions that read a key and choose what it does.  Their
# allow lines name each key, so a decision planted among them is a new line.
KEYLOOPS='e_eingabe|e_tst_cur|WpeHandleFileManager|e_data_eingabe|WpeHandleMainmenu|WpeHandleSubmenu_run|main'

WORK=$(mktemp -d "${TMPDIR:-/tmp}/keymap.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# The key constants, read from the headers.
for h in keys.h unixkeys.h; do
  [ -f "$SRCDIR/$h" ] && grep -h '^#define[ 	][A-Za-z_][A-Za-z0-9_]*[ 	]' "$SRCDIR/$h"
done | awk '{print $2}' | grep -E "$FAMILY" | sort -u > "$WORK/names"
if [ ! -s "$WORK/names" ]; then
  echo "check-keymap: no key constants read from keys.h and unixkeys.h"
  exit $BAD
fi
NAMES=$(awk 'NR > 1 { printf "|" } { printf "%s", $0 }' "$WORK/names")

# scan FILE...: print "file:line:function:KEY:NAME" for each use of a key
# constant and "file:line:function:STR:" for each string that names a key.
# Comments are dropped; a character or string literal is not code, but a
# string is kept apart for the key-name rule.
scan() {
  awk -v NAMES="$NAMES" '
  function strip(line,    out, lits, i, c, d, n) {
    out = ""; lits = ""; n = length(line); i = 1
    while (i <= n) {
      c = substr(line, i, 1); d = substr(line, i, 2)
      if (incomment) {
        if (d == "*/") { incomment = 0; i += 2; out = out " " } else i++
      } else if (d == "/*") { incomment = 1; i += 2 }
      else if (d == "//") { break }
      else if (c == "\"") {
        i++; lits = lits "\001"
        while (i <= n && substr(line, i, 1) != "\"") {
          if (substr(line, i, 1) == "\\") { lits = lits substr(line, i, 2); i += 2 }
          else { lits = lits substr(line, i, 1); i++ }
        }
        i++; out = out "\"\""
      } else if (c == "\047") {
        i++
        while (i <= n && substr(line, i, 1) != "\047") {
          if (substr(line, i, 1) == "\\") i += 2; else i++
        }
        i++; out = out "0"
      } else { out = out c; i++ }
    }
    CODE = out; LITS = lits
  }
  FNR == 1 { fn = "-"; incomment = 0 }
  {
    strip($0)
    code = CODE
    if (code ~ /^[A-Za-z_][A-Za-z0-9_ \t\*]*[ \t\*][A-Za-z_][A-Za-z0-9_]*[ \t]*\(/ && code !~ /;[ \t]*$/ \
        && code !~ /^(static[ \t]+)?(struct|typedef|union|enum)[ \t]/ && code !~ /^(else|return|if|while|for|switch|case|do|goto)[ \t(]/) {
      s = code; sub(/\(.*/, "", s); n = split(s, a, /[ \t\*]+/); fn = a[n]
    } else if (code ~ /^[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/ && code !~ /;[ \t]*$/ \
               && code !~ /^(else|return|if|while|for|switch|case|do|goto|sizeof)[ \t(]/) {
      s = code; sub(/\(.*/, "", s); fn = s
    }
    rest = code
    while (match(rest, "(^|[^A-Za-z0-9_.>])(" NAMES ")([^A-Za-z0-9_]|$)")) {
      t = substr(rest, RSTART, RLENGTH)
      sub(/^[^A-Za-z0-9_]/, "", t); sub(/[^A-Za-z0-9_]$/, "", t)
      print FILENAME ":" FNR ":" fn ":KEY:" t
      rest = substr(rest, RSTART + RLENGTH - 1)
    }
    if (LITS ~ /(Alt|Ctrl|Shift)[ +-]?F?[0-9]/ || LITS ~ /(Alt|Ctrl|Shift)[ +-][A-Z]/ ||
        LITS ~ /(^|[^A-Za-z0-9_])F([1-9]|1[0-2])([^0-9A-Za-z]|$)/ || LITS ~ /\^[A-Z]/)
      print FILENAME ":" FNR ":" fn ":STR:"
  }' "$@"
}

# The scanner on a fixture whose answer is known: one line per form.
cat > "$WORK/fixture.c" <<'EOF'
/* a comment that says case CtrlS: and c == AltX is nothing */
int quiet(int c)
{
 /* c == F2 in a comment */
 puts("prose with Altitude and Ctrlless and Shifty and F-keys");
 return c == 'x' ? CtrlLike : 0;
}

int loud(FENSTER *f, int c)
{
 int k = AltX;
 switch (c)
 {
  case CtrlS:
   break;
  case (F2):
   break;
  case F3 ... F4:
   break;
 }
 if (c >= F1 && c <= F10)
  return 1;
 if (c == k || WPE_ESC != c)
  return 2;
 puts("Alt-F2 Save");
 puts("Close   ^W");
 puts("an ordinary text");
 return 0;
}

static int keys[] = {F5, AF5, CtrlP};

void texts(void)
{
 puts("Alt-X");
 puts("Ctrl+S");
 puts("Shift-Tab");
 puts("press F2 to save");
 puts("Ctrl C");
}
EOF
cat > "$WORK/WeFixture.c" <<'EOF'
int other(int c)
{
 return c == CtrlK;
}
EOF
cat > "$WORK/fixture.h" <<'EOF'
#define IS_SAVE(c) ((c) == F2)
EOF
cat > "$WORK/fixture.want" <<'EOF'
WeFixture.c:3:other:KEY:CtrlK
fixture.c:11:loud:KEY:AltX
fixture.c:14:loud:KEY:CtrlS
fixture.c:16:loud:KEY:F2
fixture.c:18:loud:KEY:F3
fixture.c:18:loud:KEY:F4
fixture.c:21:loud:KEY:F1
fixture.c:21:loud:KEY:F10
fixture.c:23:loud:KEY:WPE_ESC
fixture.c:25:loud:STR:
fixture.c:26:loud:STR:
fixture.c:31:loud:KEY:F5
fixture.c:31:loud:KEY:AF5
fixture.c:31:loud:KEY:CtrlP
fixture.c:35:texts:STR:
fixture.c:36:texts:STR:
fixture.c:37:texts:STR:
fixture.c:38:texts:STR:
fixture.c:39:texts:STR:
fixture.h:1:-:KEY:F2
EOF
( cd "$WORK" && scan WeFixture.c fixture.c fixture.h ) > "$WORK/fixture.got"
if ! cmp -s "$WORK/fixture.got" "$WORK/fixture.want"; then
  echo "check-keymap: FAIL, the scanner is blind or sees what it must not."
  echo "--- fixture, wanted"; cat "$WORK/fixture.want"
  echo "--- fixture, found"; cat "$WORK/fixture.got"
  exit $BAD
fi
[ "$SELFTEST_ONLY" = 1 ] && { echo "check-keymap: self-test ok"; exit 0; }

# The tree.
FILES=$(cd "$SRCDIR" && ls we_*.c We*.c *.h 2>/dev/null | grep -v -e '^keys\.h$' -e '^unixkeys\.h$' -e '^we_bindings\.[ch]$')
( cd "$SRCDIR" && scan $FILES ) > "$WORK/hits"

# What the allow file names: "file:function:NAME count".
if [ -f "$ALLOW" ]; then
  grep -v '^[ 	]*#' "$ALLOW" | grep -v '^[ 	]*$' > "$WORK/allow"
else
  : > "$WORK/allow"
fi

# Every allow line has a count and a reason from the set.
BADREASON=$(awk -v R="$REASONS" 'BEGIN { n = split(R, r, "|"); for (i = 1; i <= n; i++) ok[r[i]] = 1 }
  { key = $1; cnt = $2; if (cnt !~ /^[0-9]+$/) { print key " has no count"; next }
    sub(/^[^ ]+[ ]+[^ ]+[ ]+/, "", $0)
    if (!($0 in ok) && $0 !~ /^not yet in the table, context [a-z_ ]+ \(K-d1\)$/ &&
        $0 !~ /^not yet generated from the table, K-d1$/)
      print key " has the reason \"" $0 "\", which is not in the set" }' "$WORK/allow")
if [ -n "$BADREASON" ]; then
  echo "check-keymap: FAIL, an allow line with no valid count or reason:"
  echo "$BADREASON"
  exit $BAD
fi

if [ $LIST = 1 ]; then
  awk -F: '{ k = $1 ":" $3 ":*"; c[k]++ } END { for (k in c) print k " " c[k] }' "$WORK/hits" | sort
  exit 0
fi
if [ $LIST = 2 ]; then
  cat "$WORK/hits"
  exit 0
fi

awk -F: -v ALLOWFILE="$WORK/allow" -v LOOPS="$KEYLOOPS" '
  BEGIN {
    nl = split(LOOPS, l, "|"); for (i = 1; i <= nl; i++) loop[l[i]] = 1
    while ((getline line < ALLOWFILE) > 0) { split(line, p, " "); want[p[1]] = p[2] }
  }
  {
    fnkey = $1 ":" $3
    name = fnkey ":" ($4 == "STR" ? "STR" : $5)
    got[name]++
    total[fnkey]++
    if (!(name in firstline)) firstline[name] = $1 ":" $2
    if (!(fnkey in firstfn)) firstfn[fnkey] = $1 ":" $2
  }
  END {
    # a whole-function line (NAME is *) counts every key use of the function,
    # but never for a key loop: there each decision is named
    for (k in want) {
      n = split(k, q, ":")
      if (q[n] == "*") {
        fk = q[1] ":" q[2]
        if (q[2] in loop) print "OFFENDER " k ": a key loop may not be allowed as a whole, name each key"
        else if (!(fk in total)) print "STALE " k " matches nothing"
        else if (total[fk] > want[k]) print "OFFENDER " firstfn[fk] ": " fk " uses keys " total[fk] " times, the allow file says " want[k]
        else if (total[fk] < want[k]) print "STALE " k " is used " total[fk] " times, the allow file says " want[k]
        covered[fk] = 1
      }
    }
    for (k in got) {
      split(k, q, ":")
      if ((q[1] ":" q[2]) in covered) continue
      if (!(k in want)) print "OFFENDER " firstline[k] ": " k " is used " got[k] " time(s) and is not allowed"
      else if (got[k] > want[k]) print "OFFENDER " firstline[k] ": " k " is used " got[k] " times, the allow file says " want[k]
      else if (got[k] < want[k]) print "STALE " k " is used " got[k] " times, the allow file says " want[k]
    }
    for (k in want) {
      n = split(k, q, ":")
      if (q[n] != "*" && !(k in got)) print "STALE " k " matches nothing"
    }
  }' "$WORK/hits" > "$WORK/report"

if [ -s "$WORK/report" ]; then
  awk '/^OFFENDER / { sub(/^OFFENDER /, ""); print }' "$WORK/report" | sort > "$WORK/offenders"
  awk '/^STALE / { sub(/^STALE /, ""); print }' "$WORK/report" | sort > "$WORK/stale"
  if [ -s "$WORK/offenders" ]; then
    echo "check-keymap: FAIL, a key used outside we_bindings.c:"
    awk '{ print "  " $0 }' "$WORK/offenders"
    echo "Bind the key in the table of we_bindings.c, or name the place in"
    echo "tests/check-keymap.allow with its count and its reason."
  fi
  if [ -s "$WORK/stale" ]; then
    echo "check-keymap: FAIL, an allow line that no longer matches (stale):"
    awk '{ print "  " $0 }' "$WORK/stale"
  fi
  exit $BAD
fi

HITS=$(wc -l < "$WORK/hits")
ALLOWED=$(wc -l < "$WORK/allow")
echo "check-keymap: ok, $HITS key uses in $ALLOWED allowed lines, none else"
exit 0
