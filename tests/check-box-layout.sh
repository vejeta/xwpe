#!/bin/sh
# check-box-layout.sh -- every box has one layout path.
#
# The layout owner (we_layout.c) turns a box's placement rule into a place,
# keeps the stack of open boxes on the screen owner's depth, lays them all
# out again when a display has taken a new size, and stamps every saved
# background so a stale one is never put back.  This script fails the build
# when code goes around it.
#
#   check-box-layout.sh [--strict] [--allow FILE] [--srcdir DIR] [--nm NM]
#                       [--list] [OBJECT...]
#
# Rules, each with a canary: the script plants an offender in a stand-in
# source (or asks nm for a symbol that must be there) and fails when the
# rule does not name it, so a scanner gone blind fails instead of passing.
#
#  1. Places are the owner's.  Outside we_layout.c no source assigns a field
#     of WpeBoxRect or builds one with a compound literal.
#  2. Backgrounds are dropped by the owner.  Outside we_layout*.c, a call
#     e_close_view(pic, 0) is counted per file.
#  3. One stack.  Outside we_paint.c and we_layout.c, a call
#     wpe_paint_modal_enter( is counted per file: a box opens through
#     wpe_box_open, which makes that call itself.
#  4. The displays call the owner.  e_relayout_windows( is called only from
#     we_layout_desk.c; the terminal, X11 and Wayland objects import
#     wpe_layout_screen_resized; e_forget_box_view_on_resize is defined by
#     no object.
#  5. Frames are the owner's.  An object that imports e_std_kst (a box's
#     frame) and is not we_layout_desk.o is counted; so are the calls of
#     e_open_view( per file outside we_layout*.c.
#
# Default mode lets the offenders named in the allow file stand, each with
# its count: the count may only fall, and a line that matches nothing is an
# error, so the file shrinks as the boxes move onto the owner.  --strict
# ignores the file and is the goal.  --list prints what was found as allow
# lines.  Rules 1 and 4 have no allow lines: they hold from the first pass.
#
# Environment (set by `make check`): NM, ONE_OWNER_OBJECTS, srcdir.
# Exit: 0 clean, 1 offenders or a blind scanner.

STRICT=0
ALLOW=
LIST=0
SRCDIR=${srcdir:-.}
NMTOOL=${NM:-nm}
OBJECTS=
HERE=$(dirname "$0")
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --strict) STRICT=1 ;;
    --allow) shift; ALLOW=$1 ;;
    --srcdir) shift; SRCDIR=$1 ;;
    --nm) shift; NMTOOL=$1 ;;
    --list) LIST=1 ;;
    *) OBJECTS="$OBJECTS $1" ;;
  esac
  shift
done
[ -n "$OBJECTS" ] || OBJECTS=$ONE_OWNER_OBJECTS
[ -n "$ALLOW" ] || ALLOW=$HERE/box-layout.allow

