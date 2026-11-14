"""Esc ends a short command the editor waits for, on X11.

Help on a word runs `man` and waits for it, with a deadline, before it opens
the page.  The user who does not want to wait presses Esc: the command is
ended at once and the editor takes keys again, instead of waiting out the
deadline.  The keys typed before Esc stay the editor's.

The Wayland twin (tests/wayland/test_short_command_cancel.py) runs the same
body.  On the terminal the keyboard waits for the editor while a short
command runs; tests/test_short_commands.py pins that.
"""
import os
import stat
import subprocess
import sys
import time

import pytest

from conftest import _sleep, xtool_env

# The Wayland twin runs this module under its own conftest, which does not
# put tests/ on the path.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procinfo  # noqa: E402  (the tests/ directory is added just above)

SLOW_MAN = '''#!/bin/sh
echo $$ > "%(dir)s/man.pid"
exec sleep 30
'''
WAIT_STARTS_S = 6          # Help > Topic search is running man by then
ENDED_S = 4                # Esc ends it well before its 30 s
QUIT_S = 6                 # the editor reads the key typed ahead and quits
TRACE = "ui.trace"
CANCEL_LINE = "Esc: stopped waiting for"
BOX_OPENED = "modal enter"    # the trace line of any box taking the screen
SETTLE_S = 1.5             # whatever Esc leads to has been drawn by then
STATUS_ROW_PX = 48         # the bottom row, where the cancel line is said
CHANGED_PX_MAX = 200       # a window or a box is thousands of pixels


def _alive(pid):
    """True while pid runs: an ended process nobody has collected yet (a
    zombie) is gone, though kill(pid, 0) still finds it."""
    return procinfo.run_state(pid) not in (procinfo.GONE, procinfo.ZOMBIE)


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.2)
    return predicate()


