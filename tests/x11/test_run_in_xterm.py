"""Run of a shell script in a window of its own (xwpe, X11).

A script run with Ctrl-F9 opens in an xterm, and the editor's own window
must go on answering the display meanwhile: when the xterm stops covering
it, it is drawn again at once, instead of staying a blank rectangle until
the xterm is closed.
"""
import os
import shutil
import subprocess

import pytest

from conftest import _sleep, changed_pixels, xtool_env

SCRIPT_SECONDS = 14
SCRIPT = "#!/bin/sh\nsleep %d\n" % SCRIPT_SECONDS
XTERM_UP_S = 2.5
REPAINT_S = 2.0
XTERM_CLASS = "XTerm"
# The editor's top left corner: the menu bar and the first lines of the
# source.
CORNER = (0, 0, 480, 160)
# A window drawn again differs from the one before by almost nothing; a
# blank one by thousands of pixels.
REPAINT_TOLERANCE_PX = 200


def _xdo(*args):
    return subprocess.run(["xdotool", *args],
                          env=xtool_env(),
                          stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL).stdout.decode()


def _hide_the_xterm():
    """Unmap the xterm's window (its script goes on running), so it no
    longer covers the editor's."""
    ids = _xdo("search", "--class", XTERM_CLASS).split()
    for win in ids:
        _xdo("windowunmap", win)
    return ids


@pytest.mark.skipif(not shutil.which("xterm"), reason="no xterm")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": SCRIPT,
                                   "XWPE_TEST_FILE": "t.sh"}],
                         indirect=True)
# @scenario SC-debug-86
def test_x11_run_in_an_xterm_keeps_the_window_repainting(xwpe):
    """Ctrl-F9 on a script that sleeps: once its xterm stops covering the
    editor's window, the corner shows the editor's text again within
    seconds, though the script is still running."""
    reference = xwpe.screenshot().crop(CORNER)
    xwpe.key("ctrl+F9", delay=0.5)
    _sleep(XTERM_UP_S)
    assert _hide_the_xterm(), "the script did not open an xterm"
    _sleep(REPAINT_S)
    after = xwpe.screenshot().crop(CORNER)
    assert xwpe.proc.poll() is None
    differ = changed_pixels(reference, after)
    assert differ < REPAINT_TOLERANCE_PX, \
        "the window was not drawn again while the script ran (%d pixels of " \
        "its corner differ from the idle editor)" % differ