WORK=$(mktemp -d "${TMPDIR:-/tmp}/box-layout.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
: > "$WORK/problems"
: > "$WORK/found"
mkdir -p "$WORK/stripped"

problem() { echo "$*" >> "$WORK/problems"; }

# strip_c FILE - the file's lines with comments blanked, string literals
# kept, one output line per input line.
strip_c() {
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

SOURCES=$(cd "$SRCDIR" && ls we_*.c WeXterm.c 2>/dev/null)
for f in $SOURCES; do
  strip_c "$SRCDIR/$f" > "$WORK/stripped/$f"
done

# count_in FILE REGEX - lines of the stripped file that match the regex.
count_in() { grep -cE "$2" "$1"; }

RECT_ASSIGN='(\.|->)bx_(left|top|right|bottom)[ 	]*=[^=]|\(WpeBoxRect\)[ 	]*\{'
CLOSE_DROP='e_close_view\([^,;]*,[ 	]*0[ 	]*\)'
MODAL_ENTER='wpe_paint_modal_enter\('
RELAYOUT_CALL='e_relayout_windows\('
OPEN_VIEW='e_open_view\('

# --- the canaries: a stand-in source with one offender of each rule
printf '%s\n' 'void f(WpeBoxRect *r) { r->bx_left = 3; }' > "$WORK/canary.c"
[ "$(count_in "$WORK/canary.c" "$RECT_ASSIGN")" -eq 1 ] ||
  problem "canary: the rect scan does not see an assignment to a box place (it is blind)"
printf '%s\n' 'void f(PIC *p) { e_close_view(p, 0); }' > "$WORK/canary.c"
[ "$(count_in "$WORK/canary.c" "$CLOSE_DROP")" -eq 1 ] ||
  problem "canary: the background scan does not see e_close_view(pic, 0) (it is blind)"
printf '%s\n' 'void f(void) { wpe_paint_modal_enter("x"); }' > "$WORK/canary.c"
[ "$(count_in "$WORK/canary.c" "$MODAL_ENTER")" -eq 1 ] ||
  problem "canary: the stack scan does not see wpe_paint_modal_enter (it is blind)"
printf '%s\n' 'void f(ECNT *c) { e_relayout_windows(c, 1, 1); }' > "$WORK/canary.c"
[ "$(count_in "$WORK/canary.c" "$RELAYOUT_CALL")" -eq 1 ] ||
  problem "canary: the relayout scan does not see a call (it is blind)"

# --- rule 1: places are the owner's (no allow lines)
for f in $SOURCES; do
  case $f in we_layout.c) continue ;; esac
  n=$(count_in "$WORK/stripped/$f" "$RECT_ASSIGN")
  [ "$n" -eq 0 ] || problem "rect: $f computes a box place by hand ($n site(s)); use wpe_box_place"
done
[ -f "$SRCDIR/we_layout.c" ] ||
  problem "canary: we_layout.c not found under $SRCDIR (the scan is blind)"

# --- rule 2: backgrounds are dropped by the owner
for f in $SOURCES; do
  case $f in we_layout.c|we_layout_desk.c) continue ;; esac
  n=$(count_in "$WORK/stripped/$f" "$CLOSE_DROP")
  [ "$n" -eq 0 ] || echo "drops $f $n" >> "$WORK/found"
done

# --- rule 3: one stack
for f in $SOURCES; do
  case $f in we_paint.c|we_layout.c) continue ;; esac
  n=$(count_in "$WORK/stripped/$f" "$MODAL_ENTER")
  [ "$n" -eq 0 ] || echo "modal $f $n" >> "$WORK/found"
done

# --- rule 4, sources: the windows are laid out by the owner's hook only
for f in $SOURCES; do
  case $f in we_layout_desk.c) continue ;; esac
  n=$(grep -E "$RELAYOUT_CALL" "$WORK/stripped/$f" | grep -cvE '^[ 	]*void[ 	]+e_relayout_windows')
  [ "$n" -eq 0 ] ||
    problem "relayout: $f calls e_relayout_windows itself ($n site(s)); a display calls wpe_layout_screen_resized"
done

# --- rule 5, sources: frames
for f in $SOURCES; do
  case $f in we_layout.c|we_layout_desk.c) continue ;; esac
  n=$(grep -E "$OPEN_VIEW" "$WORK/stripped/$f" | grep -cvE '^[ 	]*PIC[ 	]*\*[ 	]*e_open_view')
  [ "$n" -eq 0 ] || echo "openview $f $n" >> "$WORK/found"
done

