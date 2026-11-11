"""The screen has one owner, on X11: a build's output waits for a dialog.

The terminal rows (tests/test_screen_owner_callbacks.py) read the cells; this
one reads the pixels the graphical backend draws.  Run > Execute Make starts
a make that prints a line every quarter of a second; while the Find dialog is
up not one pixel of the window changes, the editor's trace (XWPE_UI_TRACE)
says the build's output waited, and once Esc closes the dialog the output
goes on in Messages.

The Wayland twin (tests/wayland/test_screen_owner_callbacks.py) runs the same
body: no debugger is needed, so it runs there, where the debugger rows of the
screen owner cannot.
"""
import os
import time

import pytest

from conftest import changed_pixels, _sleep

SOURCE = "int main(void)\n{\n    return 0;\n}\n"
SLOW_MAKEFILE = (
    "all:\n"
    "\t@for i in $$(seq 1 80); do echo BUILDLINE$$i; sleep 0.25; done\n"
)
TRACE = "ui.trace"
SHOW_TIMEOUT_S = 15
HOLD_S = 1.5
SETTLE_S = 1.0
DEFERRED_OUTPUT = "paint deferred the build's output"
REPLAYED_OUTPUT = "paint replay the build's output"


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _in_dir(xwpe, name):
    return os.path.join(os.path.dirname(xwpe.srcfile), name)


def _trace(xwpe):
    try:
        with open(_in_dir(xwpe, TRACE)) as fh:
            return fh.read()
    except OSError:
        return ""


def _screen_moves(xwpe):
    """True when the window changes within a moment: output is drawn."""
    first = xwpe.screenshot()
    _sleep(SETTLE_S)
    return changed_pixels(first, xwpe.screenshot()) > 0


# @scenario SC-screen-01
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": SOURCE,
                                   "XWPE_UI_TRACE": TRACE}], indirect=True)
def test_a_build_streaming_under_a_dialog_paints_nothing_under_it_and_shows_when_it_closes(xwpe):
    """While the Find dialog is up the window does not change at all; the
    trace shows the build's output waited and was drawn once the dialog
    closed."""
    with open(_in_dir(xwpe, "Makefile"), "w") as fh:
        fh.write(SLOW_MAKEFILE)
    xwpe.key("alt+a", delay=0.8)
    assert _wait(lambda: _screen_moves(xwpe), SHOW_TIMEOUT_S), \
        "the build's output should be drawn in Messages"
    xwpe.menu("s", "f", delay=0.8)
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    _sleep(HOLD_S)
    after = xwpe.screenshot()
    assert changed_pixels(before, after) == 0, \
        "%d pixels changed while the dialog was up" % changed_pixels(before, after)
    assert DEFERRED_OUTPUT in _trace(xwpe), \
        "the output that came under the dialog should wait:\n%s" % _trace(xwpe)
    xwpe.key("Escape", delay=0.8)
    assert _wait(lambda: _screen_moves(xwpe), SHOW_TIMEOUT_S), \
        "the output should be drawn again once the dialog closes"
    assert REPLAYED_OUTPUT in _trace(xwpe), \
        "the waiting output should be drawn by the key loop:\n%s" % _trace(xwpe)
    assert xwpe.proc.poll() is None, "xwpe died closing the dialog"
