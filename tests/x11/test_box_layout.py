"""Boxes have one layout path, on X11 (the owner is we_layout.c).

The terminal rows are in tests/test_box_layout.py; these read the pixels the
graphical backend draws.  Two rows, both about a picture saved before a resize
that must never be put back:

- a window closed after a resize restores its background, which the desk's
  repaint made again at the new size, so the screen is the desk drawn afresh;
- a command that runs in a window of its own while the editor's window is
  resized: the editor takes the new size when the command ends, drawn afresh,
  and not as the screen the command left behind.

The About and System Info boxes are the owner's as well: after a resize they
are centred in the window, and the About box on a window 21 rows tall stays
clear of the status row (its fixed place put its lower rows over it).

The Wayland twin (tests/wayland/test_box_layout.py) runs the same body.
"""
import os
import re
import shutil
import subprocess

import pytest

from conftest import changed_pixels, _sleep, xtool_env

START_W, START_H = 1024, 768      # the size every graphical row starts at
FULL_W, FULL_H = 880, 620
SMALL_W, SMALL_H = 800, 560
SETTLE_S = 1.5
SCRIPT_SECONDS = 6
SCRIPT = "#!/bin/sh\nsleep %d\n" % SCRIPT_SECONDS
XTERM_UP_S = 2.5


def _xdo(*args):
    subprocess.run(["xdotool", *args], env=xtool_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _window(xwpe):
    return getattr(xwpe, "win", None) or xwpe.wid


def _size(xwpe, w, h):
    _xdo("windowsize", _window(xwpe), str(w), str(h))
    _sleep(SETTLE_S)


def _let_the_window_be_sized(xwpe):
    """The harness's window manager keeps an application window full-screen;
    a window it does not manage keeps the size it is given, as for a user."""
    if getattr(xwpe, "win", None) is None:
        xwpe.key("Escape", delay=SETTLE_S)   # Wayland: the editor is up
        return
    _xdo("set_window", "--overrideredirect", "1", xwpe.win)
    _xdo("windowunmap", "--sync", xwpe.win)
    _xdo("windowmap", "--sync", xwpe.win)
    _sleep(SETTLE_S)
    xwpe.focus()


def _answer_the_xterm():
    """Press Return in the xterm the script runs in: it ends there, and the
    editor's window is the one the user sees again."""
    found = subprocess.run(["xdotool", "search", "--class", "XTerm"],
                           env=xtool_env(),
                           stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL).stdout.decode().split()
    assert found, "the script's xterm is not there"
    _xdo("windowfocus", found[0])
    _sleep(0.3)
    _xdo("key", "Return")
    _sleep(SETTLE_S)


def _where(a, b):
    from PIL import ImageChops
    return ImageChops.difference(a, b).getbbox()


def _resized(before, after):
    if before.size != after.size:
        return True
    return changed_pixels(before, after) > 0


# @scenario SC-box-06
def test_a_window_closed_after_a_resize_leaves_the_desk_drawn_afresh(xwpe):
    """Two windows, a resize, the top one closed: every window's saved view
    was made again by the desk's repaint at the new size, so closing restores
    the right background and the screen equals a single window's desk drawn
    at that size."""
    _let_the_window_be_sized(xwpe)
    try:
        _size(xwpe, FULL_W, FULL_H)
        full = xwpe.screenshot()
        _size(xwpe, SMALL_W, SMALL_H)
        fresh = xwpe.screenshot()
        assert _resized(full, fresh), "the window did not change size"
        _size(xwpe, FULL_W, FULL_H)
        xwpe.menu("f", "n")                       # File > New: a second window
        _sleep(SETTLE_S)
        _size(xwpe, SMALL_W, SMALL_H)
        two = xwpe.screenshot()
        assert changed_pixels(two, fresh) > 0, "the second window did not open"
        xwpe.key("ctrl+w", delay=0.8)             # close the top window
        _sleep(SETTLE_S)
        assert xwpe.proc.poll() is None, "xwpe died closing the window"
        closed = xwpe.screenshot()
        assert changed_pixels(closed, fresh) == 0, \
            "%d pixels differ between the screen after the window closed and " \
            "the desk drawn afresh at that size, within %s" % (
                changed_pixels(closed, fresh), _where(closed, fresh))
    finally:
        _size(xwpe, START_W, START_H)


@pytest.mark.skipif(not shutil.which("xterm"), reason="no xterm")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": SCRIPT,
                                   "XWPE_TEST_FILE": "t.sh"}],
                         indirect=True)
