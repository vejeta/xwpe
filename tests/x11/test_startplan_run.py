"""Run and Make start from a start plan on X11.

A graphical editor runs a shell script (Ctrl-F9) in a terminal of its own;
the original built that terminal's command line as shell text with the
script's name in it, so a script named "a;touch pwned;.sh" also ran "touch
pwned".  The name now reaches the terminal as one word.  And the box that
asks before make gets a folder's Arguments is the dialog the terminal, X11
and Wayland share: Alt-A draws it, Escape runs nothing.  The Wayland twin
(tests/wayland/test_startplan_run.py) runs these same bodies; the terminal
rows are in tests/test_startplan_run.py.
"""
import shutil
import time

import pytest

from conftest import _sleep, changed_pixels

TRACE = "ui.trace"
SESSION = {"XWPE_UI_TRACE": TRACE, "XDG_STATE_HOME": ""}
RUN_WAIT_S = 20            # the terminal is up and the script has run by then
ASK_WAIT_S = 20            # the question is up by then
SETTLE_S = 1.5             # the box is drawn, or gone, by then
BOX_PX_MIN = 2000          # a box is thousands of changed pixels

SCRIPT_NAME = "a;touch pwned;.sh"
SCRIPT = "#!/bin/sh\necho ran > executed.out\n"


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


# @scenario SC-compile-30
@pytest.mark.skipif(not shutil.which("xterm"), reason="no xterm")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": SCRIPT, "XWPE_TEST_FILE": SCRIPT_NAME,
                                   **SESSION}], indirect=True)
def test_startplan_script_named_like_a_command_runs_alone(xwpe, tmp_path):
    """Ctrl-F9 on "a;touch pwned;.sh": the script runs in its terminal, and
    nothing in its name does."""
    xwpe.key("ctrl+F9", delay=0.5)
    ran = _wait(lambda: (tmp_path / "executed.out").exists(), RUN_WAIT_S)
    _sleep(SETTLE_S)
    assert not (tmp_path / "pwned").exists(), "the script's name ran as shell text"
    assert ran, "the script did not run"
    assert xwpe.proc.poll() is None, "xwpe died running the script"


# A login shell that, like csh and tcsh, cannot run a script whose command is
# "$@": it refuses such a script and starts nothing.  Any other script it
# hands to sh.
CSH_LIKE_SHELL = ('#!/bin/sh\n'
                  'if grep -q \'"[$]@"\' "$1" 2>/dev/null; then\n'
                  '  echo "Illegal variable name." >&2\n'
                  '  exit 1\n'
                  'fi\n'
                  'exec /bin/sh "$@"\n')


@pytest.fixture
def csh_like_login_shell(tmp_path, monkeypatch):
    """$SHELL is a shell that cannot run "$@", as the editor starts."""
    stub = tmp_path / "csh-like"
    stub.write_text(CSH_LIKE_SHELL)
    stub.chmod(0o755)
    monkeypatch.setenv("SHELL", str(stub))
    return stub


# @scenario SC-compile-30
@pytest.mark.skipif(not shutil.which("xterm"), reason="no xterm")
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": SCRIPT, "XWPE_TEST_FILE": "t.sh",
                                   **SESSION}], indirect=True)
def test_startplan_run_in_a_terminal_under_a_csh_login_shell(csh_like_login_shell, xwpe,
                                                             tmp_path):
    """Ctrl-F9 on a script, the user's login shell a csh: the script runs
    in its terminal.  The terminal's helper was started by $SHELL, which
    cannot run its "$@", so the terminal closed with nothing run."""
    xwpe.key("ctrl+F9", delay=0.5)
    ran = _wait(lambda: (tmp_path / "executed.out").exists(), RUN_WAIT_S)
    _sleep(SETTLE_S)
    assert ran, "the script did not run under a csh login shell"
    assert xwpe.proc.poll() is None, "xwpe died running the script"


MAKEFILE = "all:\n\t@echo made > made.out\n"


@pytest.fixture
def folder_make_arguments(tmp_path):
    """The start folder's xwperc hands make an --eval that touches ran."""
    (tmp_path / "Makefile").write_text(MAKEFILE)
    (tmp_path / "xwperc").write_text(
        "[Programming]\nArguments : --eval=x:=$(shell\ttouch\tran)\n")
    return tmp_path


# @scenario SC-compile-30
@pytest.mark.skipif(not shutil.which("make"), reason="make not on PATH")
@pytest.mark.parametrize("xwpe", [SESSION], indirect=True)
def test_startplan_make_arguments_from_the_folder_ask(folder_make_arguments, xwpe):
    """Alt-A: the box is drawn, with the folder's Arguments and the line
    that make runs the folder's own Makefile; Escape runs nothing."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.key("alt+a", delay=0.6)
    assert _wait(lambda: "startcmd ask make/make" in _trace(folder_make_arguments),
                 ASK_WAIT_S), _trace(folder_make_arguments)
    _sleep(SETTLE_S)
    drawn = changed_pixels(before, xwpe.screenshot())
    xwpe.key("Escape", delay=0.4)
    assert _wait(lambda: "startcmd answer make/make cancel" in _trace(folder_make_arguments),
                 ASK_WAIT_S), _trace(folder_make_arguments)
    said = _trace(folder_make_arguments)
    assert "startplan line This runs the folder's own Makefile." in said, said
    assert drawn >= BOX_PX_MIN, "the question was not drawn (%d pixels changed)" % drawn
    _sleep(SETTLE_S)
    assert not (folder_make_arguments / "ran").exists(), "make ran the folder's --eval"
    assert not (folder_make_arguments / "made.out").exists(), "make ran after Escape"
    assert xwpe.proc.poll() is None, "xwpe died answering the question"
