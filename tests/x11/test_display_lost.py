"""The graphical editor keeps unsaved work when its X server goes away.

The X server crashes, the session ends or the connection is cut.  Xlib's own
handler used to end the editor at once with its "X connection broken" line,
and the text typed since the last save was lost.  Now the editor writes every
window with unsaved changes to its .ESV file (`xwpe -r` reopens it), prints
one line saying the X server is gone, and exits with code 1, the code Xlib's
handler gave.

The child-ending step of e_display_lost_exit (the build without Xlib's exit
handler) is checked by hand: see WAYLAND-TESTING.txt.

Each row starts an X server of its own (and its window manager) and ends it
itself, never the suite's.  The rows declare the loss and the exit with
expect_display_loss() and expect_exit(code=1), so a crash or an exit nobody
asked for still fails them.

test_x_server_end_saves_and_exits
    Type, end the X server through the owner, and the editor exits with 1
    within a scaled deadline, tells the user, and the text is in t.c.ESV.
    Red before the lost-display owner: the editor exited 1 with Xlib's own
    line and no .ESV file.

test_x_server_end_ends_the_debugger_and_saves
    The same while dbx (the stand-in of tests/stubs/legacy) holds the program
    at a breakpoint: the editor's end still tells the debugger to quit, the
    debugger goes, and the .ESV file is written.  The Wayland twin is in
    tests/wayland/test_display_lost.py.

test_x_server_end_inside_a_scrollbar_drag_refuses_the_repaint
    The server ends while a scrollbar drag holds the screen, and the drag then
    ends: from the editor's own trace, the pointer grab, both repaints, the
    pointer poll and the display teardown are refused in that order.  Red
    before the owner of the X11 connection: the drag's end let go of the
    pointer and repainted on a server that was gone.
"""
import os
import signal
import subprocess
import time

import pytest
from host_tools import needs

import gui_owner as owner_api
from conftest import (SCREEN, _WAIT_SCALE, _start_window_manager, _xdo,
                      serve_display)
from test_debug_answer_wait import (
    DBX_BREAK_LINE, DBX_STUB_ENV, DBX_STUB_LOG, MIN_BAR_PIXELS, STOP_GREEN,
    STOP_TIMEOUT_S, _count, _go_to_line, _wait, dbx_stub)

EXIT_DEADLINE_S = 10 * _WAIT_SCALE
SLOW_EXIT_DEADLINE_S = 60 * _WAIT_SCALE
GONE_S = 6
TYPED = "hello lost display"
LOST_LINE = "xwpe: connection to the X server lost"
# Enough numbered lines for a vertical scrollbar, and where its thumb starts
# (the geometry of tests/x11/test_mouse_scroll.py).
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
TRACE_NAME = "ui.trace"           # relative: the editor's folder is the row's own
TRACED = {"XWPE_UI_TRACE": TRACE_NAME}
# What the editor's own trace says when a scrollbar drag takes the screen
# (e_scroll_drag_begin) and when it gives it back (e_scroll_drag_end, after the
# repaint its end asks for).
DRAG_HELD_LINE = "a scrollbar drag"
DRAG_LEFT_LINE = "modal leave"
# What the owner of the X11 connection says about a call it refused because
# the server is gone: one line per call, naming what it refused.
REFUSED_AFTER_LOSS = "after the loss"
REFUSED_REFRESH = "refused the refresh after the loss"
REFUSED_GRAB = "refused the pointer grab after the loss"
REFUSED_POINTER = "refused the pointer state after the loss"
REFUSED_TEARDOWN = "refused the display teardown after the loss"

@pytest.fixture
def xserver(gui_owner, tmp_path, request):
    """An X server of this row, with its window manager, in place of the
    suite's: the row ends it, so no other row may depend on it.  The owner
    ends what is left with the row, and the rows after this one ask the
    suite's display again, not this one."""
    display = owner_api.start_x_server(gui_owner, request, str(tmp_path), SCREEN)
    with serve_display(display):
        _start_window_manager(gui_owner, request, display, str(tmp_path))
        yield display


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


def _trace_lines(editor):
    """The editor's own trace lines so far, from the row's folder."""
    try:
        with open(os.path.join(os.path.dirname(editor.srcfile),
                               TRACE_NAME)) as fh:
            return fh.read().splitlines()
    except OSError:
        return []