# --- the symbol scan (rules 4 and 5)
normalise() { awk '{ sub(/^ *[Uu] /, ""); sub(/@.*/, ""); print }'; }
defined_names() {
  "$NMTOOL" -g --defined-only "$1" 2>/dev/null | awk '
    NF >= 2 && $(NF-1) != "U" { n = $NF; sub(/@.*/, "", n); print n }'
}
if command -v "$NMTOOL" >/dev/null 2>&1 && [ "$NMTOOL" != ":" ]; then
  seen_displays=0
  for obj in $OBJECTS; do
    [ -f "$obj" ] || continue
    base=$(basename "$obj")
    "$NMTOOL" -u "$obj" 2>/dev/null | normalise > "$WORK/imports"
    defined_names "$obj" > "$WORK/defined"
    case $base in
      we_term.o|we_xterm.o|we_wayland.o)
        seen_displays=$((seen_displays + 1))
        grep -qx wpe_layout_screen_resized "$WORK/imports" ||
          problem "canary: $base does not import wpe_layout_screen_resized (a display lays the desk out some other way)" ;;
    esac
    grep -qx e_forget_box_view_on_resize "$WORK/defined" &&
      problem "deleted symbol: $base defines e_forget_box_view_on_resize (the walk forgets a stale box view)"
    if grep -qx e_std_kst "$WORK/imports"; then
      case $base in
        we_layout_desk.o) ;;
        *) echo "stdkst $base 1" >> "$WORK/found" ;;
      esac
    fi
    case $base in
      we_layout.o)
        grep -qx wpe_paint_modal_enter "$WORK/imports" ||
          problem "canary: we_layout.o does not import wpe_paint_modal_enter (the stack is not the screen owner's)" ;;
    esac
  done
  [ "$seen_displays" -eq 0 ] || [ "$seen_displays" -eq 3 ] ||
    problem "canary: $seen_displays of the 3 display objects were seen (the symbol scan is blind)"
fi

sort "$WORK/found" -o "$WORK/found"

if [ $LIST -eq 1 ]; then
  awk '{ print $1 " " $2 " " $3 " the pass that moves it" }' "$WORK/found"
  awk '{ print "# " $0 }' "$WORK/problems"
  exit 0
fi

# --- compare with the allow file
: > "$WORK/allowed"
if true; then
  if [ -f "$ALLOW" ]; then
    awk '
      /^[ \t]*(#|$)/ { next }
      {
        if (NF < 4 || $3 !~ /^[0-9]+$/) { print "bad line " NR " in the allow file: " $0 > "/dev/stderr"; bad = 1; next }
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
fi
# In strict mode only permanent exceptions stand (the lines whose pass is "keep").
if [ $STRICT -eq 1 ]; then
  awk '$4 == "keep"' "$WORK/allowed" > "$WORK/allowed.keep"
  mv "$WORK/allowed.keep" "$WORK/allowed"
fi

awk '
  FILENAME == ARGV[1] { al[$1 " " $2] = $3; pass[$1 " " $2] = $4; next }
  {
    key = $1 " " $2; seen[key] = 1
    if (!(key in al)) {
      print "offender: " $2 " (" $1 ", " $3 " site(s)) goes around the layout owner"; bad = 1
    } else if ($3 + 0 > al[key] + 0) {
      print "more than allowed: " $2 " (" $1 ") has " $3 " site(s), the allow file says " al[key] " (" pass[key] ")"; bad = 1
    } else if ($3 + 0 < al[key] + 0) {
      print "fewer than the allow file says: " $2 " (" $1 ") has " $3 " site(s), lower the line from " al[key] " (" pass[key] ")"; bad = 1
    }
  }
  END {
    for (k in al) if (!(k in seen)) { print "stale entry, delete it: " k " (" pass[k] ")"; bad = 1 }
    exit bad
  }' "$WORK/allowed" "$WORK/found" > "$WORK/report"
status=$?

if [ -s "$WORK/problems" ] || [ $status -ne 0 ]; then
  [ $STRICT -eq 1 ] && echo "check-box-layout --strict: FAIL"
  [ $STRICT -eq 0 ] && echo "check-box-layout: FAIL"
  cat "$WORK/problems" "$WORK/report" 2>/dev/null
  exit $BAD
fi
echo "check-box-layout: ok ($(wc -l < "$WORK/found" | tr -d ' ') allowed offender line(s))"
exit 0
