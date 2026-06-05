"""Run-menu (Alt-R) tests for xwpe (X11/Xft) -- the X11 twin of the wpe Run
coverage (tests/test_compile.py).

Make (F9) compiles the current file; we check for the compiled output on disk
(ground truth) and that the Messages window appears (screenshot diff).
"""
import os
import time

from conftest import changed_pixels


def test_make_compiles_the_file(xwpe):
    """Make (F9) compiles t.c -- compiled output appears and Messages opens."""
    d = os.path.dirname(xwpe.srcfile)
    before = xwpe.screenshot()
    xwpe.key("F9")                       # Run -> Make
    time.sleep(2.5)                      # gcc + relayout
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died during Make (F9)"
    produced = [f for f in os.listdir(d) if f not in ("t.c",)]
    assert produced, \
        "Make should produce compiled output (.o/.e/exe), dir=%r" % os.listdir(d)
    assert changed_pixels(before, after) > 2000, \
        "Make should open the Messages window (screen barely changed)"
