#!/bin/sh
# check-keymap-docs.sh -- the key reference is the key table's own output.
#
# docs/chapters/keys-generated.texi is what tools/keymap-texi prints, and
# docs/chapters/keys-cua-1.6.txt is its dump of the CUA style, the list the
# Modern style's differences are written against.  Both are kept in the tree.
# This script runs the tool and fails when either file differs, so a binding
# cannot change without the reference changing in the same diff.  To update:
#
#   tools/keymap-texi > docs/chapters/keys-generated.texi
#   tools/keymap-texi --dump cua > docs/chapters/keys-cua-1.6.txt
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

"$TOOL" --dump cua > "$WORK/keys-cua.txt" || exit $BAD
if ! diff -u "$SRCDIR/docs/chapters/keys-cua-1.6.txt" "$WORK/keys-cua.txt" > "$WORK/cua.diff"; then
  echo "check-keymap-docs: FAIL, docs/chapters/keys-cua-1.6.txt is not the CUA column of the table:"
  cat "$WORK/cua.diff"
  status=$BAD
fi

[ $status = 0 ] && echo "check-keymap-docs: ok"
exit $status
