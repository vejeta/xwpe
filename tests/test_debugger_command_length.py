"""The commands the editor writes to a debugger hold the longest path the
system allows.

A debugger starts in an empty folder of its own, and the editor's first
commands move it to the program's folder and load the program by its path.
Those commands used to be cut or refused at 255 bytes: a folder with one
name of the longest length a file system takes, a folder named in a language
whose letters take several bytes (pdb writes each byte over ASCII as four),
a very deep folder, and a source file with a very long name each broke a
different debugger in a different way.  Now one number, the request engine's,
is the longest a command to a measured debugger may be, and every command
the editor builds for gdb, pdb and jdb fits it.

Each row makes the debugger run in the folder (the program, or the script,
writes the working folder it finds), or checks what the editor sent.  Each
route has a canary: the real debugger started directly in that folder must
do what the row expects of it, or the row is skipped naming the broken
probe, so a failure means the editor's command and not a dead probe.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, tool_usable
from test_startplan_debug import CTRL_G, RAN_WAIT, RUN_IT, SETTLE, Folder, _screen, _wait
from test_startplan_pdb_jdb_apart import (DOWN, JAVA_INI_LINE, JdbFolder, _break_in_the_script,
                                          _clean_env, _jdb_canary, _run_on, _start_jdb,
                                          _stopped, _traced_sends)

needs_gdb = pytest.mark.skipif(not (tool_usable("gcc", "--version") and
                                    tool_usable("gdb", "--version")),
                               reason="gcc and gdb not usable")
needs_pdb = pytest.mark.skipif(not tool_usable("python3", "--version"),
                               reason="python3 not usable")
needs_jdb = pytest.mark.skipif(not (tool_usable("javac", "-version") and
                                    tool_usable("jdb", "-version")),
                               reason="javac and jdb not usable")

INTERFACES = ["cli", "mi"]
CANARY_S = 40              # a direct debugger run is far inside this
STOP_S = 25                # a build, the debugger's start and its first stop
NAME_BYTES = 255           # the longest name a file system takes
DEEP_NAME = "d" * 200      # one level of a deep folder
DEEP_TOTAL = 3800          # about how long the deep folder is, in bytes
BOX_TOTAL = 700            # a folder the question box can still show whole

# A program that writes the folder it runs in.
CWD_C = ('#include <stdio.h>\n#include <unistd.h>\n'
         'int main(void)\n{\n char b[8192];\n'
         ' FILE *f = getcwd(b, sizeof b) ? fopen("%s", "w") : 0;\n'
         ' if (f) { fputs(b, f); fclose(f); }\n return 0;\n}\n')
CWD_PY = "import os\nopen(%r, 'w').write(os.getcwd())\n"
CJK_NAME = "\u4e2d" * 20          # 20 letters of 3 bytes each


def _mkdirs(tmp_path, names):
    """Make the folders @names one inside the other under @tmp_path; return
    the deepest."""
    path = tmp_path
    for name in names:
        path = path / name
        path.mkdir()
    return path


def _deep_names(tmp_path, total=DEEP_TOTAL):
    """Names that make a folder of about @total bytes under @tmp_path."""
    count = (total - len(str(tmp_path))) // (len(DEEP_NAME) + 1)
    return [DEEP_NAME] * count


class LongFolder(Folder):
    """A start folder that is @ws (made by the row), and a HOME of the user's."""

    def __init__(self, tmp_path, ws):
        super().__init__(tmp_path)
        self.ws = ws
        self.ran = tmp_path / "ran"
        self.cwd_file = tmp_path / "cwd"

    def env(self, interface=None):
        env = {"HOME": str(self.home), "XWPE_UI_TRACE": str(self.trace)}
        if interface == "mi":
            env["XWPE_GDB_INTERFACE"] = "mi"
        return env

    def session(self, seed, filename, interface=None):
        return WpeSession(str(self.ws), seed, filename=filename, env_extra=self.env(interface))


