"""Resizing while the pointer moves over the window must not crash (Wayland).

The resize relayout repaints the desktop, and that repaint can poll the pointer
(fk_w_mouse -> wl_pump_once); driving a resize from there re-entered the relayout
and reallocated the cell grid under the outer repaint (a use-after-free).  This
drives a resize sweep while jiggling the pointer inside the window -- the
combination that crashed on a real kwin session (and under ASAN headless).
"""
import os
import subprocess
import time

from conftest import DISPLAY


def _xdo(*a):
    subprocess.run(["xdotool", *a], env={**os.environ, "DISPLAY": DISPLAY},
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def test_resize_with_pointer_motion(xwpe):
    xwpe.type("int main(){ return 0; }")
    for rep in range(3):
        for i, w in enumerate(range(1000, 400, -20)):
            _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.72)))
            _xdo("mousemove", "--window", xwpe.wid,
                 str(20 + (i % 30) * 8), str(40 + (i % 10) * 8))
            time.sleep(0.02)
        for i, w in enumerate(range(400, 1000, 20)):
            _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.72)))
            _xdo("mousemove", "--window", xwpe.wid,
                 str(30 + (i % 25) * 8), str(50 + (i % 8) * 8))
            time.sleep(0.02)
        assert xwpe.proc.poll() is None, \
            "xwpe died resizing with pointer motion (rep %d)" % rep
    assert xwpe.proc.poll() is None