# @scenario SC-box-06
def test_a_command_window_does_not_bring_back_the_old_screen(xwpe):
    """Ctrl-F9 on a script that sleeps, the editor's window resized while its
    xterm runs, the script ended: the editor is the desk drawn afresh at the
    new size (a control row: X11 opens no screen-saving view for a command
    that runs in its own window).  X11 only: the Wayland editor has no X
    display for the script's xterm."""
    if getattr(xwpe, "win", None) is None:
        pytest.skip("the command's xterm needs an X display: X11 only")
    _let_the_window_be_sized(xwpe)
    try:
        _size(xwpe, FULL_W, FULL_H)
        _size(xwpe, SMALL_W, SMALL_H)
        fresh = xwpe.screenshot()
        _size(xwpe, FULL_W, FULL_H)
        xwpe.key("ctrl+F9", delay=0.5)
        _sleep(XTERM_UP_S)
        _size(xwpe, SMALL_W, SMALL_H)
        _sleep(SCRIPT_SECONDS)                    # the script ends, then asks for Return
        _answer_the_xterm()
        _sleep(SETTLE_S)
        assert xwpe.proc.poll() is None, "xwpe died after the command"
        after = xwpe.screenshot()
        assert changed_pixels(after, fresh) == 0, \
            "%d pixels differ between the editor after the command and the " \
            "desk drawn afresh at that size, within %s" % (
                changed_pixels(after, fresh), _where(after, fresh))
    finally:
        _size(xwpe, START_W, START_H)


@pytest.mark.skipif(not shutil.which("xterm"), reason="no xterm")
@pytest.mark.parametrize("how", ["ctrl+F9", "the Run menu"])
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": SCRIPT,
                                   "XWPE_TEST_FILE": "t.sh"}],
                         indirect=True)
# @scenario SC-box-06
def test_growing_the_window_while_a_command_runs_leaves_the_desk_afresh(
        xwpe, how):
    """The window made larger while the script runs in its xterm: when the
    script ends the editor takes the new size before it paints anything, so
    it is the desk drawn afresh at that size.  Taking the size while the
    command still ran changed the grid under screen buffers of the old
    size: the editor read past them (a black window, and a memory error
    under a sanitizer).  X11 only, as the command's xterm needs an X
    display."""
    if getattr(xwpe, "win", None) is None:
        pytest.skip("the command's xterm needs an X display: X11 only")
    _let_the_window_be_sized(xwpe)
    try:
        _size(xwpe, FULL_W, FULL_H)
        fresh = xwpe.screenshot()
        _size(xwpe, SMALL_W, SMALL_H)
        if how == "ctrl+F9":
            xwpe.key("ctrl+F9", delay=0.5)
        else:
            xwpe.menu("r", "r")
        _sleep(XTERM_UP_S)
        _size(xwpe, FULL_W, FULL_H)
        _sleep(SCRIPT_SECONDS)
        _answer_the_xterm()
        xwpe.focus()
        _sleep(SETTLE_S)
        assert xwpe.proc.poll() is None, "xwpe died after the command"
        after = xwpe.screenshot()
        assert changed_pixels(after, fresh) == 0, \
            "%d pixels differ between the editor after the command (%s) and " \
            "the desk drawn afresh at that size, within %s" % (
                changed_pixels(after, fresh), how, _where(after, fresh))
    finally:
        _size(xwpe, START_W, START_H)


STATUS_PX = 17                    # the status row: one text row at the pinned font
TRACE_NAME = "ui.trace"           # relative: the editor's folder is the row's own
TRACED = {"XWPE_UI_TRACE": TRACE_NAME}
TALL_W, TALL_H = 800, 560
SHORT_W, SHORT_H = 800, 365       # 21 rows of the 17 pixel text row the harness pins
SHORT_ROWS = 21
ABOUT_KEYS = ("alt+f", "Left", "a")
SYSTEM_INFO_KEYS = ("alt+f", "Left", "s")
ABOUT_W, ABOUT_H = 55, 18
GRID_RE = re.compile(r"layout resized from \d+x\d+ to (\d+)x(\d+)")
PLACED_RE = re.compile(r"layout placed (?:the )?(.+?) box at (\d+),(\d+) (\d+)x(\d+)")


