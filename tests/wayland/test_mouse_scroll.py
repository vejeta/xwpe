"""Mouse scrollbar-thumb drag for xwpe under the NATIVE Wayland backend.

Mirror of tests/x11/test_mouse_scroll.py.  The scrollbar-thumb drag
(e_scroll_drag_v/h in we_mouse.c) used the X11 pointer grab (XGrabPointer) and
the X11 event loop directly; under the native Wayland backend there is no X
display, so the drag crashed (SIGSEGV).  These tests drive the real Wayland
pointer path (wl_pointer button PRESS -> motion* -> RELEASE) via the conftest
drag() helper and assert on disk (saved_text) that the view+cursor moved -- and,
crucially, that xwpe did NOT die during the drag.
"""
import os
import subprocess
import time

from conftest import DISPLAY


def _xdo(*a):
    subprocess.run(["xdotool", *a], env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _seed_lines(xwpe, n=50):
    """Type n short numbered lines so the editor gets a vertical scrollbar."""
    for i in range(n):
        _xdo("type", "--window", xwpe.wid, "L%02dxxxx" % i)
        _xdo("key", "--window", xwpe.wid, "Return")
    time.sleep(0.5)


def _to_top(xwpe):
    for _ in range(70):
        _xdo("key", "--window", xwpe.wid, "Up")
    time.sleep(0.3)


def test_vscrollbar_thumb_drag_scrolls(xwpe):
    """Dragging the vertical scrollbar thumb to the bottom moves the view and
    the cursor far down -- a typed marker lands near the end of the file."""
    _seed_lines(xwpe)
    _to_top(xwpe)
    xwpe.drag(1019, 70, 1019, 480)          # vertical thumb: top -> bottom
    xwpe.type("ZMARK")
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during scrollbar-thumb drag (Wayland)"
    lines = xwpe.saved_text().splitlines()
    idx = next((k for k, ln in enumerate(lines) if "ZMARK" in ln), -1)
    assert idx > 15, \
        "dragging the thumb to the bottom should move the view+cursor far down; " \
        "ZMARK landed at line %d of %d" % (idx, len(lines))
    assert any("L00xxxx" in ln for ln in lines), "early content should be intact"


def test_hscrollbar_thumb_drag_scrolls(xwpe):
    """Dragging the horizontal scrollbar thumb right moves the view and cursor
    far right -- a typed marker lands at a high column."""
    _xdo("type", "--window", xwpe.wid, "H" * 300)
    time.sleep(0.4)
    for _ in range(400):
        _xdo("key", "--window", xwpe.wid, "Left")   # cursor -> column 0
    time.sleep(0.3)
    xwpe.drag(220, 516, 850, 516)           # horizontal thumb: left -> right
    xwpe.type("HMARK")
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during horizontal scrollbar drag (Wayland)"
    marked = [ln for ln in xwpe.saved_text().splitlines() if "HMARK" in ln]
    assert marked, "HMARK not found in the saved file"
    col = marked[0].index("HMARK")
    assert col > 30, \
        "dragging the horizontal thumb right should move the view+cursor far " \
        "right; HMARK landed at column %d" % col
