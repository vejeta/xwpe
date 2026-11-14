#!/bin/sh
# check-list-entry.sh -- the selected entry of a list window has one owner.
#
# we_listsel.[ch] answer whether the highlighted row of a list names an entry
# and hand the entry out; the blank row that ends a project list is never one.
# A site that indexes a list's names by the highlighted row itself, with
# name[...nf...] or *(...name + ...nf...), reads a row that may be the blank
# one, or that an empty list does not have.  That is how Enter on an empty
# Grep result and on a project list with no files killed the editor.
#
#   check-list-entry.sh [--allow FILE] [--srcdir DIR] [--list]
#
# Rule, applied to every top level source except we_listsel.c, with comments
# blanked and string literals kept, counted per function: an index into a
# list's names that contains the highlighted row (a word "nf") or a local
# that was assigned from an expression holding "nf", in either spelling,
# name[index] or *(pointer name + index).  A builder that appends at the end
# (name[df->anz - 1]) selects nothing and is not counted.
#
# tests/list-entry.allow lists each function that still indexes by the
# highlighted row, with its count and the reason it is not a list window's
# selection: the file manager's panels keep their own in-range invariant
# (activation refuses an empty panel).  The count may only fall: more fails,
# fewer fails until the line is lowered, a line that matches nothing fails as
# stale.  --list prints what was found as allow lines.
#
# Canaries: an offender planted in each spelling, and through a local, in a
# copy of a scanned file; a rule that does not name it fails the script as
# blind.  A builder's append planted the same way must not be named.
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
    *) echo "check-list-entry: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done
[ -n "$ALLOW" ] || ALLOW=$HERE/list-entry.allow

OWNER_FILES="we_listsel.c we_listsel.h"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/list-entry.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"

problem() { echo "$*" >> "$WORK/problems"; }

