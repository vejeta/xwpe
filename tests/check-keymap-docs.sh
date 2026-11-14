#!/bin/sh
# check-keymap-docs.sh -- the key reference is the key table's own output.
#
# docs/chapters/keys-generated.texi is what tools/keymap-texi prints;
# docs/chapters/keys-modern-changes.texi is the list of what the Modern style
# did to the keys of the 1.6 CUA style, computed from the frozen dump
# docs/chapters/keys-cua-1.6.txt (never regenerated: it is the record of what
# 1.6 did); docs/chapters/keys-borland-1.6.txt is the frozen Borland dump, which
# the Borland style must still equal.  This script runs the tool and fails when
# a file differs, so a binding cannot change without the reference changing in
# the same diff.  To update (the tool writes the file itself):
#
#   tools/keymap-texi -o docs/chapters/keys-generated.texi
#   tools/keymap-texi -o docs/chapters/keys-modern-changes.texi \
#       --changes docs/chapters/keys-cua-1.6.txt
#   tools/keymap-texi -o docs/chapters/menus-generated.texi --menus
#
# Environment (set by `make check`): KEYMAP_TEXI, srcdir.
# Exit: 0 equal, 1 different, 77 the tool was not built.

SRCDIR=${srcdir:-.}
TOOL=${KEYMAP_TEXI:-./tools/keymap-texi}
BAD=1
SKIP=77

if [ ! -x "$TOOL" ]; then
  echo "check-keymap-docs: SKIP, $TOOL is not built"
  exit $SKIP
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/keymap-docs.XXXXXX") || exit $BAD
trap 'rm -rf "$WORK"' EXIT HUP INT TERM

status=0

"$TOOL" > "$WORK/keys-generated.texi" || exit $BAD
if ! diff -u "$SRCDIR/docs/chapters/keys-generated.texi" "$WORK/keys-generated.texi" > "$WORK/texi.diff"; then
  echo "check-keymap-docs: FAIL, docs/chapters/keys-generated.texi is not the table's output:"
  cat "$WORK/texi.diff"
  status=$BAD
fi

# The Borland style did not change when the second style became Modern: its
# dump is the frozen 1.6 dump (docs/chapters/keys-borland-1.6.txt) plus the
# control-letter cursor keys, which were code in the editor and are table rows
# now.  Any other line that appears, goes or changes fails here.
"$TOOL" --dump borland > "$WORK/borland-all.txt" || exit $BAD
grep -v '^Cursor keys | ' "$WORK/borland-all.txt" > "$WORK/borland-old.txt"
if ! diff -u "$SRCDIR/docs/chapters/keys-borland-1.6.txt" "$WORK/borland-old.txt" > "$WORK/borland.diff"; then
  echo "check-keymap-docs: FAIL, the Borland column differs from the frozen 1.6 dump:"
  cat "$WORK/borland.diff"
  status=$BAD
fi
# The control-letter rows themselves are what the editor did in 1.6 with
# these letters, one by one, so they are held to that list too.
grep '^Cursor keys | ' "$WORK/borland-all.txt" > "$WORK/borland-cursor.txt"
printf '%s\n' \
  'Cursor keys | 16 | Ctrl-P | Cursor up' \
  'Cursor keys | 14 | Ctrl-N | Cursor down' \
  'Cursor keys | 2 | Ctrl-B | Cursor left' \
  'Cursor keys | 6 | Ctrl-F | Cursor right' \
  'Cursor keys | 1 | Ctrl-A | Start of line' \
  'Cursor keys | 5 | Ctrl-E | End of line' \
  'Cursor keys | 20 | Ctrl-T | Delete word right' \
  'Cursor keys | 26 | Ctrl-Z | Delete to end of line' \
  'Cursor keys | 25 | Ctrl-Y | Delete line' \
  'Cursor keys | 4 | Ctrl-D | Delete character' > "$WORK/borland-cursor-1.6.txt"
if ! diff -u "$WORK/borland-cursor-1.6.txt" "$WORK/borland-cursor.txt" > "$WORK/cursor.diff"; then
  echo "check-keymap-docs: FAIL, the Borland control-letter rows differ from what 1.6 did:"
  cat "$WORK/cursor.diff"
  status=$BAD
fi

# The lists of what Modern did to the 1.6 CUA keys are computed from the frozen
# CUA dump (docs/chapters/keys-cua-1.6.txt, never regenerated) and the table.
"$TOOL" --changes "$SRCDIR/docs/chapters/keys-cua-1.6.txt" > "$WORK/keys-modern-changes.texi" || exit $BAD
if ! diff -u "$SRCDIR/docs/chapters/keys-modern-changes.texi" "$WORK/keys-modern-changes.texi" > "$WORK/changes.diff"; then
  echo "check-keymap-docs: FAIL, docs/chapters/keys-modern-changes.texi is not the table's output:"
  cat "$WORK/changes.diff"
  status=$BAD
fi

# The menu item tables are the menu table's own output
# (tools/keymap-texi -o docs/chapters/menus-generated.texi --menus).
"$TOOL" --menus > "$WORK/menus-generated.texi" || exit $BAD
if ! diff -u "$SRCDIR/docs/chapters/menus-generated.texi" "$WORK/menus-generated.texi" > "$WORK/menus.diff"; then
  echo "check-keymap-docs: FAIL, docs/chapters/menus-generated.texi is not the menu table's output:"
  cat "$WORK/menus.diff"
  status=$BAD
fi

[ $status = 0 ] && echo "check-keymap-docs: ok"
exit $status
