"""X11 GUI regression tests for the Options -> Editor dialog (xwpe).

Regression guarded here
-----------------------
In xwpe (X11/Xft) with NEWSTYLE the dialog redrew checkbox / radio marks
with e_make_xrect_abs (a 3D bevel whose checked vs unchecked states differ
only by a near-invisible highlight bit).  The toggle value DID flip, but the
user saw no change -- "I can't tick the box with Space / mouse / Alt-K".
The fix draws the Borland-style 'X' / '*' text glyph in every mode.

These tests open the dialog under a real headless X server and assert that
toggling a checkbox produces a VISIBLE pixel change (the glyph appears) and
that toggling back removes it.  Before the fix the visible change was ~0.
"""
import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent
from PIL import ImageChops


def changed_pixels(img_a, img_b, thresh=40):
    """Number of pixels that differ by more than `thresh` (0..255 grey)."""
    diff = ImageChops.difference(img_a, img_b).convert("L")
    binar = diff.point(lambda p: 255 if p > thresh else 0)
    return binar.histogram()[255]


def open_editor_options(xwpe):
    """Options menu (Alt-O) -> Editor (e)."""
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)


def test_editor_options_opens(xwpe):
    """Opening the dialog must visibly change the screen."""
    before = xwpe.screenshot()
    open_editor_options(xwpe)
    after = xwpe.screenshot()
    assert changed_pixels(before, after) > 2000, "Editor-Options dialog did not appear"


def test_checkbox_toggle_renders_mark(xwpe):
    """Alt-K toggles the 'WordStar block' checkbox; the [X] glyph must
    appear, then disappear on a second Alt-K.  This is the direct
    regression for the invisible-checkbox bug."""
    open_editor_options(xwpe)

    unchecked = xwpe.screenshot()
    xwpe.key("alt+k", delay=0.8)          # tick it
    checked = xwpe.screenshot()
    xwpe.key("alt+k", delay=0.8)          # untick it
    unchecked2 = xwpe.screenshot()

    appeared = changed_pixels(unchecked, checked)
    restored = changed_pixels(unchecked, unchecked2)

    # The 'X' glyph is a small but unambiguous cluster of pixels.
    assert appeared > 15, (
        "toggling the checkbox produced no visible mark (%d px) -- "
        "the X11 redraw regressed to the invisible bevel" % appeared)
    # Toggling back must return close to the original frame.
    assert restored < appeared / 2, (
        "checkbox did not visually clear on a second toggle "
        "(appeared=%d restored=%d)" % (appeared, restored))
