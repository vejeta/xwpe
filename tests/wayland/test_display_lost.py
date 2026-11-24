"""The native-Wayland editor keeps unsaved work when its compositor goes away.

The compositor crashes, the session ends or a protocol error closes the
connection.  The editor used to linger with no window and no way to be ended
but a signal, and the text typed since the last save was lost.  Now it writes
every window with unsaved changes to its .ESV file (`xwpe -r` reopens it),
prints one line saying the Wayland compositor is gone, and exits with code 1,
as it does when its terminal is closed.

The child-ending step of e_display_lost_exit (the X11 build without Xlib's
exit handler) is checked by hand: see WAYLAND-TESTING.txt.

Each row starts an X server and a weston of its own and ends the weston
itself, never the suite's.  The rows declare the loss and the exit with
expect_display_loss() and expect_exit(code=1), so a crash or an exit nobody
asked for still fails them.

test_compositor_end_saves_and_exits
    Type, end the compositor through the owner, and the editor exits with 1
    within a scaled deadline, tells the user, and the text is in t.c.ESV.
    Red before the lost-display owner: the editor was alive after the
    deadline, with no .ESV file.

test_compositor_end_ends_the_debugger_and_saves
    The same while dbx (the stand-in of tests/stubs/legacy) holds the program
    at a breakpoint: the editor's end still tells the debugger to quit, the
    debugger goes, and the .ESV file is written.  The X11 twin is in
    tests/x11/test_display_lost.py.
"""
import os
import signal
import subprocess
import time

import pytest
from host_tools import needs

import gui_owner as owner_api
from conftest import (SCREEN, WL_HEIGHT, WL_WIDTH, _WAIT_SCALE, _xdo,
                      serve_display)
from test_debug_answer_wait import (
    DBX_BREAK_LINE, DBX_STUB_ENV, DBX_STUB_LOG, MIN_BAR_PIXELS, STOP_GREEN,
    STOP_TIMEOUT_S, _count, _go_to_line, _wait, dbx_stub)

EXIT_DEADLINE_S = 10 * _WAIT_SCALE
SLOW_EXIT_DEADLINE_S = 60 * _WAIT_SCALE
GONE_S = 6
TYPED = "hello lost display"
LOST_LINE = "xwpe: connection to the Wayland compositor lost"
# Enough numbered lines for a vertical scrollbar, and where its thumb starts
# (the geometry of tests/wayland/test_mouse_scroll.py).
DRAG_LINES = 50
THUMB_X, THUMB_Y = 1019, 70
# Where button 1 is held when the display ends (window pixels): the menu bar,
# the text, and the scrollbar's track below the thumb.
HELD_AT = {"menu": (30, 5), "text": (300, 250), "track": (THUMB_X, 400)}
STUB_PID_FILE = "stub.pid"     # relative: the debugger runs in the source's directory
CORE_SIGNALS = ("SIGBUS", "SIGSEGV", "SIGQUIT", "SIGABRT", "SIGILL",
                "SIGFPE")
LOST_STUB_ENV = dict(DBX_STUB_ENV, XWPE_STUB_NOTE_SIGNALS="1",
                     XWPE_STUB_PID_FILE=STUB_PID_FILE)
# The stand-in answers "quit" with output for longer than the end waits.
SLOW_QUIT_NAP_S = "120"
SLOW_QUIT_ENV = dict(LOST_STUB_ENV, XWPE_STUB_QUIT_NAP_S=SLOW_QUIT_NAP_S)


@pytest.fixture
def xserver(gui_owner, tmp_path, request):
    """An X server and a weston of this row, in place of the suite's: the row
    ends the weston, so no other row may depend on it.  The owner ends what is
    left with the row.  Yields the owner's record of the compositor, and the
    rows after this one ask the suite's compositor again, not this one."""
    log_dir = str(tmp_path)
    x_display = owner_api.start_x_server(gui_owner, request, log_dir, SCREEN)
    compositor = owner_api.start_compositor(gui_owner, request, x_display,
                                            log_dir, WL_WIDTH, WL_HEIGHT)
    with serve_display(compositor):
        yield compositor


def _wait_exit(editor, deadline=EXIT_DEADLINE_S):
    """The editor's exit code, or None when it still runs after the deadline."""
    try:
        return editor.proc.wait(timeout=deadline)
    except subprocess.TimeoutExpired:
        return None


def _read_pid(path):
    try:
        with open(path) as fh:
            text = fh.read().strip()
    except OSError:
        return None
    return int(text) if text.isdigit() else None


def _gone(pid):
    """True once pid has ended (a zombie has), asked of the process table
    through the one owner (gui_owner.process_running), never kill(pid, 0)."""
    return not owner_api.process_running(pid)


SCREEN_QUIET_READS = 3
SCREEN_QUIET_POLL_S = 0.3
SCREEN_QUIET_DEADLINE_S = 30.0


def _wait_screen_quiet(xwpe):
    """Wait, bounded, until the editor has handled what was typed: its screen
    is the same on SCREEN_QUIET_READS reads in a row.  A key still queued
    would otherwise be lost when the display ends."""
    deadline = time.time() + SCREEN_QUIET_DEADLINE_S * _WAIT_SCALE
    last, same = None, 0
    while time.time() < deadline:
        now = xwpe.screenshot().tobytes()
        same = same + 1 if now == last else 0
        if same >= SCREEN_QUIET_READS:
            return
        last = now
        time.sleep(SCREEN_QUIET_POLL_S)
    raise AssertionError("the editor's screen never settled after typing")


