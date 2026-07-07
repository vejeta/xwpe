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