def _fill_and_drag_the_thumb(xwpe):
    """Type DRAG_LINES numbered lines, put the scrollbar's thumb at the top,
    take it and drag it down.

    The editor is then inside the drag loop, which it leaves only for the
    button's release or for a display that is gone, and no release is sent
    here: what ends the drag is the row's next step."""
    for i in range(DRAG_LINES):
        _xdo("type", "--window", xwpe.win, "L%02dxxxx" % i)
        _xdo("key", "--window", xwpe.win, "Return")
    _wait_screen_quiet(xwpe)
    for _ in range(DRAG_LINES + 20):        # the thumb is at the top
        xwpe.key("Up", delay=0.0)
    _wait_screen_quiet(xwpe)
    _xdo("mousemove", "--window", xwpe.win, str(THUMB_X), str(THUMB_Y))
    time.sleep(0.2)
    _xdo("mousedown", "1")
    time.sleep(0.1)
    for y in range(THUMB_Y + 10, THUMB_Y + 230, 20):
        _xdo("mousemove", "--window", xwpe.win, str(THUMB_X), str(y))
        time.sleep(0.03)


def test_x_server_end_saves_and_exits(xserver, xwpe):
    """End the X server under an editor with typed, unsaved text: it exits 1
    within the deadline, says the X server is gone, and t.c.ESV holds the text."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    xwpe.type(TYPED, delay=0.8)
    xserver.server.end()
    code = _wait_exit(xwpe)
    assert code is not None, "xwpe still running %ds after its X server ended" % EXIT_DEADLINE_S
    assert code == 1, "exit code %r, wanted 1" % code
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    saved = xwpe.srcfile + ".ESV"
    assert os.path.exists(saved), sorted(os.listdir(os.path.dirname(saved)))
    with open(saved) as fh:
        assert TYPED in fh.read()


@pytest.mark.parametrize("where", sorted(HELD_AT), ids=sorted(HELD_AT))
def test_x_server_end_with_a_button_held_saves_and_exits(xserver, xwpe, where):
    """The X server ends while button 1 is held down on the menu bar, in the
    text or on the scrollbar's track: the editor must reach the wait that ends
    it.  It used to spin at full speed, because a pointer that cannot be
    queried was still reported as holding the button, so every poll was a
    click and the editor never waited.

    Reference mutation (the [menu] row goes red, the editor hangs): in
    e_x_getch remove the key_b initialisation, the WpeXInfo.lost check and
    the XQueryPointer return check together.  Removing only one of them does
    not reproduce the hang."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    xwpe.type(TYPED, delay=0.5)
    if where == "track":
        for i in range(DRAG_LINES):
            _xdo("type", "--window", xwpe.win, "L%02dxxxx" % i)
            _xdo("key", "--window", xwpe.win, "Return")
        time.sleep(0.4)
    px, py = HELD_AT[where]
    _xdo("mousemove", "--window", xwpe.win, str(px), str(py))
    time.sleep(0.2)
    _xdo("mousedown", "1")
    time.sleep(1.0)
    xserver.server.end()
    code = _wait_exit(xwpe)
    assert code is not None, "xwpe still running %ds after its X server ended" % EXIT_DEADLINE_S
    assert code == 1, "exit code %r, wanted 1" % code
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    assert os.path.exists(xwpe.srcfile + ".ESV")


def test_x_server_end_during_a_scrollbar_drag_saves_and_exits(xserver, xwpe):
    """The X server ends while the scrollbar thumb is held down, the editor
    waiting inside the drag for the next pointer event: it exits 1, says so,
    and t.c.ESV holds the text.  Before the drag waited for events itself,
    Xlib's read crashed the editor once its handlers had returned: no .ESV
    and no line."""
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    _fill_and_drag_the_thumb(xwpe)
    time.sleep(0.3)
    xserver.server.end()
    code = _wait_exit(xwpe)
    assert code is not None, "xwpe still running %ds after its X server ended" % EXIT_DEADLINE_S
    assert code == 1, "exit code %r, wanted 1 (a crash is %r)" % (code, code)
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    with open(xwpe.srcfile + ".ESV") as fh:
        assert "L%02dxxxx" % (DRAG_LINES - 1) in fh.read()


@needs("gcc")
@pytest.mark.parametrize("xwpe,slow", [(LOST_STUB_ENV, False),
                                        (SLOW_QUIT_ENV, True)],
                         indirect=["xwpe"], ids=["quits", "slow-quit"])