@pytest.fixture
def slow_man(tmp_path, monkeypatch):
    """A `man` that takes thirty seconds, first on the editor's PATH."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    stub = bindir / "man"
    stub.write_text(SLOW_MAN % {"dir": str(tmp_path)})
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", "%s:%s" % (bindir, os.environ.get("PATH", "")))
    return tmp_path


# @scenario SC-screen-08
def test_esc_ends_a_short_command_the_editor_waits_for(slow_man, xwpe):
    """Help > Topic search starts `man`; Esc ends it within seconds, and the
    editor is alive and answers."""
    xwpe.menu("h", "t", delay=0.2)
    pid_file = slow_man / "man.pid"
    assert _wait(pid_file.exists, WAIT_STARTS_S), "help never ran man"
    pid = int(pid_file.read_text().strip())
    assert _alive(pid), "man ended by itself: the row proves nothing"
    xwpe.key("Escape", delay=0.2)
    assert _wait(lambda: not _alive(pid), ENDED_S), \
        "Esc should end the command the editor waits for"
    assert xwpe.proc.poll() is None, "xwpe died ending the command"


@pytest.fixture
def esc_released():
    """Release Esc on the X server after a row whose editor quits on it.

    xdotool sends each half of a key to the window it names only while that
    window has the focus, and through the server otherwise.  An editor that
    quits between Esc's press and its release has no window left, so the
    release never reaches the server: Esc stays down there, and the next
    editor on the shared display is never told of an Esc again."""
    yield
    subprocess.run(["xdotool", "keyup", "Escape"],
                   env=xtool_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# @scenario SC-screen-08
def test_a_key_typed_before_esc_is_still_read_by_the_editor(slow_man, xwpe,
                                                             esc_released):
    """While the editor waits for `man`, the user types Alt-X and then Esc.
    Esc ends the command and only Esc is taken: the Alt-X typed before it
    is read by the editor afterwards, which then quits."""
    xwpe.menu("h", "t", delay=0.2)
    pid_file = slow_man / "man.pid"
    assert _wait(pid_file.exists, WAIT_STARTS_S), "help never ran man"
    pid = int(pid_file.read_text().strip())
    assert _alive(pid), "man ended by itself: the row proves nothing"
    xwpe.key("alt+x", delay=0.2)
    xwpe.key("Escape", delay=0.2)
    assert _wait(lambda: not _alive(pid), ENDED_S), \
        "Esc should end the command the editor waits for"
    assert _wait(lambda: xwpe.proc.poll() is not None, QUIT_S), \
        "the Alt-X typed before Esc was lost: the editor did not quit"


def _trace(xwpe):
    try:
        with open(os.path.join(os.path.dirname(xwpe.srcfile), TRACE)) as fh:
            return fh.read()
    except OSError:
        return ""


def _above_status_row(img):
    return img.crop((0, 0, img.width, img.height - STATUS_ROW_PX))


def _assert_cancelled_quietly(xwpe, before, trace_before):
    """After Esc: the editor says it stopped waiting, opened no error box
    and no window, and the desk above the status row is as it was."""
    _sleep(SETTLE_S)
    said = _trace(xwpe)[len(trace_before):]
    assert BOX_OPENED not in said, "an error box opened after Esc:\n" + said
    from conftest import changed_pixels
    moved = changed_pixels(_above_status_row(before),
                           _above_status_row(xwpe.screenshot()))
    assert moved <= CHANGED_PX_MAX, \
        "%d pixels changed: a window or a box opened after Esc" % moved
    assert CANCEL_LINE in said, "nothing said the wait was stopped:\n" + said
    assert xwpe.proc.poll() is None, "xwpe died ending the command"


# @scenario SC-screen-08
@pytest.mark.parametrize("xwpe", [{"XWPE_UI_TRACE": TRACE}], indirect=True)
def test_esc_on_help_says_it_stopped_and_shows_no_error(slow_man, xwpe):
    """Help on a word with a slow `man`, then Esc: the editor says it
    stopped waiting; it does not open a window saying there is no manual
    entry, for the user chose not to wait, which is not a missing page."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.menu("h", "t", delay=0.2)
    pid_file = slow_man / "man.pid"
    assert _wait(pid_file.exists, WAIT_STARTS_S), "help never ran man"
    pid = int(pid_file.read_text().strip())
    assert _alive(pid), "man ended by itself: the row proves nothing"
    trace_before = _trace(xwpe)
    xwpe.key("Escape", delay=0.2)
    assert _wait(lambda: not _alive(pid), ENDED_S), \
        "Esc should end the command the editor waits for:\n" + _trace(xwpe)
    _assert_cancelled_quietly(xwpe, before, trace_before)


SLOW_PRINT = '''#!/bin/sh
echo $$ > "%(dir)s/print.pid"
exec sleep 30
'''


@pytest.fixture
def slow_print(tmp_path):
    """A print command that takes thirty seconds, set in the editor's
    options so nothing reaches a real printer."""
    stub = tmp_path / "slowprint"
    stub.write_text(SLOW_PRINT % {"dir": str(tmp_path)})
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    config = tmp_path / ".config" / "xwpe"
    config.mkdir(parents=True)
    (config / "xwperc").write_text("[General]\nPrintCmd : %s\n" % stub)
    return tmp_path


# @scenario SC-screen-08
@pytest.mark.parametrize("xwpe", [{"XWPE_UI_TRACE": TRACE}], indirect=True)
def test_esc_on_print_says_it_stopped_and_shows_no_not_installed_box(slow_print, xwpe):
    """File > Print File, answer Yes, the print command is slow, Esc: the
    editor says it stopped waiting and does not claim the command is not
    installed."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.menu("f", "p", delay=0.4)
    xwpe.key("y", delay=0.2)
    pid_file = slow_print / "print.pid"
    assert _wait(pid_file.exists, WAIT_STARTS_S), "print never ran the command"
    pid = int(pid_file.read_text().strip())
    assert _alive(pid), "the command ended by itself: the row proves nothing"
    trace_before = _trace(xwpe)
    xwpe.key("Escape", delay=0.2)
    assert _wait(lambda: not _alive(pid), ENDED_S), \
        "Esc should end the command the editor waits for"
    _assert_cancelled_quietly(xwpe, before, trace_before)


