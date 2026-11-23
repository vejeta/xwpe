"""Ctrl-Break (Pause) answers a missing session as every other way in (X11).

A terminal cannot send Ctrl-Break, so the terminal rows press Ctrl-G I (the
rows in tests/test_debugger_integration.py).  Under X11 the key itself comes
through the display, and it must reach the same table entry as the Ctrl-G
letter: with no session the status row says "No debug session", not the
line the debugger's own Pause says ("Nothing to pause").

The sentence is read from the screen as pixels: the status row after
Ctrl-Break must be the one Ctrl-G I leaves, and differ from the row with
nothing said.  A key that went around the table says another sentence and
leaves other pixels.
"""
import os

import pytest
from PIL import ImageChops

from conftest import _sleep, changed_pixels

TRACE = "ui.trace"
SESSION = {"XWPE_UI_TRACE": TRACE}
CTRL_G = "ctrl+g"
BREAK_KEY = "ctrl+Break"
PAUSE_ACTION = "Interrupt"      # the name the binding table gives Pause
# The status row of one sentence against another: a few hundred pixels
# apart at the fixture's font, so equal rows are well under this.
SAME_ROW_PIXELS = 20
PIXEL_THRESHOLD = 40
SAID_SOMETHING_PIXELS = 100


def _actions(xwpe):
    path = os.path.join(os.path.dirname(xwpe.srcfile), TRACE)
    if not os.path.exists(path):
        return []
    with open(path) as fh:
        return [line.strip().rsplit("action=", 1)[1] for line in fh if "action=" in line]


def _quiet_screen(xwpe):
    """The screen once a plain key has cleared what the status row said."""
    xwpe.key("Right", delay=0.6)
    return xwpe.screenshot()


# @scenario SC-debug-45
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_ctrl_break_with_no_session_says_what_ctrl_g_i_says(xwpe):
    """Both keys reach one entry, so the status row is the same pixels."""
    quiet = _quiet_screen(xwpe)
    xwpe.key(CTRL_G, "i", delay=0.8)
    by_letter = xwpe.screenshot()
    said = ImageChops.difference(quiet, by_letter).convert("L").point(
        lambda p: 255 if p > PIXEL_THRESHOLD else 0).getbbox()
    assert said is not None and changed_pixels(quiet, by_letter) > SAID_SOMETHING_PIXELS, \
        "Ctrl-G I should say there is no session"
    _quiet_screen(xwpe)
    xwpe.key(BREAK_KEY, delay=0.8)
    assert _actions(xwpe).count(PAUSE_ACTION) == 2, \
        "Ctrl-G I and Ctrl-Break should each reach Pause: %s" % _actions(xwpe)
    by_break = xwpe.screenshot()
    differ = changed_pixels(by_letter.crop(said), by_break.crop(said))
    assert differ < SAME_ROW_PIXELS, \
        "Ctrl-Break should say what Ctrl-G I says (%d pixels of the status row differ)" % differ
