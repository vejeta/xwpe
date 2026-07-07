"""Resizing the X11 window re-lays-out the editor and never crashes.

X11 handles resize in its ConfigureNotify handler (drain to the final size,
re-fit the grid, re-lay-out, repaint).  This drives a shrink/grow sweep of the
xwpe window, including a deep shrink where the bottom key-hint bar no longer
fits (the case whose off-grid extbyte write corrupted the heap on Wayland and
was latent here) -- xwpe must survive every size.

This is also the safety net for refactoring the (duplicated) ConfigureNotify
handler: it fails if a resize stops re-laying-out or starts crashing.
"""
import time

from conftest import DISPLAY, _xdo


def _size(win, w, h):
    _xdo("windowsize", win, str(w), str(h))


def test_x11_resize_sweep_survives(xwpe):
    xwpe.key("i")  # ensure the editor has focus / is interactive
    for rep in range(2):
        for w in range(1000, 360, -24):        # shrink deep (bottom bar overflows)
            _size(xwpe.win, w, int(w * 0.75))
            time.sleep(0.03)
        for w in range(360, 1000, 24):
            _size(xwpe.win, w, int(w * 0.75))
            time.sleep(0.03)
        assert xwpe.proc.poll() is None, "xwpe died during X11 resize (rep %d)" % rep
    _size(xwpe.win, 1000, 750)
    time.sleep(0.4)
    assert xwpe.proc.poll() is None, "xwpe died after X11 resize sweep"
