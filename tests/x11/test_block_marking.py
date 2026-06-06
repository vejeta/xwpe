"""X11 GUI tests: WordStar vs modern block-marking modes (xwpe).

Begin Mark (^K B) behaves differently per the Options > Editor "WordStar
block" checkbox (ED_BLOCK_WORDSTAR):

- clear (modern, default): ^K B starts a FRESH selection -- it resets BOTH
  the begin and end markers to the cursor, so re-marking never leaves the
  previous block highlighted.
- set   (WordStar): ^K B moves only the begin marker and leaves the end where
  it was -- the two markers are independent (classic Borland behaviour).

Regression guarded: the keyboard ^K B set mark_begin inline in e_ctrl_k,
bypassing e_blck_begin, so the mode flag had NO effect from the keyboard and
both modes behaved identically.  It now routes through e_blck_begin.

Method: mark a block, then Begin-Mark again ABOVE it, and compare the screen
to the unmarked baseline.  Modern clears the block (screen ~= baseline);
WordStar keeps the end, so a block from the new begin to the old end stays
highlighted (screen clearly differs).
"""
import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent
from PIL import ImageChops


def changed_pixels(img_a, img_b, thresh=40):
    diff = ImageChops.difference(img_a, img_b).convert("L")
    return diff.point(lambda p: 255 if p > thresh else 0).histogram()[255]


def mark_then_rebegin_above(xwpe):
    """Mark a block on lines 2..4, then Begin-Mark again on line 1.
    Returns (baseline_unmarked, after_rebegin)."""
    baseline = xwpe.screenshot()
    xwpe.key("Down")                       # cursor -> line 2
    xwpe.key("ctrl+k"); xwpe.key("b")      # Begin mark
    xwpe.key("Down"); xwpe.key("Down")     # cursor -> line 4
    xwpe.key("ctrl+k"); xwpe.key("k")      # End mark  (lines 2..3 highlighted)
    xwpe.key("Up"); xwpe.key("Up"); xwpe.key("Up")   # cursor -> line 1 (above end)
    xwpe.key("ctrl+k"); xwpe.key("b")      # re-Begin mark
    return baseline, xwpe.screenshot()


def set_wordstar_mode(xwpe):
    """Options > Editor: tick 'WordStar block' (Alt-K) and confirm (Alt-O OK)."""
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)
    xwpe.key("alt+k", delay=0.5)           # checkbox starts clear -> tick it
    xwpe.key("alt+o", delay=1.0)           # OK button hotkey


def test_modern_rebegin_clears_block(xwpe):
    """Modern (default): re-Begin resets begin AND end -> no stale block."""
    baseline, after = mark_then_rebegin_above(xwpe)
    changed = changed_pixels(baseline, after)
    assert changed < 600, (
        "modern ^K B should clear the previous block, but %d px still differ "
        "from the unmarked screen (stale highlight)" % changed)


def test_wordstar_rebegin_keeps_end(xwpe):
    """WordStar: re-Begin keeps the end marker -> a block stays highlighted,
    visibly different from the modern result."""
    set_wordstar_mode(xwpe)
    baseline, after = mark_then_rebegin_above(xwpe)
    changed = changed_pixels(baseline, after)
    assert changed > 1500, (
        "WordStar ^K B should keep the end marker and leave a block "
        "highlighted, but only %d px differ from the unmarked screen" % changed)


# --- shortcut path (#159): the WordStar ^K block-operation chords ---------
# Twin of tests/test_block.py's ^K chord tests in the X11 (we_xterm.c) input
# path.  Mark Whole (^K X) then operate, and assert on the SAVED file (ground
# truth) -- the chord decode is separate from the Alt-B menu route.

def test_chord_mark_whole_delete_empties_buffer(xwpe):
    """^K X (Mark Whole) then ^K Y (Delete) empties the buffer."""
    xwpe.key("ctrl+k"); xwpe.key("x")      # Mark Whole
    xwpe.key("ctrl+k"); xwpe.key("y")      # Delete
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during ^K Y Delete"
    assert xwpe.saved_text().strip() == "", \
        "^K X + ^K Y should empty the buffer, got %r" % xwpe.saved_text()


def test_chord_indent_adds_leading_space(xwpe):
    """^K X (Mark Whole) then ^K I indents every block line."""
    xwpe.key("ctrl+k"); xwpe.key("x")      # Mark Whole
    xwpe.key("ctrl+k"); xwpe.key("i")      # Move to RIght (indent)
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during ^K I indent"
    lines = [ln for ln in xwpe.saved_text().splitlines() if ln.strip()]
    assert lines and all(ln.startswith(" ") for ln in lines), \
        "^K I should indent every block line, got %r" % xwpe.saved_text()