def _grid_and_placement(xwpe, box):
    """Where the owner placed `box` ("About", "System Info") the last time,
    and the grid the window had then, from the editor's trace: (cols, rows,
    left, top, width, height)."""
    trace = os.path.join(os.path.dirname(xwpe.srcfile), TRACE_NAME)
    grid_now = None
    placed = None
    with open(trace) as fh:
        for line in fh:
            grid = GRID_RE.search(line)
            found = PLACED_RE.search(line)
            if grid:
                grid_now = (int(grid.group(1)), int(grid.group(2)))
            elif found and found.group(1) == box:
                assert grid_now is not None, "the trace names no resize before the box"
                placed = grid_now + tuple(int(found.group(i)) for i in range(2, 6))
    assert placed is not None, "the owner did not place the %s box" % box
    return placed


def _the_box_after_a_resize(xwpe, keys, width, height):
    """Open the box with the window at the larger size, resize the window to
    width x height, and return the screenshots with the box up and, after
    Esc, without it."""
    _size(xwpe, FULL_W, FULL_H)
    xwpe.key(*keys, delay=0.6)
    _sleep(SETTLE_S)
    _size(xwpe, width, height)
    assert xwpe.proc.poll() is None, "xwpe died on the resize under the box"
    under = xwpe.screenshot()
    xwpe.key("Escape", delay=0.8)
    _sleep(SETTLE_S)
    return under, xwpe.screenshot()


# @scenario SC-box-04
@pytest.mark.parametrize("xwpe", [TRACED], indirect=True)
@pytest.mark.parametrize("size", [(TALL_W, TALL_H), (SHORT_W, SHORT_H)])
def test_the_about_box_is_centred_and_clear_of_the_status_row(xwpe, size):
    """After the window was resized the About box is centred between the menu
    bar and the status row, and on a window 21 rows tall it ends above the
    status row.  Its fixed place, column 8, row 3, was neither (a pixel
    check of the status row, then the owner's place from the trace)."""
    width, height = size
    _let_the_window_be_sized(xwpe)
    try:
        under, closed = _the_box_after_a_resize(xwpe, ABOUT_KEYS, width, height)
    finally:
        _size(xwpe, START_W, START_H)
    status = (0, height - STATUS_PX, width, height)
    assert changed_pixels(under.crop(status), closed.crop(status)) == 0, \
        "About wrote on the status row, which starts at y %d" % (height - STATUS_PX)
    cols, rows, left, top, w, h = _grid_and_placement(xwpe, "About")
    if size == (SHORT_W, SHORT_H):
        assert rows == SHORT_ROWS, "the window has %d rows, the row needs %d" % (rows, SHORT_ROWS)
    assert (w, h) == (ABOUT_W, ABOUT_H), "About was placed %dx%d" % (w, h)
    assert left == (cols - ABOUT_W) // 2, "About starts at column %d of %d" % (left, cols)
    assert top == 1 + (rows - 2 - ABOUT_H) // 2, "About starts at row %d of %d" % (top, rows)
    assert top + ABOUT_H - 1 <= rows - 2, "About reaches the status row"


# @scenario SC-box-04
@pytest.mark.parametrize("xwpe", [TRACED], indirect=True)
def test_system_info_is_centred_after_a_resize(xwpe):
    """The System Info box keeps the same margin left and right and sits in
    the middle between the menu bar and the status row after a resize: it
    used to start at row 3 for ever."""
    _let_the_window_be_sized(xwpe)
    try:
        _the_box_after_a_resize(xwpe, SYSTEM_INFO_KEYS, TALL_W, TALL_H)
    finally:
        _size(xwpe, START_W, START_H)
    cols, rows, left, top, w, h = _grid_and_placement(xwpe, "System Info")
    assert 2 * left + w == cols, "System Info has %d columns left and %d right" % (
        left, cols - left - w)
    assert top == 1 + (rows - 2 - h) // 2, \
        "System Info starts at row %d of %d, %d rows tall" % (top, rows, h)


