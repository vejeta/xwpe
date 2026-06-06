"""Mouse scrolling for xwpe (X11/Xft): wheel + scrollbar-thumb drag.

The fluid scrollbar-thumb drag (e_scroll_drag_v/h in we_mouse.c) and the mouse
wheel (button 4/5 -> WPE_SCROLL_UP/DOWN) were only verified by hand.  These
drive the REAL X11 button/motion events via the conftest drag()/wheel() helpers
(ButtonPress -> MotionNotify* -> ButtonRelease for the drag).

A dragged vertical thumb also pulls the cursor into the new viewport
(e_scroll_drag_end_cursor), so a typed marker lands far down the file -- a
ground-truth check stronger than a screenshot diff.
"""
import os
import subprocess
import time

from conftest import DISPLAY, changed_pixels


def _xdo(*a):
    subprocess.run(["xdotool", *a], env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _seed_lines(xwpe, n=50):
    """Type n short numbered lines (L00xxxx..) so the editor gets a vertical
    scrollbar; lines stay short so a scroll changes a clear band of pixels."""
    for i in range(n):
        _xdo("type", "--window", xwpe.win, "L%02dxxxx" % i)
        _xdo("key", "--window", xwpe.win, "Return")
    time.sleep(0.4)


def _to_top(xwpe):
    for _ in range(70):
        xwpe.key("Up", delay=0.0)
    time.sleep(0.3)


def test_wheel_scrolls_the_view(xwpe):
    """Mouse wheel down scrolls the view; wheel up scrolls back toward the top."""
    _seed_lines(xwpe)
    _to_top(xwpe)
    top = xwpe.screenshot()
    xwpe.wheel("down", count=8, px=400, py=250)
    scrolled = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on wheel-down"
    down_change = changed_pixels(top, scrolled)
    assert down_change > 2000, \
        "mouse wheel down should scroll the view (changed only %d px)" % down_change
    xwpe.wheel("up", count=8, px=400, py=250)
    back = xwpe.screenshot()
    assert changed_pixels(top, back) < down_change, \
        "mouse wheel up should scroll back toward the top"


def test_vscrollbar_thumb_drag_scrolls(xwpe):
    """Dragging the vertical scrollbar thumb to the bottom moves the view and
    the cursor far down -- a typed marker lands near the end of the file."""
    _seed_lines(xwpe)
    _to_top(xwpe)
    xwpe.drag(1019, 70, 1019, 480)         # vertical thumb: top -> bottom
    xwpe.type("ZMARK")
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during scrollbar-thumb drag"
    lines = xwpe.saved_text().splitlines()
    idx = next((k for k, ln in enumerate(lines) if "ZMARK" in ln), -1)
    assert idx > 15, \
        "dragging the thumb to the bottom should move the view+cursor far down; " \
        "ZMARK landed at line %d of %d" % (idx, len(lines))
    assert any("L00xxxx" in ln for ln in lines), "early content should be intact"


def test_hscrollbar_thumb_drag_scrolls(xwpe):
    """Dragging the horizontal scrollbar thumb right moves the view and cursor
    far right -- a typed marker lands at a high column.  (The line is all 'H',
    so it looks identical scrolled; the cursor column is the ground truth.)"""
    _xdo("type", "--window", xwpe.win, "H" * 300)
    time.sleep(0.4)
    for _ in range(400):
        xwpe.key("Left", delay=0.0)        # cursor -> column 0 (thumb at left)
    time.sleep(0.3)
    xwpe.drag(220, 516, 850, 516)          # horizontal thumb: left -> right
    xwpe.type("HMARK")
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during horizontal scrollbar drag"
    marked = [ln for ln in xwpe.saved_text().splitlines() if "HMARK" in ln]
    assert marked, "HMARK not found in the saved file"
    col = marked[0].index("HMARK")
    assert col > 30, \
        "dragging the horizontal thumb right should move the view+cursor far " \
        "right; HMARK landed at column %d" % col