def test_x_server_end_ends_the_debugger_and_saves(xserver, dbx_stub, xwpe, slow):
    """The X server ends at a stop under dbx: the editor saves, exits 1, the
    debugger is told to quit and goes, and it is sent no signal that dumps core.

    In the slow-quit row the debugger keeps writing output after "quit" for
    longer than the editor's end waits, so the end runs while output arrives
    on a display that is gone: the end itself must have ended the debugger
    (the stand-in never logs QUIT FINISHED), and the save and the line are
    there."""
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
        xserver.server.end()
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


def _at(lines, text, start=0):
    """The index of the first line at or after `start` that holds `text`, or
    None.  The refusals are asserted in the order the trace holds them, so a
    site that only refuses part of the work cannot pass the row."""
    for i in range(start, len(lines)):
        if text in lines[i]:
            return i
    return None


@pytest.mark.parametrize("xwpe", [TRACED], indirect=["xwpe"])
def test_x_server_end_inside_a_scrollbar_drag_refuses_the_repaint(xserver, xwpe):
    """The X server ends while the editor is inside the scrollbar drag, and the
    drag then ends: no call of the four paths that follow may reach the server
    that is gone.

    The row proves the order from the editor's own trace.  When the server
    ends, the drag's line is there and its leave is not, so the editor is
    inside the drag loop, which it leaves only for the button's release or the
    loss; no release is sent, so what ends the drag is the loss, and the leave
    that then appears in the trace comes after it.  Four paths in three files
    then run against a display that is gone, and only an owner of the X11
    connection refuses all four:

    1. the drag's end (we_mouse.c): it lets go of the pointer and repaints, so
       "the pointer grab" and "the refresh" are refused before "modal leave";
    2. the key reader's entry (we_xterm.c e_x_getch): one more "the refresh"
       after "modal leave";
    3. its pointer poll: "the pointer state";
    4. the end itself (we_xterm.c e_x_display_end, reached from the exit
       teardown): "the display teardown", the last refusal of the run.

    Without the owner these reach libXrender, and on an X stack that does not
    drop requests after the I/O error handler returned (the CI runner's) the
    editor dies of SIGSEGV in XRenderCompositeText8 with no .ESV file and no
    line.
    """
    xwpe.owned.expect_display_loss()
    xwpe.owned.expect_exit(code=1)
    _fill_and_drag_the_thumb(xwpe)
    _wait_screen_quiet(xwpe)                # the drag painted the last motion
    held = _trace_lines(xwpe)
    assert any(DRAG_HELD_LINE in line for line in held), held[-3:]
    assert not any(DRAG_LEFT_LINE in line for line in held), held[-3:]
    xserver.server.end()
    code = _wait_exit(xwpe)
    assert code == 1, "exit code %r, wanted 1 (a crash is %r)" % (code, code)
    left = _trace_lines(xwpe)
    leave = _at(left, DRAG_LEFT_LINE)
    assert leave is not None, \
        "the drag never ended after the loss: %r" % left[-3:]
    grab = _at(left, REFUSED_GRAB)
    assert grab is not None and grab < leave, \
        "the drag's end still let go of the pointer on a server that is gone: %r" % left[-3:]
    first_paint = _at(left, REFUSED_REFRESH)
    assert first_paint is not None and first_paint < leave, \
        "the drag's end still repainted on a server that is gone: %r" % left[-3:]
    second_paint = _at(left, REFUSED_REFRESH, leave + 1)
    assert second_paint is not None, \
        "the key reader still repainted after the drag gave the screen back: %r" % left[-3:]
    pointer = _at(left, REFUSED_POINTER, second_paint + 1)
    assert pointer is not None, \
        "the key reader still asked the server for the pointer: %r" % left[-3:]
    teardown = _at(left, REFUSED_TEARDOWN, pointer + 1)
    assert teardown is not None, \
        "the end still tore the display down on a server that is gone: %r" % left[-3:]
    refusals = [i for i, line in enumerate(left) if REFUSED_AFTER_LOSS in line]
    assert teardown == refusals[-1], \
        "something still reached the server after the teardown: %r" % left[teardown + 1:]
    line = _lost_line(xwpe)
    assert line is not None and line.startswith(LOST_LINE), xwpe.owned.log_text()
    with open(xwpe.srcfile + ".ESV") as fh:
        assert "L%02dxxxx" % (DRAG_LINES - 1) in fh.read()