def _gdb_canary(folder, seed):
    """The real gdb, started directly in the folder, runs the program there."""
    (folder.ws / "t.c").write_text(seed)
    subprocess.run(["gcc", "-g", "-o", str(folder.ws / "t.e"), str(folder.ws / "t.c")],
                   check=True, timeout=CANARY_S)
    env = _clean_env(folder)
    subprocess.run(["gdb", "-q", "-batch", "-ex", "run", str(folder.ws / "t.e")],
                   cwd=str(folder.ws), env=env, timeout=CANARY_S,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = folder.cwd_file.exists() and \
        folder.cwd_file.read_text() == os.path.realpath(str(folder.ws))
    for gone in (folder.cwd_file, folder.ws / "t.e"):
        if gone.exists():
            gone.unlink()
    if not ok:
        pytest.skip("canary: a gdb started directly in the folder did not run the program "
                    "there: this route's probe is dead here")


def _pdb_canary(folder, script):
    """The real pdb, started directly in the folder, runs the script there."""
    (folder.ws / "t.py").write_text(script)
    subprocess.run(["python3", "-m", "pdb", "t.py"], cwd=str(folder.ws), env=_clean_env(folder),
                   input=b"c\nq\n", timeout=CANARY_S, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    ok = folder.cwd_file.exists() and \
        folder.cwd_file.read_text() == os.path.realpath(str(folder.ws))
    if folder.cwd_file.exists():
        folder.cwd_file.unlink()
    (folder.ws / "t.py").unlink()
    if not ok:
        pytest.skip("canary: a pdb started directly in the folder did not run the script "
                    "there: this route's probe is dead here")


def _gdb_runs_in(tmp_path, ws, interface):
    """Ctrl-G R twice on a program in @ws: the debugger stops at the start,
    the program then runs on and writes the folder it found."""
    folder = LongFolder(tmp_path, ws)
    seed = CWD_C % folder.cwd_file
    _gdb_canary(folder, seed)
    with folder.session(seed, "t.c", interface) as s:
        s.key(CTRL_G, "r", delay=1.0)
        stopped = _wait(lambda: s.mark_line() is not None, STOP_S)
        screen = _screen(s)
        s.key(CTRL_G, "r", delay=1.0)
        done = _wait(folder.cwd_file.exists, RAN_WAIT * 2)
        s._drain(SETTLE)
    assert stopped, "gdb never stopped at the start:\n%s\n%s" % (folder.traced(), screen)
    assert done, "the program never reached its end:\n" + folder.traced()
    assert folder.cwd_file.read_text() == os.path.realpath(str(ws)), folder.cwd_file.read_text()
    return folder


# @scenario SC-debug-120
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
def test_gdb_starts_in_a_folder_named_with_the_longest_name(tmp_path, interface):
    """A folder with one name of 255 bytes: no command shorter than that can
    move gdb there, and the start used to be refused.  gdb now runs the
    program in it."""
    ws = _mkdirs(tmp_path, ["a" * NAME_BYTES])
    folder = _gdb_runs_in(tmp_path, ws, interface)
    cd = "cd %s" if interface == "cli" else '-environment-cd "%s"'
    assert cd % os.path.realpath(str(ws)) in _traced_sends(folder), _traced_sends(folder)


# @scenario SC-debug-121
@needs_pdb
@pytest.mark.parametrize("name", [CJK_NAME, "b" * 240], ids=["twenty-cjk-letters", "240-ascii-bytes"])
def test_pdb_starts_in_a_folder_whose_move_is_wider_than_255(tmp_path, name):
    """pdb's move writes each byte over ASCII as four: a folder of 20 CJK
    letters (60 bytes) or one of 240 ASCII bytes made a command over 255
    and was refused.  pdb now stops in the script and runs it there."""
    ws = _mkdirs(tmp_path, [name])
    folder = LongFolder(tmp_path, ws)
    script = CWD_PY % str(folder.cwd_file)
    _pdb_canary(folder, script)
    with folder.session(script, "t.py") as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        stopped = _wait(lambda: _stopped(s), STOP_S)
        screen = _screen(s)
        s.key(CTRL_G, "r", delay=1.0)
        done = _wait(folder.cwd_file.exists, RAN_WAIT * 2)
    assert stopped, "pdb never stopped at the start:\n%s\n%s" % (folder.traced(), screen)
    assert done, "the script never reached its end:\n" + folder.traced()
    assert folder.cwd_file.read_text() == os.path.realpath(str(ws)), folder.cwd_file.read_text()
    assert folder.cwd_file.read_bytes() == os.path.realpath(str(ws)).encode(), "not byte for byte"


# @scenario SC-debug-122
@needs_gdb
def test_gdb_history_is_moved_for_a_very_deep_folder(tmp_path):
    """The editor moves gdb's history file out of its own folder, into the
    program's.  For a folder over about 220 bytes the command did not fit and
    the move was skipped without a word, so a user who keeps a history lost
    it with the session's folder.  The command is now sent for a folder of
    3.8 KB."""
    ws = _mkdirs(tmp_path, _deep_names(tmp_path))
    folder = LongFolder(tmp_path, ws)
    (folder.home / ".gdbinit").write_text("set history save on\n")
    seed = CWD_C % folder.cwd_file
    with folder.session(seed, "t.c") as s:
        s.key(CTRL_G, "r", delay=1.0)
        assert _wait(lambda: s.mark_line() is not None, STOP_S), \
            "gdb never stopped at the start:\n%s\n%s" % (folder.traced(), _screen(s))
    real = os.path.realpath(str(ws))
    sends = _traced_sends(folder)
    assert "show history filename" in sends, sends
    assert "set history filename %s/.gdb_history" % real in sends, \
        "the history was not moved to the program's folder:\n%s" % [x[:80] for x in sends]
    assert "cd %s" % real in sends, "one cd of the whole folder: %s" % [x[:80] for x in sends]


# @scenario SC-debug-123
@needs_jdb
def test_jdb_run_it_is_offered_for_a_deep_folder(tmp_path):
    """A jdb.ini in a folder of 700 bytes (the longest the question box shows
    whole): the editor's read command was longer than 255 and Run it was not
    offered.  It is now, and the breakpoint the file names takes."""
    ws = _mkdirs(tmp_path, _deep_names(tmp_path, BOX_TOTAL))
    folder = JdbFolder(tmp_path)
    folder.ws = ws
    title = folder.folder_ini()
    _jdb_canary(folder)
    with folder.session() as s:
        _start_jdb(s, folder, title, RUN_IT)
        _run_on(s)
        reached = _wait(lambda: s.mark_line() == JAVA_INI_LINE, STOP_S)
        screen = _screen(s)
    assert reached, "Run it did not load the listed jdb.ini:\n%s\n%s" % (folder.traced(), screen)
    reads = [x for x in _traced_sends(folder) if x.startswith("read ")]
    assert reads and len(reads[0]) > 255, reads


# @scenario SC-debug-124
@needs_pdb
def test_pdb_a_breakpoint_in_a_script_under_a_very_deep_folder_stops_there(tmp_path):
    """The editor names the script by its full path in pdb's break command,
    however deep the folder is: the breakpoint is set before the start and
    pdb stops on it."""
    ws = _mkdirs(tmp_path, _deep_names(tmp_path))
    folder = LongFolder(tmp_path, ws)
    script = CWD_PY % str(folder.cwd_file)
    _pdb_canary(folder, script)
    with folder.session(script, "t.py") as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        stopped = _wait(lambda: _stopped(s), STOP_S)
        line = s.mark_line()
        screen = _screen(s)
    assert stopped, "pdb never stopped on the breakpoint:\n%s\n%s" % (folder.traced(), screen)
    assert line == 2, line


# A script whose fourth line is reached by Go to cursor from a stop on the
# second.
GOTO_PY = "import os\nx = 1\ny = 2\nopen(%r, 'w').write(os.getcwd())\n"
GOTO_LINE = 4


# @scenario SC-debug-129
@needs_pdb
def test_pdb_go_to_cursor_in_a_script_under_a_very_deep_folder_stops_there(tmp_path):
    """Go to cursor names the script by its full path in the breakpoint it
    sets for itself, however deep the folder is: from a stop on line 2,
    Ctrl-G U on line 4 stops there."""
    ws = _mkdirs(tmp_path, _deep_names(tmp_path))
    folder = LongFolder(tmp_path, ws)
    script = GOTO_PY % str(folder.cwd_file)
    _pdb_canary(folder, script)
    with folder.session(script, "t.py") as s:
        _break_in_the_script(s)
        s.key(CTRL_G, "r", delay=1.0)
        stopped = _wait(lambda: _stopped(s), STOP_S)
        s.key(DOWN, DOWN, delay=0.2)
        s.key(CTRL_G, "u", delay=1.0)
        reached = _wait(lambda: s.mark_line() == GOTO_LINE, STOP_S)
        line = s.mark_line()
        screen = _screen(s)
    assert stopped, "pdb never stopped on the breakpoint:\n%s\n%s" % (folder.traced(), screen)
    assert reached, "Go to cursor did not stop on the cursor's line (at %s):\n%s" % (line, screen)


F9 = "\033[20~"                     # Make: compile and link
STEM_BYTES = [80, 200, 250]        # a source's name before ".c": the link used to cut it at 79
HELLO_C = '#include <stdio.h>\nint main(void)\n{\n puts("linked");\n return 0;\n}\n'
BUILD_WAIT = 30                    # a compile and a link of a small file


def _program_prints(path):
    """Run the program the build made and return what it printed."""
    done = subprocess.run([str(path)], capture_output=True, text=True, timeout=CANARY_S)
    return done.stdout


# @scenario SC-debug-130
@pytest.mark.skipif(not tool_usable("gcc", "--version"), reason="gcc not usable")
@pytest.mark.parametrize("stem", STEM_BYTES, ids=["%d-byte-stem" % n for n in STEM_BYTES])
def test_a_source_with_a_long_name_builds_and_the_program_runs(tmp_path, stem):
    """The link names the program from the source's name without its
    extension, as the build's plan does.  The link kept that name in 80
    bytes, so a source over 79 bytes was compiled and then refused at the
    link ("a word the plan did not show").  Both now take the one name the
    plan judged: F9 makes the program and it runs."""
    folder = LongFolder(tmp_path, tmp_path / "ws")
    name = "s" * stem
    program = folder.ws / (name + ".e")
    with folder.session(HELLO_C, name + ".c") as s:
        s.key(F9, delay=1.0)
        made = _wait(program.exists, BUILD_WAIT)
        screen = _screen(s)
    assert made, "F9 made no program for a %d-byte name:\n%s\n%s" % (stem, folder.traced(), screen)
    assert _program_prints(program) == "linked\n"


BRK_C = ('int main(void)\n{\n int x = 1;\n x = x + 1;\n return x - 2;\n}\n')
BRK_LINE = 5                       # the line the breakpoint is set on
LONG_SOURCE_STEM = 250             # a source name of 252 bytes, as long as a name can be with ".c"


# @scenario SC-debug-131
@needs_gdb
@pytest.mark.parametrize("interface", INTERFACES)
@pytest.mark.parametrize("stem", [10, LONG_SOURCE_STEM], ids=["short-name-control", "250-byte-name"])
def test_gdb_a_breakpoint_in_a_source_with_a_long_name_stops_there(tmp_path, stem, interface):
    """gdb names a breakpoint by the source's file name, so a source whose
    name is 252 bytes makes a break command over 255 (a console command or
    an MI one).  The build refused such a name at the link, and the
    engine's old limit would have cut the command; both now take it whole.
    The breakpoint is set before the start and the program stops on its line
    instead of running to its end.  The short name is the control: the same
    keys with a name that never made a long command."""
    ws = tmp_path / "ws"
    ws.mkdir()
    folder = LongFolder(tmp_path, ws)
    name = "b" * stem + ".c"
    with folder.session(BRK_C, name, interface) as s:
        s.key(DOWN, DOWN, DOWN, DOWN, delay=0.1)
        s.key(CTRL_G, "b", delay=0.4)
        s.key(CTRL_G, "r", delay=1.0)
        reached = _wait(lambda: s.mark_line() == BRK_LINE, STOP_S)
        line = s.mark_line()
        screen = _screen(s)
    assert reached, ("the program did not stop on the breakpoint (at %s):\n%s\n%s" %
                     (line, folder.traced(), screen))

