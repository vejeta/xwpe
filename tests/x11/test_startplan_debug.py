"""A debug session's start is shown first when the folder decided it, on X11.

The box is the one dialog the terminal, X11 and Wayland share.  The start
folder's xwperc sets Arguments that end gdb's run command and start a shell
command of their own: Ctrl-G R draws the box with the run command they make
before gdb starts, and Escape starts nothing.  A debugger the folder names
is asked about in that same one box with the .pdbrc it would run, and Use
mine on a script gdb would load beside the program, or from inside it,
starts gdb with none of the folder's files, and Run it makes the editor load
the script, both through the graphical editor's helper script; a library's
script, which no box lists, never runs.  The Wayland twin
(tests/wayland/test_startplan_debug.py) runs this same body; the terminal
rows, start-up files among them, are in tests/test_startplan_debug.py.
"""
import os
import shutil
import subprocess
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
CANARY_S = 40              # a direct gdb run is far inside this


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
    assert "startplan line With Run it: ./.pdbrc (1 line)" in said, said
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
    through the graphical editor's own helper, with none of the folder's
    files loaded: the session starts and the script never runs."""
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
    assert "startplan line With Run it: ./t.e-gdb.py (1 line)" in said, said
    assert not (program_script / "ran").exists(), "gdb ran the program's script after Use mine"
    assert xwpe.proc.poll() is None, "xwpe died starting gdb"


# @scenario SC-debug-116
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("gdb")),
                    reason="gcc and gdb not on PATH")
@pytest.mark.parametrize("xwpe", [SUB_SESSION], indirect=True)
def test_startplan_program_gdb_script_run_it_loads_it(program_script, xwpe):
    """Run it on that box: the graphical editor's helper script starts gdb
    apart from the folder, and the editor's own command loads the script."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startcmd ask debug/gdb" in _trace(program_script), ASK_WAIT_S), \
        _trace(program_script)
    _sleep(SETTLE_S)
    xwpe.key("alt+r", delay=0.4)
    assert _wait(lambda: (program_script / "ran").exists(), ASK_WAIT_S), \
        "Run it did not load the program's script:\n" + _trace(program_script)
    said = _trace(program_script)
    assert "startplan send set auto-load safe-path $debugdir:$datadir/auto-load" in said, said
    assert "startplan contain gdb apart=1" in said, said
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
    starts gdb, through the graphical editor's own helper, with none of the
    folder's files loaded: the session starts and the script never runs."""
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
    assert "startplan line With Run it: ./t.e (.debug_gdb_scripts) (2 lines)" in said, said
    assert not (embedded_script / "ran").exists(), \
        "gdb ran the program's own script after Use mine"
    assert xwpe.proc.poll() is None, "xwpe died starting gdb"


# A program that loads a shared library of its folder, which has a script
# beside it: nothing the box could list.
LIBRARY_SOURCE = ('#include <dlfcn.h>\n'
                  'int main(void){void *h = dlopen("./libx.so", RTLD_NOW); return h ? 0 : 1;}\n')
LIBRARY_SESSION = dict(SESSION, XWPE_TEST_FILE=SUB_C, XWPE_TEST_SOURCE=LIBRARY_SOURCE)
HOME_RAN = "home-ran"


@pytest.fixture
def library_script(tmp_path):
    """The user's gdb trusts every folder and runs a command of its own; the
    program's folder holds libx.so with libx.so-gdb.py beside it, which
    touches tmp_path/ran.  The canary: gdb started directly there fires it."""
    _subfolder(tmp_path)
    sub = tmp_path / "sub"
    (tmp_path / ".gdbinit").write_text("set auto-load safe-path /\nshell touch %s\n"
                                       % (tmp_path / HOME_RAN))
    (sub / "libx.c").write_text("int lib_f(void){return 1;}\n")
    (sub / "libx.so-gdb.py").write_text("open(%r, 'w').close()\n" % str(tmp_path / "ran"))
    subprocess.run(["gcc", "-g", "-shared", "-fPIC", "-o", str(sub / "libx.so"),
                    str(sub / "libx.c")], check=True, timeout=CANARY_S)
    (sub / "canary.c").write_text(LIBRARY_SOURCE)
    subprocess.run(["gcc", "-g", "-o", str(sub / "canary.e"), str(sub / "canary.c")],
                   check=True, timeout=CANARY_S)
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(tmp_path), "TERM": "dumb"}
    subprocess.run(["gdb", "-q", "-batch", "-ex", "run", str(sub / "canary.e")], cwd=str(sub),
                   env=env, timeout=CANARY_S, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    fired = (tmp_path / "ran").exists() and (tmp_path / HOME_RAN).exists()
    for name in ("ran", HOME_RAN):
        if (tmp_path / name).exists():
            (tmp_path / name).unlink()
    if not fired:
        pytest.skip("canary: gdb started directly in the folder did not fire the library's "
                    "script: this route's probe is dead here")
    return tmp_path


# @scenario SC-debug-116
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("gdb")),
                    reason="gcc and gdb not on PATH")
