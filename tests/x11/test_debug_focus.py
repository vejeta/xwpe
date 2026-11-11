"""Who has the keyboard and who is in front around a debug session (X11).

A debug start gives the editor's window the keyboard and leaves the
stacking alone, so the user types into the editor while whatever window is
in front stays there.  A debug stop brings the editor to the front and
gives it the keyboard, so the user sees where the program stopped.

The two answers come from two different X queries: `xdotool getwindowfocus`
is the keyboard focus (XGetInputFocus), `xdotool getactivewindow` is the
window manager's choice (_NET_ACTIVE_WINDOW, the front window).  A second
client window, `xlogo`, is mapped after the editor is up, so it is in front
and active; the rows then watch which of the two answers changes.

X11 only: a Wayland client can neither take the keyboard nor raise its
window, so the native Wayland display has no twin of these rows (its two
no-ops write a line in an --enable-trace build;
tests/wayland/test_screen_owner.py drives a debug stop there).  Both rows skip
by name when the window manager sets no _NET_ACTIVE_WINDOW (twm).
"""
import os
import shutil
import subprocess
import sys

import pytest

from conftest import DISPLAY, _sleep

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_menu_debug import BLOCKED_PROG, BREAK_LINE, RUN_TIMEOUT_S, _gdb, _wait  # noqa: E402

needs_tools = pytest.mark.skipif(
    not (shutil.which("gdb") and shutil.which("gcc") and shutil.which("xlogo")
         and shutil.which("xdotool")),
    reason="no gdb, gcc, xlogo or xdotool")

XLOGO_WAIT_S = 10


def _xdo_out(*args):
    """What an xdotool query printed, or None when it failed."""
    done = subprocess.run(["xdotool", *args],
                          env={**os.environ, "DISPLAY": DISPLAY},
                          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    if done.returncode != 0:
        return None
    return done.stdout.decode().strip()


def _focus_window():
    return _xdo_out("getwindowfocus")


def _active_window():
    return _xdo_out("getactivewindow")


def _map_xlogo():
    """Map a second window and wait until it is the active one; return its
    process (the caller ends it) and its window id."""
    proc = subprocess.Popen(
        ["xlogo", "-geometry", "120x120+850+620"],
        env={**os.environ, "DISPLAY": DISPLAY},
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    found = []

    def mapped():
        out = _xdo_out("search", "--class", "xlogo")
        found[:] = out.split() if out else []
        return bool(found)

    assert _wait(mapped, XLOGO_WAIT_S), "xlogo did not appear"
    _sleep(1.0)
    return proc, found[0]


def _skip_without_active_window_property():
    if _active_window() is None:
        pytest.skip("the window manager sets no _NET_ACTIVE_WINDOW")


def _break_and_run(xwpe):
    """Breakpoint after the read, Ctrl-G R."""
    for _ in range(BREAK_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")


def _wait_blocked(xwpe):
    started = os.path.join(os.path.dirname(xwpe.srcfile), "started.txt")
    assert _wait(lambda: os.path.exists(started), RUN_TIMEOUT_S), \
        "the program should build, start under gdb and run"
    assert _gdb(xwpe), "gdb should be running"
    _sleep(1.0)


@needs_tools
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCKED_PROG}],
                         indirect=True)
def test_a_debug_start_gives_the_editor_the_keyboard_and_leaves_the_stacking(xwpe):
    """With another window in front, a debug start moves the keyboard to the
    editor (getwindowfocus) and leaves the other window the active one
    (getactivewindow)."""
    _skip_without_active_window_property()
    logo, logo_win = _map_xlogo()
    try:
        assert _active_window() == logo_win, "xlogo should be the active window"
        before = _focus_window()
        assert before != xwpe.win, "the keyboard should not be in the editor yet"
        _break_and_run(xwpe)
        _wait_blocked(xwpe)
        assert _wait(lambda: _focus_window() == xwpe.win, 5), \
            "the debug start should give the editor the keyboard"
        assert _active_window() == logo_win, \
            "the debug start should leave the stacking alone"
    finally:
        logo.terminate()
        logo.wait(timeout=5)


@needs_tools
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCKED_PROG}],
                         indirect=True)
def test_a_debug_stop_brings_the_editor_window_to_the_front(xwpe):
    """The program runs, another window is mapped in front, the program
    reads its line and stops at the breakpoint: the editor is then both the
    active window and the one with the keyboard."""
    _skip_without_active_window_property()
    _break_and_run(xwpe)
    _wait_blocked(xwpe)
    logo, logo_win = _map_xlogo()
    try:
        assert _active_window() == logo_win, "xlogo should be the active window"
        assert _focus_window() != xwpe.win, "the keyboard should be elsewhere"
        xwpe.type("go")
        xwpe.key("Return")
        assert _wait(lambda: _active_window() == xwpe.win, 15), \
            "the debug stop should bring the editor to the front"
        assert _wait(lambda: _focus_window() == xwpe.win, 5), \
            "the debug stop should give the editor the keyboard"
    finally:
        logo.terminate()
        logo.wait(timeout=5)
