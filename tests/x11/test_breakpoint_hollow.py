"""A breakpoint the debugger has not confirmed is drawn as an outline (X11/Xft).

The bar of a confirmed breakpoint is black text on red; one the debugger has
not confirmed yet (set while the program runs, until the next stop) is the same
red as the text colour on the editor's own background, so the user sees at a
glance which marks the debugger really has.  The terminal rows are in
tests/test_debugger_integration.py; these read the pixels the graphical
backends draw, which is where the colour entry has to end up whatever the
backend.

The Wayland twin (tests/wayland/test_breakpoint_hollow.py) runs the same
bodies; the one that needs a debug session skips there, as the Pause row of
test_menu_debug.py does, and the dialog row runs on both.
"""
import os
import shutil
import sys

import pytest
from PIL import Image, ImageChops

from conftest import _sleep

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))

# The palette entries the colour scheme names, as the backends draw them.
RED3 = (205, 0, 0)              # entry 1: the breakpoint bar, the hollow text
EDITOR_BG = (72, 61, 139)       # entry 4: the editor's background
STOP_GREEN = (0, 255, 0)        # entry 10: the stop line's bar
STOP_SAMPLE = "Stop at Breakpoint"
STOP_SAMPLE_CELLS = len(STOP_SAMPLE)
UNCONFIRMED_SAMPLE_CELLS = len("An unconfirmed Breakpoint")
EXAMPLE_WIDTH_SHARE = 0.25      # the example box is the dialog's left part,
                                # before the colour palette (which holds a
                                # swatch of the stop line's green too)
MIN_BAR_PIXELS = 60             # a sample bar is a dozen cells wide
MIN_TEXT_PIXELS = 10           # glyph pixels in the outline's own colour:
                                # anti-aliasing leaves few at the exact red
SOLID_OVER_HOLLOW = 2           # the bar has this many times the red pixels
RUN_TIMEOUT_S = 40
TICK_PAUSE_WAIT_S = 10
CURSOR_LINE = 9                 # fopen(...) inside TICK_PROG's loop

# A program that writes a counter to a file ten times a second, as the Pause
# row of test_menu_debug.py has it: the file stops changing while gdb holds it.
TICK_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int n;\n"
    "  for (n = 0; ; n++)\n"
    "  {\n"
    "    FILE *o = fopen(\"tick.txt\", \"w\");\n"
    "    fprintf(o, \"%d\", n);\n"
    "    fclose(o);\n"
    "    usleep(100000);\n"
    "  }\n"
    "  return 0;\n"
    "}\n"
)


def _wait(predicate, timeout):
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _count(img, rgb):
    """How many pixels of @img are exactly @rgb."""
    colours = img.getcolors(maxcolors=img.width * img.height) or []
    return sum(count for count, px in colours if px == rgb)


def _tick(xwpe):
    path = os.path.join(os.path.dirname(xwpe.srcfile), "tick.txt")
    try:
        with open(path) as fh:
            text = fh.read().strip()
    except OSError:
        return None
    return int(text) if text.isdigit() else None


def _ticks_move(xwpe):
    before = _tick(xwpe)
    _sleep(1.5)
    after = _tick(xwpe)
    return before is not None and after is not None and after != before


def _go_to_line(xwpe, line):
    xwpe.menu("s", "g", delay=0.6)
    xwpe.type(str(line), delay=0.4)
    xwpe.key("Return", delay=0.6)


@pytest.mark.skipif(not shutil.which("gdb") or not shutil.which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": TICK_PROG}],
                         indirect=True)
def test_a_breakpoint_set_while_running_stops_the_program_there(xwpe):
    """A breakpoint set on the loop gdb's program runs is applied at once:
    gdb is interrupted, told and let run on, and the program stops on the
    line at its next pass, without a Pause: the counter stops and the stop
    line's green bar is drawn.  The Wayland twin skips: the headless
    compositor harness cannot start a debug session."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    xwpe.key("ctrl+g", "r", delay=2.0)          # to the start of main
    xwpe.key("ctrl+g", "r", delay=1.0)          # run on
    assert _wait(lambda: _ticks_move(xwpe), RUN_TIMEOUT_S), \
        "the program should run under gdb and write its counter"
    _go_to_line(xwpe, CURSOR_LINE)
    xwpe.key("ctrl+g", "b", delay=1.0)
    assert _wait(lambda: not _ticks_move(xwpe), TICK_PAUSE_WAIT_S), \
        "the breakpoint set while running should stop the program"
    _sleep(1.0)
    assert _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS, \
        "the stop on the line should be drawn as the green bar"


def _example_box(img):
    """The dialog's left part, where its example lines are."""
    return img.crop((0, 0, int(img.width * EXAMPLE_WIDTH_SHARE), img.height))


def _bar_box(img, rgb):
    """The box of the one wide bar of colour @rgb in @img: the rows that hold
    at least MIN_BAR_PIXELS pixels of it (single glyphs and window buttons in
    the same green are too narrow to count), and the columns they cover."""
    diff = ImageChops.difference(img, Image.new("RGB", img.size, rgb))
    mask = diff.convert("L").point(lambda p: 255 if p == 0 else 0)
    data = mask.tobytes()
    rows = [y for y in range(mask.height)
            if sum(1 for v in data[y * mask.width:(y + 1) * mask.width]
                   if v) >= MIN_BAR_PIXELS]
    if not rows:
        return None
    left, _, right, _ = mask.crop((0, rows[0], mask.width,
                                   rows[-1] + 1)).getbbox()
    return (left, rows[0], right, rows[-1] + 1)


def test_the_colours_dialog_samples_the_unconfirmed_breakpoint(xwpe):
    """Options > Adjust Colors > Editor Colors draws a sample line below the
    stop line's: red text on the editor's blue, not a red bar like the
    confirmed breakpoint's sample above it."""
    xwpe.menu("o", "a", delay=0.8)
    xwpe.key("Return", delay=1.0)
    img = _example_box(xwpe.screenshot())
    box = _bar_box(img, STOP_GREEN)
    assert box is not None, "the stop line's sample bar should be on screen"
    left, top, right, bottom = box
    cell_w = (right - left) / STOP_SAMPLE_CELLS
    cell_h = bottom - top
    width = int(cell_w * UNCONFIRMED_SAMPLE_CELLS)
    hollow = img.crop((left, bottom, left + width, bottom + cell_h))
    solid = img.crop((left, top - cell_h, left + width, top))
    text = _count(hollow, RED3)
    assert text >= MIN_TEXT_PIXELS, \
        "the unconfirmed sample should show red text, got %d red pixels" % text
    assert _count(hollow, EDITOR_BG) > text, \
        "the unconfirmed sample should sit on the editor's background"
    assert _count(solid, RED3) >= SOLID_OVER_HOLLOW * text, \
        "the confirmed sample above should be the solid bar"
