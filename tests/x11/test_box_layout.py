"""Boxes have one layout path, on X11 (the owner is we_layout.c).

The terminal rows are in tests/test_box_layout.py; these read the pixels the
graphical backend draws.  Two rows, both about a picture saved before a resize
that must never be put back:

- a window closed after a resize restores its background, which the desk's
  repaint made again at the new size, so the screen is the desk drawn afresh;
- a command that runs in a window of its own while the editor's window is
  resized: the editor takes the new size when the command ends, drawn afresh,
  and not as the screen the command left behind.

The Wayland twin (tests/wayland/test_box_layout.py) runs the same body.
"""
import os
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
