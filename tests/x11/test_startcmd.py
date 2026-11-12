"""A program a project folder names runs only after a yes, on X11.

The question is the same box on every display: the terminal, X11 and
Wayland draw it through the one dialog code they share.  These rows drive
it in the graphical editor: a start-folder xwperc names ./fake-cc as the C
compiler and F9 asks before it runs; a remembered yes runs it with no box;
a print command holding an escape sequence is shown written out.

The editor's trace (XWPE_UI_TRACE) says which question it asked and how it
was answered; the pixels say a box was drawn.  The Wayland twin
(tests/wayland/test_startcmd.py) runs this same body.  The terminal rows
are in tests/test_startcmd.py.
"""
import os
import stat
import time

import pytest

from conftest import _sleep, changed_pixels

TRACE = "ui.trace"
# The trace, and the state folder under the session's HOME (an empty value
# is unset to the editor), so the remembered answers are this test's own.
SESSION = {"XWPE_UI_TRACE": TRACE, "XDG_STATE_HOME": ""}
ASK_WAIT_S = 8             # F9 has reached the question by then
RAN_WAIT_S = 10            # the folder's compiler has run by then
SETTLE_S = 1.5             # the box is drawn, or gone, by then
BOX_PX_MIN = 2000          # a box is thousands of changed pixels

FAKE = '''#!/bin/sh
touch "%(marker)s"
exit 1
'''


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


def _fake(tmp_path):
    fake = tmp_path / "fake-cc"
    fake.write_text(FAKE % {"marker": tmp_path / "ran-fake"})
    fake.chmod(fake.stat().st_mode | stat.S_IXUSR)
    return fake


@pytest.fixture
def folder_compiler(tmp_path):
    """The start folder's xwperc names ./fake-cc as the C compiler."""
    _fake(tmp_path)
    (tmp_path / "xwperc").write_text("[Language/C]\nCompiler : ./fake-cc\n")
    return tmp_path


# The plan F9 judges for t.c in that folder, as the store keeps it
# (version 2: the build's effect, its lines escaped onto one).
PLAN_TEXT = ("xwpe-start 2\\nkind build\\ncwd %(cwd)s\\nargv ./fake-cc\\nargv -g"
             "\\nargv -c\\nargv -o")


@pytest.fixture
def remembered_compiler(folder_compiler):
    """... and the user said Run it for that very plan in this folder."""
    store = folder_compiler / ".local" / "state" / "xwpe" / "start-commands"
    store.parent.mkdir(parents=True)
    real = os.path.realpath(str(folder_compiler))
    store.write_text("xwpe-start-commands 2\nyes\tbuild/C\t%s\t%s\n"
                     % (real, PLAN_TEXT % {"cwd": real}))
    return folder_compiler


@pytest.fixture
def folder_print_escape(tmp_path):
    """The start folder's xwperc names a print command that clears a terminal."""
    (tmp_path / "xwperc").write_text("[General]\nPrintCmd : echo \x1b[2J\n")
    return tmp_path


# @scenario SC-compile-28
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_startcmd_compiler_asks(folder_compiler, xwpe):
    """F9: the box is drawn, naming the folder's command; Esc runs nothing.
    (The answer's key names the action, build/C: F9's compile and link are
    one plan.)"""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.key("F9", delay=0.4)
    asked = "startcmd ask build/C ./fake-cc -g"
    assert _wait(lambda: asked in _trace(folder_compiler), ASK_WAIT_S), _trace(folder_compiler)
    _sleep(SETTLE_S)
    drawn = changed_pixels(before, xwpe.screenshot())
    xwpe.key("Escape", delay=0.4)
    assert _wait(lambda: "startcmd answer build/C cancel" in _trace(folder_compiler),
                 ASK_WAIT_S), _trace(folder_compiler)
    assert drawn >= BOX_PX_MIN, "the question was not drawn (%d pixels changed)" % drawn
    assert not (folder_compiler / "ran-fake").exists(), "the folder's compiler ran after Esc"
    assert xwpe.proc.poll() is None, "xwpe died answering the question"


# @scenario SC-compile-28
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_startcmd_remembered(remembered_compiler, xwpe):
    """A remembered yes: F9 runs the folder's compiler with no question."""
    xwpe.key("F9", delay=0.4)
    assert _wait((remembered_compiler / "ran-fake").exists, RAN_WAIT_S), \
        "the remembered command did not run:\n" + _trace(remembered_compiler)
    said = _trace(remembered_compiler)
    assert "startcmd ask" not in said, said
    assert "startcmd run build/C" in said, said


# @scenario SC-compile-28
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_startcmd_escape_shown(folder_print_escape, xwpe):
    """File > Print File, Yes: the box shows the escape sequence written out."""
    xwpe.menu("f", "p", delay=0.4)
    xwpe.key("y", delay=0.4)
    asked = "startcmd ask PrintCmd echo \\x1b[2J"
    assert _wait(lambda: asked in _trace(folder_print_escape), ASK_WAIT_S), \
        _trace(folder_print_escape)
    xwpe.key("Escape", delay=0.4)
    assert _wait(lambda: "startcmd answer PrintCmd cancel" in _trace(folder_print_escape),
                 ASK_WAIT_S), _trace(folder_print_escape)
    assert xwpe.proc.poll() is None, "xwpe died answering the question"
