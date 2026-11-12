"""A debug session's start is shown first when the folder decided it, on X11.

The box is the one dialog the terminal, X11 and Wayland share.  The start
folder's xwperc sets Arguments that end gdb's run command and start a shell
command of their own: Ctrl-G R draws the box with the run command they make
before gdb starts, and Escape starts nothing.  The Wayland twin
(tests/wayland/test_startplan_debug.py) runs this same body; the terminal
rows, start-up files among them, are in tests/test_startplan_debug.py.
"""
import shutil
import time

import pytest

from conftest import _sleep, changed_pixels

TRACE = "ui.trace"
# The trace, and the state folder under the session's HOME, so the
# remembered answers are this test's own.
SESSION = {"XWPE_UI_TRACE": TRACE, "XDG_STATE_HOME": ""}
ASK_WAIT_S = 30            # the build is done and the question up by then
SETTLE_S = 1.5             # the box is drawn, or gone, by then
RAN_WAIT_S = 4             # gdb would have run the program by then
BOX_PX_MIN = 2000          # a box is thousands of changed pixels


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.2)
    return predicate()


def _trace(tmp_path):
    try:
        return (tmp_path / TRACE).read_text()
    except OSError:
        return ""


@pytest.fixture
def folder_arguments(tmp_path):
    """The start folder's xwperc sets Arguments that touch tmp_path/ran."""
    (tmp_path / "xwperc").write_text("[Programming]\nArguments : x ; touch %s\n"
                                     % (tmp_path / "ran"))
    return tmp_path


# @scenario SC-compile-29
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("gdb")),
                    reason="gcc and gdb not on PATH")
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_startplan_folder_arguments_reach_gdb_only_after_the_box(folder_arguments, xwpe):
    """Ctrl-G R: the box is drawn, with the run command the folder's
    Arguments make; Escape runs nothing."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.key("ctrl+g", "r", delay=0.6)
    asked = "startcmd ask debug/gdb"
    assert _wait(lambda: asked in _trace(folder_arguments), ASK_WAIT_S), _trace(folder_arguments)
    _sleep(SETTLE_S)
    drawn = changed_pixels(before, xwpe.screenshot())
    xwpe.key("Escape", delay=0.4)
    assert _wait(lambda: "startcmd answer debug/gdb cancel" in _trace(folder_arguments),
                 ASK_WAIT_S), _trace(folder_arguments)
    said = _trace(folder_arguments)
    assert "startplan line Then runs: r x ; touch %s" % (folder_arguments / "ran") in said, said
    assert drawn >= BOX_PX_MIN, "the question was not drawn (%d pixels changed)" % drawn
    _sleep(RAN_WAIT_S)
    assert not (folder_arguments / "ran").exists(), "the folder's Arguments ran after Escape"
    assert xwpe.proc.poll() is None, "xwpe died answering the question"