# count_by_function FILE PATH - "file|function|count" for each function that
# indexes a list's names by the highlighted row, comments blanked, string and
# character literals kept.  A function starts at a line in column 0 that names
# an identifier followed by "(" and does not end with ";".  "#fn|file|function"
# lines list the functions seen.
count_by_function() {
  awk -v file="$1" '
    function hasword(s, w) {
      return (s ~ ("(^|[^A-Za-z0-9_])" w "([^A-Za-z0-9_]|$)"))
    }
    function selects(idx,    k) {
      if (hasword(idx, "nf")) return 1
      for (k in loc) if (hasword(idx, k)) return 1
      return 0
    }
    BEGIN { incom = 0; fn = "(file scope)" }
    {
      line = $0; out = ""; n = length(line); i = 1; instr = 0; inchr = 0
      while (i <= n) {
        c = substr(line, i, 1); d = substr(line, i, 2)
        if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
        if (instr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
          if (c == "\"") instr = 0
          i++; continue
        }
        if (inchr) {
          out = out c
          if (c == "\\") { out = out substr(line, i + 1, 1); i += 2; continue }
          if (c == "\047") inchr = 0
          i++; continue
        }
        if (d == "/*") { incom = 1; i += 2; continue }
        if (d == "//") break
        if (c == "\"") instr = 1
        if (c == "\047") inchr = 1
        out = out c; i++
      }
      if (out ~ /^[A-Za-z_]/ && out ~ /[(]/ && out !~ /;[ \t]*$/ && out !~ /^(typedef|struct[^(]*[{])/) {
        name = out; sub(/[ \t]*[(].*/, "", name); sub(/.*[^A-Za-z0-9_]/, "", name)
        if (name != "") { fn = name; seen[fn] = 1; delete loc }
      }
      s = out
      while (match(s, /[A-Za-z_][A-Za-z0-9_]*[ \t]*=[^=;]*/)) {
        a = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
        if (hasword(a, "nf")) { v = a; sub(/[ \t]*=.*/, "", v); if (v != "nf") loc[v] = 1 }
      }
      # A loop that walks down (or up) to the highlighted row ends on it:
      # "for (i = anz - 1; i > fw->nf; i--) ... name[i] = name[i - 1];
      # name[i] = new" inserts at the selection by hand.
      s = out
      while (match(s, /for[ \t]*[(][^;]*;[^;]*;/)) {
        a = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
        sub(/^for[ \t]*[(][^;]*;/, "", a)
        if (hasword(a, "nf") && match(a, /^[ \t]*[A-Za-z_][A-Za-z0-9_]*/)) {
          v = substr(a, RSTART, RLENGTH); gsub(/[ \t]/, "", v)
          if (v != "nf") loc[v] = 1
        }
      }
      s = out
      while (match(s, /name\[[^]]*\]/)) {
        a = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
        if (selects(a)) cnt[fn]++
      }
      s = out
      while (match(s, /\*[ \t]*[(][^()]*name[ \t]*[+][^()]*[)]/)) {
        a = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
        sub(/.*name[ \t]*[+]/, "", a)
        if (selects(a)) cnt[fn]++
      }
    }
    END {
      for (f in cnt) print file "|" f "|" cnt[f]
      for (f in seen) print "#fn|" file "|" f
    }' "$2"
}

# scan_tree DIR OUT - what the scan finds under DIR, "file|function|count".
scan_tree() {
  dir=$1; out=$2
  : > "$out.raw"
  for p in "$dir"/*.c; do
    [ -f "$p" ] || continue
    f=${p##*/}
    case " $OWNER_FILES config.h " in *" $f "*) continue ;; esac
    count_by_function "$f" "$p" >> "$out.raw"
  done
  grep -v '^#fn|' "$out.raw" | sort > "$out"
  grep '^#fn|' "$out.raw" > "$out.fn"
  rm -f "$out.raw"
}

# canary NAME CODE - plant CODE in a copy of a scanned file alone and fail
# unless the scan reports "planted" in it.
canary() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$2" > "$c/we_fl_unix.c"
  scan_tree "$c" "$c/out"
  if ! grep -qF "we_fl_unix.c|planted|" "$c/out"; then
    echo "canary $1: a planted offender was not found: $2"
    return 1
  fi
  return 0
}

# allowed_canary NAME CODE - a way that selects nothing: the scan must NOT
# report it, or the rule is too wide.
allowed_canary() {
  c=$(mktemp -d "$WORK/canary.XXXXXX")
  printf '%s\n' "$2" > "$c/we_fl_unix.c"
  scan_tree "$c" "$c/out"
  if grep -qF "we_fl_unix.c|planted|" "$c/out"; then
    echo "allowed canary $1: a way that selects nothing was reported: $2"
    return 1
  fi
  return 0
}

blind=0
canary bracket 'void planted(FLWND *fw) { e_edit(fw->f, fw->df->name[fw->nf]); }' || blind=1
canary pointer 'void planted(FLBFFR *b) { WpeRemove(*(b->df->name + b->fw->nf), 0); }' || blind=1
canary local 'void planted(FLBFFR *b) { int t; t = b->dw->nf - b->cd->anz; WpeRemove(b->dd->name[t], 0); }' || blind=1
canary local-pointer 'void planted(FLBFFR *b) { int t = b->dw->nf - 2; WpeRemove(*(b->dd->name + t), 0); }' || blind=1
canary loop-insert 'void planted(FLWND *fw, char *s) { int i; for (i = fw->df->anz - 1; i > fw->nf; i--) fw->df->name[i] = fw->df->name[i - 1]; fw->df->name[i] = s; }' || blind=1
allowed_canary append'void planted(struct dirfile *df) { df->name[df->anz - 1] = 0; }' || blind=1
allowed_canary loop-index 'void planted(struct dirfile *df, int i) { for (i = 0; i < df->anz; i++) free(df->name[i]); }' || blind=1
allowed_canary comment 'void planted(FLWND *fw) { /* name[fw->nf] and *(fw->df->name + fw->nf) */ }' || blind=1

scan_tree "$SRCDIR" "$WORK/found"
grep -q '^#fn|we_fl_unix.c|e_data_eingabe$' "$WORK/found.fn" ||
  problem "canary: e_data_eingabe not seen as a function of we_fl_unix.c (the function scan is blind)"

if [ $LIST -eq 1 ]; then
  awk -F'|' '{ print $1 "|" $2 "|" $3 "|keep|reason" }' "$WORK/found"
  awk '{ print "# " $0 }' "$WORK/problems"
  exit 0
fi

: > "$WORK/allowed"
if [ -r "$ALLOW" ]; then
  awk -F'|' '
    /^[ \t]*(#|$)/ { next }
    {
      if (NF < 5 || $3 !~ /^[0-9]+$/ || $4 != "keep") { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
      key = $1 " " $2
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
  FILENAME == ARGV[1] { al[$1 "|" $2] = $3; next }
  {
    key = $1 "|" $2; got[key] = 1
    if (!(key in al)) {
      print "offender: " $1 ": " $2 " indexes a list by the highlighted row (" $3 " place(s)): ask we_listsel (wpe_listsel_name, wpe_listsel_tree_subdir), or list it with its reason"; bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $1 ": " $2 " has " $3 ", the allow file says " al[key]; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $1 ": " $2 " has " $3 ", lower the line from " al[key]; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in got)) { print "stale entry, delete it: " k; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ "$blind" -ne 0 ] || [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  echo "check-list-entry: FAIL"
  cat "$WORK/problems" "$WORK/report"
  exit $BAD
fi
echo "check-list-entry: ok ($(awk -F'|' '{ n += $3 } END { print n + 0 }' "$WORK/found") place(s) in $(wc -l < "$WORK/found" | tr -d ' ') listed function(s))"
exit 0