@pytest.mark.parametrize("xwpe", [LIBRARY_SESSION], indirect=True)
def test_startplan_a_library_script_is_not_run_by_a_clean_start(library_script, xwpe):
    """Nothing is listed, so no box opens; the program loads a library of the
    folder with a script beside it.  gdb, started apart through the graphical
    editor's helper script, has its default auto-load trust, so the script
    never runs, and the user's own ~/.gdbinit does."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startplan judge debug/gdb" in _trace(library_script), ASK_WAIT_S), \
        _trace(library_script)
    _sleep(RAN_WAIT_S * 2)
    xwpe.key("ctrl+g", "r", delay=0.6)
    _sleep(RAN_WAIT_S * 2)
    said = _trace(library_script)
    assert "startcmd ask" not in said, said
    assert (library_script / HOME_RAN).exists(), "the user's own ~/.gdbinit did not run:\n" + said
    assert not (library_script / "ran").exists(), "gdb ran the library's script"
    assert xwpe.proc.poll() is None, "xwpe died starting gdb"


# A Python program edited in the subfolder, whose folder holds a .pdbrc that
# touches tmp_path/ran.  pdb is started apart, through the graphical editor's
# helper script, like gdb.
PY_SESSION = dict(SESSION, XWPE_TEST_FILE="sub/t.py", XWPE_TEST_SOURCE="x = 1\nprint(x)\n")


@pytest.fixture
def pdb_startup_file(tmp_path):
    """The program's folder holds a .pdbrc that touches tmp_path/ran.  The
    canary: pdb started directly there, with the session's HOME, runs it."""
    _subfolder(tmp_path)
    sub = tmp_path / "sub"
    (sub / ".pdbrc").write_text("!open(%r, 'w').close()\n" % str(tmp_path / "ran"))
    (sub / "canary.py").write_text("x = 1\n")
    env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(tmp_path), "TERM": "dumb"}
    subprocess.run(["python3", "-m", "pdb", "canary.py"], cwd=str(sub), env=env, input=b"q\n",
                   timeout=CANARY_S, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    fired = (tmp_path / "ran").exists()
    if fired:
        (tmp_path / "ran").unlink()
    (sub / "canary.py").unlink()
    if not fired:
        pytest.skip("canary: pdb started directly in the folder did not run its .pdbrc: "
                    "this route's probe is dead here")
    return tmp_path


# @scenario SC-debug-119
@pytest.mark.skipif(not shutil.which("python3"), reason="python3 not on PATH")
@pytest.mark.parametrize("xwpe", [PY_SESSION], indirect=True)
def test_startplan_pdbrc_use_mine_stops_it(pdb_startup_file, xwpe):
    """Ctrl-G R lists the folder's .pdbrc; Use mine starts pdb, through the
    graphical editor's own helper, apart from the folder: the session starts
    and the file never runs."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startcmd ask debug/pdb" in _trace(pdb_startup_file), ASK_WAIT_S), \
        _trace(pdb_startup_file)
    _sleep(SETTLE_S)
    xwpe.key("alt+u", delay=0.4)
    assert _wait(lambda: "startcmd answer debug/pdb own" in _trace(pdb_startup_file),
                 ASK_WAIT_S), _trace(pdb_startup_file)
    _sleep(RAN_WAIT_S)
    said = _trace(pdb_startup_file)
    assert "startplan line With Run it: ./.pdbrc (1 line)" in said, said
    assert "startplan contain pdb apart=1" in said, said
    assert not (pdb_startup_file / "ran").exists(), "pdb ran the folder's .pdbrc after Use mine"
    assert xwpe.proc.poll() is None, "xwpe died starting pdb"


# @scenario SC-debug-119
@pytest.mark.skipif(not shutil.which("python3"), reason="python3 not on PATH")
@pytest.mark.parametrize("xwpe", [PY_SESSION], indirect=True)
def test_startplan_pdbrc_run_it_loads_it(pdb_startup_file, xwpe):
    """Run it on that box: pdb starts apart through the helper script, the
    editor moves it into the program's folder, and sends the .pdbrc's line."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: "startcmd ask debug/pdb" in _trace(pdb_startup_file), ASK_WAIT_S), \
        _trace(pdb_startup_file)
    _sleep(SETTLE_S)
    xwpe.key("alt+r", delay=0.4)
    assert _wait(lambda: (pdb_startup_file / "ran").exists(), ASK_WAIT_S), \
        "Run it did not load the folder's .pdbrc:\n" + _trace(pdb_startup_file)
    said = _trace(pdb_startup_file)
    assert "startplan send !__import__('os').chdir(b'" in said, said
    assert "startplan contain pdb apart=1" in said, said
    assert xwpe.proc.poll() is None, "xwpe died starting pdb"