ANY_PLACED_RE = re.compile(r"layout placed (.+?) at (\d+),(\d+) (\d+)x(\d+)")
ERROR_TEXT_W = 20                 # "Can't open directory": the box's least text width
AI_ENV = {"XWPE_UI_TRACE": TRACE_NAME, "XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
MOVE_LEFT_BY = 4


def _placements(xwpe, what):
    """Every place the owner gave the box named `what`, in order, each with
    the grid the window had then: [(cols, rows, left, top, width, height)]."""
    trace = os.path.join(os.path.dirname(xwpe.srcfile), TRACE_NAME)
    grid_now = None
    found = []
    with open(trace) as fh:
        for line in fh:
            grid = GRID_RE.search(line)
            placed = ANY_PLACED_RE.search(line)
            if grid:
                grid_now = (int(grid.group(1)), int(grid.group(2)))
            elif placed and placed.group(1) == what and grid_now is not None:
                found.append(grid_now + tuple(int(placed.group(i)) for i in range(2, 6)))
    return found


# @scenario SC-box-04
@pytest.mark.parametrize("xwpe", [TRACED], indirect=True)
def test_the_error_box_keeps_its_corner_and_follows_a_resize(xwpe):
    """A control: the error box opened, the window resized under it: the
    owner places it again at the corner it has had since 1.5.30a, a box of
    the text's width centred as one column and one row smaller, for the
    new grid, and Return still closes it."""
    _let_the_window_be_sized(xwpe)
    try:
        _size(xwpe, FULL_W, FULL_H)
        xwpe.key("alt+f", "a", delay=0.8)
        xwpe.key("ctrl+u", delay=0.3)
        xwpe.type("/proc/nope/x.c")
        xwpe.key("Return", delay=1.0)
        _sleep(SETTLE_S)
        _size(xwpe, SMALL_W, SMALL_H)
        assert xwpe.proc.poll() is None, "xwpe died on the resize under the error box"
        xwpe.key("Return", delay=0.8)
        assert xwpe.proc.poll() is None, "xwpe died closing the error box"
    finally:
        _size(xwpe, START_W, START_H)
    placed = _placements(xwpe, "an error box")
    assert len(placed) >= 2, "the owner did not place the error box again: %r" % placed
    cols, rows, left, top, w, h = placed[-1]
    assert left == (cols - ERROR_TEXT_W) // 2 - 2, \
        "the error box starts at column %d of %d" % (left, cols)
    assert top == (rows - 6) // 2, "the error box starts at row %d of %d" % (top, rows)


# @scenario SC-box-07
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_a_moved_dialog_keeps_its_place_across_a_picker_and_a_resize_centres_it(xwpe):
    """The AI settings dialog moved four columns left with Alt-F2, a picker
    opened over it and closed: the dialog is drawn again where the user left
    it (the same pixels as right after the move, and not those of the place
    it opened at).  Then the window is resized and the owner centres it by
    its rule again, at the place it has when opened fresh at that size."""
    _let_the_window_be_sized(xwpe)
    try:
        _size(xwpe, FULL_W, FULL_H)
        xwpe.key("alt+o", "i", delay=0.8)
        _sleep(SETTLE_S)
        opened = xwpe.screenshot()
        xwpe.key("alt+F2", delay=0.6)
        xwpe.key(*(["Left"] * MOVE_LEFT_BY), delay=0.3)
        xwpe.key("Return", delay=0.8)
        _sleep(SETTLE_S)
        moved = xwpe.screenshot()
        xwpe.key("alt+v", delay=1.0)
        _sleep(SETTLE_S)
        picker = xwpe.screenshot()
        xwpe.key("Escape", delay=1.0)
        _sleep(SETTLE_S)
        kept = xwpe.screenshot()
        _size(xwpe, SMALL_W, SMALL_H)
        assert xwpe.proc.poll() is None, "xwpe died on the resize under the dialog"
        xwpe.key("Escape", delay=0.8)
        _sleep(SETTLE_S)
        xwpe.key("alt+o", "i", delay=0.8)
        _sleep(SETTLE_S)
        xwpe.key("Escape", delay=0.8)
    finally:
        _size(xwpe, START_W, START_H)
    assert changed_pixels(opened, moved) > 0, "Alt-F2 and Left did not move the dialog"
    assert changed_pixels(moved, picker) > 0, "the picker did not open"
    assert changed_pixels(moved, kept) == 0, \
        "%d pixels differ between the dialog after the picker closed and right " \
        "after the move, within %s" % (changed_pixels(moved, kept), _where(moved, kept))
    placed = _placements(xwpe, "a dialog")
    assert len(placed) >= 3, "the owner placed the dialog %d times: %r" % (len(placed), placed)
    after_resize, fresh = placed[-2], placed[-1]
    assert after_resize[:2] == fresh[:2], "the grid changed between the two: %r" % placed
    assert after_resize == fresh, \
        "after the resize the dialog is at %r, opened fresh it is at %r" % (
            after_resize, fresh)
