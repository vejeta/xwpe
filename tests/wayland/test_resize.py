"""Resizing the window re-fits the editor grid (native Wayland).

The compositor draws the frame (xdg-decoration) and drives the size; when the
user drags an edge, xwpe must re-fit its cell grid to the new window size and
re-lay-out its windows -- the mirror of X11's ConfigureNotify handler.  Before
that, a shrink left MAXSCOL/MAXSLNS too large and the next paint wrote past the
smaller wl_shm buffer (a crash).  This drives a shrink then a grow of the weston
output (the surface is fullscreen under kiosk, so it follows the output size)
and checks xwpe survives and the dumped buffer tracks the window both ways.
"""
import os
import subprocess
import time

from conftest import DISPLAY


def _xdo(*a):
    subprocess.run(["xdotool", *a], env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def test_resize_refits_grid(xwpe):
    xwpe.type("resize regression content")
    before = xwpe.screenshot()
    assert before is not None, "no initial frame"

    _xdo("windowsize", xwpe.wid, "640", "400")   # shrink -- the old crash case
    time.sleep(1.2)
    assert xwpe.proc.poll() is None, "xwpe died shrinking the window"
    small = xwpe.screenshot()
    assert small is not None, "no frame after shrink"
    assert small.size[0] < before.size[0], "buffer did not shrink with the window"

    _xdo("windowsize", xwpe.wid, "1000", "700")  # grow again
    time.sleep(1.2)
    assert xwpe.proc.poll() is None, "xwpe died growing the window"
    big = xwpe.screenshot()
    assert big is not None, "no frame after grow"
    assert big.size[0] > small.size[0], "buffer did not grow with the window"


def test_deep_shrink_survives(xwpe):
    """Shrinking the window far enough that the bottom key-hint bar's buttons no
    longer fit must not crash.  With the grid down to ~48 columns a button's
    start column runs past the grid, so the bar label's frame (e_make_xrect) was
    asked to mark an extbyte cell one past the MAXSCOL*MAXSLNS plane -- an
    off-grid write that corrupted the heap (ASAN: heap-buffer-overflow in
    e_make_xrect).  A continuous grow/shrink sweep exercises many grid sizes."""
    xwpe.type("int main(){ return 0; }")
    for _ in range(2):
        for w in range(1000, 380, -16):        # shrink deep, to ~48 columns
            _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.75)))
            time.sleep(0.03)
        for w in range(380, 1000, 16):
            _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.75)))
            time.sleep(0.03)
    assert xwpe.proc.poll() is None, "xwpe died shrinking the bottom bar past the grid"
