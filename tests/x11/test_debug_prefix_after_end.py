"""A command prefix typed as the debug session ends never lands in the file
(X11).

The terminal rows are in tests/test_debug_prefix_after_end.py.  Under X11
every key comes through the display's own event loop, also while the
program runs to its end, so Ctrl-G I, Ctrl-G K or Alt-Q typed right after
Ctrl-G R run or are refused and their letter is not typed into the source.
"""
import os
import shutil
import sys
import time

import pytest

from conftest import _sleep

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

STOP_GREEN = (0, 255, 0)        # entry 10: the stop line's bar
# A stop bar fills its line's cells, some 2000 pixels at the fixture's font.
# The same green is also the colour of "#include" text and of letters on the
# window's status row, about 75 pixels with no stop at all, so the bar is
# counted only well above that.
MIN_BAR_PIXELS = 400
STOP_TIMEOUT_S = 40
END_TIMEOUT_S = 25
POLL_S = 0.3
PRINT_LINE = 4
UNUSED_LETTER = "z"
C_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"READY\\n\");\n"
    "  return 0;\n"
    "}\n"
)


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(POLL_S)
    return predicate()


def _stop_bar_drawn(xwpe):
    """The green bar of a stop line is on screen."""
    img = xwpe.screenshot()
    colours = img.getcolors(maxcolors=img.width * img.height) or []
    green = sum(count for count, px in colours if px == STOP_GREEN)
    return green >= MIN_BAR_PIXELS


def _stop_on_print_line(xwpe):
    """Breakpoint on the printf line, Ctrl-G R, wait for the stop bar."""
    xwpe.menu("s", "g", delay=0.6)
    xwpe.type(str(PRINT_LINE), delay=0.4)
    xwpe.key("Return", delay=0.6)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _stop_bar_drawn(xwpe), STOP_TIMEOUT_S), \
        "gdb should stop on line %d" % PRINT_LINE


def _continue_then(xwpe, *keys):
    """Ctrl-G R and, with no pause, the keys of another command."""
    xwpe.key("ctrl+g", "r", delay=0.0)
    xwpe.key(*keys, delay=0.0)


def _assert_untouched(xwpe, what):
    """The session has ended and the file on disk is the seed."""
    assert _wait(lambda: not _stop_bar_drawn(xwpe), END_TIMEOUT_S), \
        "the program should run to its end"
    _sleep(1.0)
    xwpe.key("Escape", delay=0.5)
    xwpe.save()
    assert xwpe.saved_text() == C_PROG, "%s was typed into the file" % what


def _need_gdb(xwpe):
    if not shutil.which("gdb") or not shutil.which("gcc"):
        pytest.skip("no gdb or gcc")
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")


@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": C_PROG}],
                         indirect=True)
@pytest.mark.parametrize("letter", ["i", "k"])
def test_ctrl_g_letter_after_continue_is_a_command(xwpe, letter):
    """Ctrl-G R then Ctrl-G I / Ctrl-G K at once: the letter is not text."""
    _need_gdb(xwpe)
    _stop_on_print_line(xwpe)
    _continue_then(xwpe, "ctrl+g", letter)
    _assert_untouched(xwpe, "Ctrl-G " + letter)


@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": C_PROG}],
                         indirect=True)
def test_alt_q_after_continue_is_a_prefix(xwpe):
    """Ctrl-G R then Alt-Q at once: the Alt key press stays with the editor,
    so the letter after it is not typed into the file."""
    _need_gdb(xwpe)
    _stop_on_print_line(xwpe)
    _continue_then(xwpe, "alt+q", UNUSED_LETTER)
    _assert_untouched(xwpe, "the key after Alt-Q")