def _lost_line(editor):
    """The editor's own line about the lost display, from its log, or None."""
    for line in editor.owned.log_text().splitlines():
        if line.startswith("xwpe: connection to"):
            return line
    return None


def test_compositor_end_saves_and_exits(xserver, xwpe):
    """End the compositor under an editor with typed, unsaved text: it exits 1
    within the deadline, says the compositor is gone, and t.c.ESV holds the
    text."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    xwpe.type(TYPED, delay=0.8)
    xserver.compositor.end()
    code = _wait_exit(xwpe)
    assert code is not None, "xwpe still running %ds after its compositor ended" % EXIT_DEADLINE_S
    assert code == 1, "exit code %r, wanted 1" % code
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    saved = xwpe.srcfile + ".ESV"
    assert os.path.exists(saved), sorted(os.listdir(os.path.dirname(saved)))
    with open(saved) as fh:
        assert TYPED in fh.read()


@pytest.mark.parametrize("where", sorted(HELD_AT), ids=sorted(HELD_AT))
def test_compositor_end_with_a_button_held_saves_and_exits(xserver, xwpe, where):
    """The compositor ends while button 1 is held down on the menu bar, in
    the text or on the scrollbar's track: the editor must reach the wait that
    ends it.  It spun at full speed in the loops that wait for the button to
    be let go, because no release can come from a compositor that is gone."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    for i in range(DRAG_LINES):
        _xdo("type", "--window", xwpe.wid, "L%02dxxxx" % i)
        _xdo("key", "--window", xwpe.wid, "Return")
    time.sleep(0.4)
    px, py = HELD_AT[where]
    _xdo("mousemove", "--window", xwpe.wid, str(px), str(py))
    time.sleep(0.2)
    _xdo("mousedown", "1")
    time.sleep(0.4)
    xserver.compositor.end()
    code = _wait_exit(xwpe)
    assert code is not None, "xwpe still running %ds after its compositor ended" % EXIT_DEADLINE_S
    assert code == 1, "exit code %r, wanted 1" % code
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    assert os.path.exists(xwpe.srcfile + ".ESV")


def test_compositor_end_during_a_scrollbar_drag_saves_and_exits(xserver, xwpe):
    """The compositor ends while the scrollbar thumb is held down, the editor
    waiting inside the drag for the next pointer event: it exits 1, says so,
    and t.c.ESV holds the text."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    for i in range(DRAG_LINES):
        _xdo("type", "--window", xwpe.wid, "L%02dxxxx" % i)
        _xdo("key", "--window", xwpe.wid, "Return")
    _wait_screen_quiet(xwpe)
    for _ in range(DRAG_LINES + 20):        # the thumb is at the top
        xwpe.key("Up", delay=0.0)
    _wait_screen_quiet(xwpe)
    _xdo("mousemove", "--window", xwpe.wid, str(THUMB_X), str(THUMB_Y))
    time.sleep(0.2)
    _xdo("mousedown", "1")
    time.sleep(0.1)
    for y in range(THUMB_Y + 10, THUMB_Y + 230, 20):
        _xdo("mousemove", "--window", xwpe.wid, str(THUMB_X), str(y))
        time.sleep(0.03)
    time.sleep(0.3)
    xserver.compositor.end()
    code = _wait_exit(xwpe)
    assert code is not None, "xwpe still running %ds after its compositor ended" % EXIT_DEADLINE_S
    assert code == 1, "exit code %r, wanted 1" % code
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    with open(xwpe.srcfile + ".ESV") as fh:
        assert "L%02dxxxx" % (DRAG_LINES - 1) in fh.read()


@needs("gcc")
@pytest.mark.parametrize("xwpe,slow", [(LOST_STUB_ENV, False),
                                        (SLOW_QUIT_ENV, True)],
                         indirect=["xwpe"], ids=["quits", "slow-quit"])
def test_compositor_end_ends_the_debugger_and_saves(xserver, dbx_stub, xwpe, slow):
    """The compositor ends at a stop under dbx: the editor saves, exits 1, the
    debugger is told to quit and goes, and it is sent no signal that dumps
    core.

    In the slow-quit row the debugger keeps writing output after "quit" for
    longer than the editor's end waits, so the end runs while output arrives
    and the editor paints on a connection that is gone: the end itself must
    have ended the debugger (the stand-in never logs QUIT FINISHED), and the
    save and the line are there."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, DBX_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 STOP_TIMEOUT_S), "Ctrl-G R should stop on the breakpoint"
    pid = _read_pid(os.path.join(home, STUB_PID_FILE))
    assert pid, "the stand-in wrote no PID"
    xwpe.type(TYPED, delay=0.8)
    try:
        xserver.compositor.end()
        code = _wait_exit(xwpe, SLOW_EXIT_DEADLINE_S if slow else EXIT_DEADLINE_S)
        assert code == 1, "exit code %r, wanted 1" % code
        deadline = time.time() + GONE_S * _WAIT_SCALE
        while time.time() < deadline and not _gone(pid):
            time.sleep(0.1)
        assert _gone(pid), "dbx (pid %d) was left running" % pid
        with open(os.path.join(home, DBX_STUB_LOG)) as fh:
            log = fh.read()
        cores = [s for s in CORE_SIGNALS if ("SIGNAL " + s) in log]
        assert not cores, "dbx was sent %s, which dumps core" % cores
        if slow:
            assert "QUIT FINISHED" not in log, "dbx ended by itself, not by the end"
            assert _lost_line(xwpe) is not None, xwpe.owned.log_text()
        with open(xwpe.srcfile + ".ESV") as fh:
            assert TYPED in fh.read()
    finally:
        if not _gone(pid):
            os.kill(pid, signal.SIGKILL)
