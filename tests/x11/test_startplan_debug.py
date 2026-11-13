"""A debug session's start is shown first when the folder decided it, on X11.

The box is the one dialog the terminal, X11 and Wayland share.  The start
folder's xwperc sets Arguments that end gdb's run command and start a shell
command of their own: Ctrl-G R draws the box with the run command they make
before gdb starts, and Escape starts nothing.  A debugger the folder names
is asked about in that same one box with the .pdbrc it would run, and Use
mine on a script gdb would load beside the program, or from inside it,
starts gdb without it,
through the graphical editor's helper script.  The Wayland twin
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


# The program is edited in a subfolder: the debugger runs in the program's
# folder, and the session's HOME is the start folder, whose own files are
# the user's and never asked about.
SUB_C = "sub/t.c"
SUB_SOURCE = "int main(void){return 0;}\n"
SUB_SESSION = dict(SESSION, XWPE_TEST_FILE=SUB_C, XWPE_TEST_SOURCE=SUB_SOURCE)


def _subfolder(tmp_path):
    """The program's folder.  The trace's name is relative and the editor
    builds and debugs from the program's folder, so the trace there is the
    start folder's one."""
    (tmp_path / "sub").mkdir()
    (tmp_path / "sub" / TRACE).symlink_to(tmp_path / TRACE)


@pytest.fixture
def folder_debugger(tmp_path):
    """The start folder's xwperc names pdb; the program's folder holds a
    .pdbrc that touches tmp_path/ran."""
    _subfolder(tmp_path)
    (tmp_path / "xwperc").write_text("[Programming]\nDebugger : 5\n")
    (tmp_path / "sub" / ".pdbrc").write_text("!open('%s','w').close()\n" % (tmp_path / "ran"))
    return tmp_path


# @scenario SC-compile-29
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("python3")),
                    reason="gcc and python3 not on PATH")
@pytest.mark.parametrize("xwpe", [SUB_SESSION], indirect=True)
def test_startplan_folder_debugger_and_its_startup_file_ask_once(folder_debugger, xwpe):
    """Ctrl-G R: one box is drawn, naming the folder's debugger and the
    .pdbrc it would run together; Escape starts nothing."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startcmd ask debug/pdb" in _trace(folder_debugger), ASK_WAIT_S), \
        _trace(folder_debugger)
    _sleep(SETTLE_S)
    drawn = changed_pixels(before, xwpe.screenshot())
    xwpe.key("Escape", delay=0.4)
    assert _wait(lambda: "startcmd answer debug/pdb cancel" in _trace(folder_debugger),
                 ASK_WAIT_S), _trace(folder_debugger)
    _sleep(RAN_WAIT_S)
    said = _trace(folder_debugger)
    assert "startplan line From ./xwperc: Debugger = pdb" in said, said
    assert "startplan line Runs at start: ./.pdbrc (1 line)" in said, said
    assert said.count("startcmd ask ") == 1, said
    assert drawn >= BOX_PX_MIN, "the question was not drawn (%d pixels changed)" % drawn
    assert not (folder_debugger / "ran").exists(), "the folder's .pdbrc ran after Escape"
    assert xwpe.proc.poll() is None, "xwpe died answering the question"


@pytest.fixture
def program_script(tmp_path):
    """The user's gdb auto-loads from any folder; beside the program the
    folder holds t.e-gdb.py, which touches tmp_path/ran."""
    _subfolder(tmp_path)
    (tmp_path / ".gdbinit").write_text("set auto-load safe-path /\n")
    (tmp_path / "sub" / "t.e-gdb.py").write_text("open(%r, 'w').close()\n" % str(tmp_path / "ran"))
    return tmp_path


# @scenario SC-compile-29
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("gdb")),
                    reason="gcc and gdb not on PATH")
@pytest.mark.parametrize("xwpe", [SUB_SESSION], indirect=True)
def test_startplan_program_gdb_script_use_mine_stops_it(program_script, xwpe):
    """Ctrl-G R shows the script beside the program; Use mine starts gdb,
    through the graphical editor's own helper, told to leave it alone: the
    session starts and the script never runs."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startcmd ask debug/gdb" in _trace(program_script), ASK_WAIT_S), \
        _trace(program_script)
    _sleep(SETTLE_S)
    xwpe.key("alt+u", delay=0.4)
    assert _wait(lambda: "startcmd answer debug/gdb own" in _trace(program_script),
                 ASK_WAIT_S), _trace(program_script)
    _sleep(RAN_WAIT_S)
    said = _trace(program_script)
    assert "startplan line Runs at start: ./t.e-gdb.py (1 line)" in said, said
    assert not (program_script / "ran").exists(), "gdb ran the program's script after Use mine"
    assert xwpe.proc.poll() is None, "xwpe died starting gdb"


# The program carries a gdb script inside itself: its .debug_gdb_scripts
# section names emb.py, which gdb looks for in its working folder, the
# program's.
EMBEDDED_SOURCE = ("int main(void){return 0;}\n"
                   '__asm__(".pushsection .debug_gdb_scripts,\\"MS\\",@progbits,1\\n"\n'
                   '        ".byte 1\\n"\n'
                   '        ".asciz \\"emb.py\\"\\n"\n'
                   '        ".popsection\\n");\n')
EMBEDDED_SESSION = dict(SESSION, XWPE_TEST_FILE=SUB_C, XWPE_TEST_SOURCE=EMBEDDED_SOURCE)


@pytest.fixture
def embedded_script(tmp_path):
    """The user's gdb auto-loads from any folder; the program's folder holds
    emb.py, which touches tmp_path/ran."""
    _subfolder(tmp_path)
    (tmp_path / ".gdbinit").write_text("set auto-load safe-path /\n")
    (tmp_path / "sub" / "emb.py").write_text("open(%r, 'w').close()\n" % str(tmp_path / "ran"))
    return tmp_path


# @scenario SC-compile-29
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("gdb")),
                    reason="gcc and gdb not on PATH")
@pytest.mark.parametrize("xwpe", [EMBEDDED_SESSION], indirect=True)
def test_startplan_gdb_script_inside_the_program_use_mine_stops_it(embedded_script, xwpe):
    """Ctrl-G R lists the script the program carries inside itself; Use mine
    starts gdb, through the graphical editor's own helper, told to leave it
    alone: the session starts and the script never runs."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startcmd ask debug/gdb" in _trace(embedded_script), ASK_WAIT_S), \
        _trace(embedded_script)
    _sleep(SETTLE_S)
    xwpe.key("alt+u", delay=0.4)
    assert _wait(lambda: "startcmd answer debug/gdb own" in _trace(embedded_script),
                 ASK_WAIT_S), _trace(embedded_script)
    _sleep(RAN_WAIT_S)
    said = _trace(embedded_script)
    assert "startplan line Runs at start: ./t.e (.debug_gdb_scripts) (2 lines)" in said, said
    assert not (embedded_script / "ran").exists(), \
        "gdb ran the program's own script after Use mine"
    assert xwpe.proc.poll() is None, "xwpe died starting gdb"
