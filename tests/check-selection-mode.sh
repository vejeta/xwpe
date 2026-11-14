#!/bin/sh
# check-selection-mode.sh -- the selection block mode, the pending auto-indent
# and the file's final line break each have one owner.
#
# Typing over a selection replaces it as one Undo step that is made whole or
# not at all (e_selection_run, through e_edit_action_run).  The indent that
# Return put on a line is a mark of the buffer owner (E_BUFFER_MARK_AUTO_INDENT)
# so it moves with the text and goes when the row changes.  Save leaves the
# final line break out only where write_keeps_file_ending says so.  A site
# that does these by itself leaves half a replacement, an indent mark that
# points at the wrong row, or a file that loses its last line break.
#
#   check-selection-mode.sh [--srcdir DIR]
#
# Seven rules, each with a canary: the script plants an offender in a copy of
# the sources and fails when the rule does not name it.
#
#  1. The pending auto-indent mark is written only by the buffer owner: no
#     source but we_buffer.c assigns to the window's ai position.
#  2. A selection is replaced in two places, one per kind of replacement:
#     E_BUFFER_FAULT_REPLACE is named once in we_edit.c (e_selection_run,
#     what is typed) and once in we_block.c (e_paste_over_selection, what is
#     pasted), and nowhere else but the owner's table in we_buffer.[ch].
#  3. The selection bit has its readers: ED_BLOCK_SELECTION appears in
#     options.h, we_edit.c and we_opt.c and nowhere else.
#  4. The final line break has one writer: write_rows in we_write.c gives
#     e_eol_write write_keeps_file_ending, and every other caller outside
#     the line-ending owner passes 0.
#  5. The block Ctrl-K B leaves open has one owner: only we_buffer.c sets or
#     clears the window's block_open (e_buffer_block_open and every write of
#     a block end), apart from the zero a new window starts with in we_edit.c.
#     A site that sets it by itself leaves a block that a plain move no
#     longer ends.
#  6. The WordStar bit has one reader: ED_BLOCK_WORDSTAR appears in options.h
#     and we_opt.c (e_opt_block_wordstar and the dialog) and nowhere else, so
#     a WordStar bit left in an options file next to the selection mode
#     changes nothing.
#  7. Where a Redo leaves the cursor is kept by the buffer owner: only
#     we_buffer.c names E_UNDO_REDO_VIEW and redo_at, apart from their
#     declarations in we_buffer.h and edit.h.
#
# Exit: 0 clean, 1 offenders or a blind scanner.

SRCDIR=${srcdir:-.}
BAD=1

while [ $# -gt 0 ]; do
  case $1 in
    --srcdir) shift; SRCDIR=$1 ;;
    *) echo "check-selection-mode: unknown argument $1" >&2; exit $BAD ;;
  esac
  shift
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/selection-mode.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

# scan DIR - "rule|file:line|text" for every offence in the sources of DIR.
# One awk pass over the files: comments are blanked, then each rule is a
# pattern on the line.
scan() {
  dir=$1
  for f in "$dir"/*.c "$dir"/*.h; do
    [ -f "$f" ] || continue
    awk -v base="$(basename "$f")" '
      BEGIN { incom = 0 }
      {
        line = $0; out = ""; n = length(line); i = 1
        while (i <= n) {
          d = substr(line, i, 2)
          if (incom) { if (d == "*/") { incom = 0; i += 2 } else i++; continue }
          if (d == "/*") { incom = 1; i += 2; continue }
          out = out substr(line, i, 1); i++
        }
        if (base != "we_buffer.c" &&
            out ~ /(->|\.)ai(\.[xy])?[ \t]*(=[^=]|\+\+|--|\+=|-=)/)
          print "1|" base ":" NR "|" out
        if (base != "we_edit.c" && base != "we_block.c" && base != "we_buffer.c" &&
            base != "we_buffer.h" && out ~ /E_BUFFER_FAULT_REPLACE/)
          print "2|" base ":" NR "|" out
        if ((base == "we_edit.c" || base == "we_block.c") && out ~ /E_BUFFER_FAULT_REPLACE/)
          named++
        if (base != "options.h" && base != "we_edit.c" && base != "we_opt.c" &&
            out ~ /ED_BLOCK_SELECTION/)
          print "3|" base ":" NR "|" out
        if (base != "we_lineend.c" && base != "we_lineend.h" && base != "we_write.c" &&
            out ~ /e_eol_write[ \t]*\(/ && out !~ /,[ \t]*0[ \t]*\)/)
          print "4|" base ":" NR "|" out
        if (base != "we_buffer.c" &&
            out ~ /block_open[ \t]*(=[^=]|\+\+|--|\+=|-=|\|=|&=)/ &&
            !(base == "we_edit.c" && out ~ /->block_open[ \t]*=[ \t]*0[ \t]*;/))
          print "5|" base ":" NR "|" out
        if (base != "options.h" && base != "we_opt.c" && out ~ /ED_BLOCK_WORDSTAR/)
          print "6|" base ":" NR "|" out
        if (base != "we_buffer.c" && base != "we_buffer.h" && base != "edit.h" &&
            out ~ /E_UNDO_REDO_VIEW|redo_at/)
          print "7|" base ":" NR "|" out
      }
      END {
        if ((base == "we_edit.c" || base == "we_block.c") && named != 1)
          print "2|" base "|E_BUFFER_FAULT_REPLACE named " named + 0 " times, want once"
      }' "$f"
  done
  if ! grep -q 'e_eol_write(fp, b, ya, xa, ye, xe, write_keeps_file_ending(' "$dir/we_write.c"; then
    echo "4|we_write.c|write_rows does not give e_eol_write write_keeps_file_ending"
  fi
}

real=$(scan "$SRCDIR")
if [ -n "$real" ]; then
  echo "check-selection-mode: offenders"
  printf '%s\n' "$real"
  fail=1
fi

# canaries: a copy of the sources with one offender per rule must be named.
mkdir "$WORK/c" || exit $BAD
cp "$SRCDIR"/*.c "$SRCDIR"/*.h "$WORK/c/"
echo 'static void canary_one(SCHIRM *s) { s->ai.y = 3; }' >> "$WORK/c/we_wind.c"
echo 'static int canary_two(BUFFER *b) { return e_edit_action_run(b, 0, 0, E_BUFFER_FAULT_REPLACE, "x", 0, 0); }' >> "$WORK/c/we_wind.c"
echo 'static int canary_three(ECNT *e) { return e->edopt & ED_BLOCK_SELECTION; }' >> "$WORK/c/we_menue.c"
echo 'static void canary_four(FILE *fp, BUFFER *b) { e_eol_write(fp, b, 0, 0, 1, 1, 1); }' >> "$WORK/c/we_progn.c"
echo 'static void canary_five(SCHIRM *s) { s->block_open = 1; }' >> "$WORK/c/we_edit.c"
echo 'static int canary_six(ECNT *e) { return e->edopt & ED_BLOCK_WORDSTAR; }' >> "$WORK/c/we_block.c"
echo 'static void canary_seven(Undo *u) { u->redo_at.x = 0; }' >> "$WORK/c/we_block.c"
seen=$(scan "$WORK/c")
for r in 1 2 3 4 5 6 7; do
  if ! printf '%s\n' "$seen" | grep -q "^$r|"; then
    echo "check-selection-mode: blind scanner: the canary of rule $r was not named"
    fail=1
  fi
done

[ -n "$fail" ] && exit $BAD
echo "check-selection-mode: clean"
exit 0
