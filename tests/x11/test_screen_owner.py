"""The screen has one owner, on X11: program output waits for a dialog.

The terminal row (tests/test_screen_owner.py) reads the cells; this one reads
the pixels the graphical backend draws.  A program under gdb prints all the
time; while the Find dialog is up not one pixel of the window changes, the
editor's trace (XWPE_UI_TRACE) says the output waited, and once Esc closes
the dialog the output scrolls on in Messages.

The Wayland twin (tests/wayland/test_screen_owner.py) runs the same body and
skips, as the other debugger rows there do: the headless compositor harness
cannot start a debug session.  Its live check is a line in
WAYLAND-TESTING.txt.
"""
import os
import shutil
import time

import pytest

from conftest import changed_pixels, _sleep

PRINTER_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int i = 0;\n"
    "  FILE *o = fopen(\"started.txt\", \"w\");\n"
    "  fclose(o);\n"
    "  setvbuf(stdout, NULL, _IONBF, 0);\n"
    "  for (;;) { printf(\"PRINTER%06d\\n\", i++); usleep(20000); }\n"
    "  return 0;\n"
    "}\n"
)
UNREACHED_LINE = 10           # return 0;  after the endless loop
TRACE = "ui.trace"
RUN_TIMEOUT_S = 40
SHOW_TIMEOUT_S = 10
HOLD_S = 1.5
SETTLE_S = 1.0
DEFERRED_OUTPUT = "paint deferred program output"
REPLAYED_OUTPUT = "paint replay program output"


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
@pytest.mark.skipif(not shutil.which("gdb") or not shutil.which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": PRINTER_PROG,
                                   "XWPE_UI_TRACE": TRACE}], indirect=True)
def test_program_output_under_a_dialog_paints_nothing_under_it_and_shows_when_it_closes(xwpe):
    """While the Find dialog is up the window does not change at all; the
    trace shows the output waited and was drawn once the dialog closed."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    for _ in range(UNREACHED_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: os.path.exists(_in_dir(xwpe, "started.txt")),
                 RUN_TIMEOUT_S), "the program should build, start and run"
    assert _wait(lambda: _screen_moves(xwpe), SHOW_TIMEOUT_S), \
        "the program's output should be drawn in Messages"
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
