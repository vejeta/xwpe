"""The window gets its picture back when another window uncovers it.

Another window covers the editor's and goes away.  The X server then sends
the editor an Expose, and the editor must put its picture back: from the back
buffer when it has one (the Xft and Cairo builds), and by painting the cells
again when it has none (a core-font build, or an editor whose Xft found no
font).  Either way the corner shows the editor's text as before, not the
blank window the cover left.

This drives the key reader's Expose path (the editor is idle, waiting for a
key); tests/x11/test_short_command_repaint.py drives the other one (the
editor waits for a command).  Both end in the one function that answers an
Expose.

There is no Wayland twin: a Wayland compositor keeps the buffer the editor
committed, so an uncovered window never asks for its picture again
(we_wlframe.c owns that buffer).
"""
import subprocess
import time

import pytest
from host_tools import needs

from conftest import changed_pixels, launch_client, xtool_env
from test_resize import NO_FONT_ENV, PAINTED_MIN_PIXELS, _painted_pixels

COVER_UP_S = 1.5
REPAINT_S = 1.5
COVER_CLASS = "XLogo"
# The editor's top left corner: the menu bar and the first lines of the
# source.
CORNER = (0, 0, 480, 160)
# A window drawn again differs from the one before by almost nothing; a
# blank or covered one by thousands of pixels.
REPAINT_TOLERANCE_PX = 200
# The two states of the editor: as the build starts it (a back buffer on an
# Xft build, none on a core-font build), and with no buffer on any build.
STATES = [pytest.param(None, id="as-built"),
          pytest.param(NO_FONT_ENV, id="core-font")]


def _xdo(*args):
    return subprocess.run(["xdotool", *args], env=xtool_env(),
                          stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL).stdout.decode()


@pytest.fixture
def cover(gui_owner, tmp_path, request):
    """A window that covers the editor's once it is mapped; the owner ends
    it with the row, also when the row fails."""
    return launch_client(gui_owner, tmp_path, request, ["xlogo"])


@needs("xlogo")
# @scenario SC-x11-backbuf-02
@pytest.mark.parametrize("xwpe", STATES, indirect=True)
def test_the_window_is_drawn_again_when_it_is_uncovered(xwpe, request):
    """Another window covers the idle editor's and goes away: the corner
    shows the editor's text again within seconds."""
    reference = xwpe.screenshot().crop(CORNER)
    assert _painted_pixels(reference) > PAINTED_MIN_PIXELS, \
        "the editor's corner is not painted before the cover: an editor " \
        "that never paints would pass the comparison below"
    request.getfixturevalue("cover")
    time.sleep(COVER_UP_S)
    covered = changed_pixels(reference, xwpe.screenshot().crop(CORNER))
    assert covered > REPAINT_TOLERANCE_PX, \
        "the other window did not cover the editor's: the row proves nothing"
    for win in _xdo("search", "--class", COVER_CLASS).split():
        _xdo("windowunmap", win)
    time.sleep(REPAINT_S)
    differ = changed_pixels(reference, xwpe.screenshot().crop(CORNER))
    assert differ < REPAINT_TOLERANCE_PX, \
        "the window was not drawn again once uncovered (%d pixels of its " \
        "corner differ from the editor before the cover)" % differ
    assert xwpe.proc.poll() is None, "xwpe died"
