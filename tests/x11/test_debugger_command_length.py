"""The commands the editor writes to a debugger hold the longest path the
system allows, on X11.

The graphical editor starts the debugger through a helper script that reads
its commands from a named pipe, so the same commands as in the terminal
(tests/test_debugger_command_length.py) reach it by another road.  A program
in a folder with one name of the longest length a file system takes (255
bytes) cannot be reached by any command shorter than that, and a folder of 20
CJK letters is a move pdb's literal writes at four bytes per byte; both were
refused at 255.  The editor's trace carries every command it sent, whole,
so the row reads what the editor wrote and that the start was not refused.
The Wayland twin runs this same body.
"""
import os
import shutil
import time

import pytest

from conftest import _sleep

TRACE = "ui.trace"
SESSION = {"XWPE_UI_TRACE": TRACE, "XDG_STATE_HOME": ""}
SENT_WAIT_S = 40           # a build, the debugger's start and its first lines
SETTLE_S = 1.5             # the editor has said whatever it was going to say
LONGEST_NAME = "a" * 255   # the longest name a file system takes
CJK_NAME = "中" * 20   # 20 letters of 3 bytes each
PDB_CHDIR = "!__import__('os').chdir(b'"
GDB_SOURCE = "int main(void){return 0;}\n"
PDB_SOURCE = "x = 1\nprint(x)\n"


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


def _sends(tmp_path):
    return [l.split("startplan send ", 1)[1] for l in _trace(tmp_path).splitlines()
            if "startplan send " in l]


def _folder(tmp_path, name):
    """The program's folder, with the trace reachable from it: the editor
    builds and debugs from the program's folder, and the trace's name is
    relative."""
    (tmp_path / name).mkdir()
    (tmp_path / name / TRACE).symlink_to(tmp_path / TRACE)
    return tmp_path / name


LONG_STEM = "s" * 200     # a source name before ".c": the link used to cut it at 79
LONG_STEM_SOURCE = "int main(void){return 0;}\n"
LONG_STEM_SESSION = dict(SESSION, XWPE_TEST_FILE=LONG_STEM + ".c",
                         XWPE_TEST_SOURCE=LONG_STEM_SOURCE)
BUILD_WAIT_S = 40         # a compile and a link of a small file

GDB_SESSION = dict(SESSION, XWPE_TEST_FILE=LONGEST_NAME + "/t.c", XWPE_TEST_SOURCE=GDB_SOURCE)
PDB_SESSION = dict(SESSION, XWPE_TEST_FILE=CJK_NAME + "/t.py", XWPE_TEST_SOURCE=PDB_SOURCE)


@pytest.fixture
def longest_name_folder(tmp_path):
    return _folder(tmp_path, LONGEST_NAME)


@pytest.fixture
def cjk_folder(tmp_path):
    return _folder(tmp_path, CJK_NAME)


# @scenario SC-debug-126
@pytest.mark.skipif(not (shutil.which("gcc") and shutil.which("gdb")),
                    reason="gcc and gdb not on PATH")
@pytest.mark.parametrize("xwpe", [GDB_SESSION], indirect=True)
def test_gdb_starts_in_a_folder_named_with_the_longest_name(longest_name_folder, tmp_path, xwpe):
    """Ctrl-G R on a program in a folder with one 255-byte name: the editor
    sends gdb one cd of the whole folder and then the program by its path,
    and the start is not refused."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    real = os.path.realpath(str(longest_name_folder))
    assert _wait(lambda: "file %s/t.e" % real in _sends(tmp_path), SENT_WAIT_S), \
        "gdb was not given the program:\n" + _trace(tmp_path)
    _sleep(SETTLE_S)
    sends = _sends(tmp_path)
    assert sends.count("cd %s" % real) == 1, sends
    assert "startplan refused" not in _trace(tmp_path), _trace(tmp_path)
    assert xwpe.proc.poll() is None, "xwpe died starting gdb"


# @scenario SC-debug-127
@pytest.mark.skipif(not (shutil.which("python3")), reason="python3 not on PATH")
@pytest.mark.parametrize("xwpe", [PDB_SESSION], indirect=True)
def test_pdb_starts_in_a_folder_named_in_cjk_letters(cjk_folder, tmp_path, xwpe):
    """Ctrl-G R on a script in a folder of 20 CJK letters: pdb's first input
    line is one chdir that writes each byte of the folder as \\xNN, 240 bytes
    for the 60 of the name, and the start is not refused."""
    _sleep(SETTLE_S)
    xwpe.key("ctrl+g", "r", delay=0.6)
    assert _wait(lambda: any(s.startswith(PDB_CHDIR) for s in _sends(tmp_path)), SENT_WAIT_S), \
        "pdb was not moved to the folder:\n" + _trace(tmp_path)
    _sleep(SETTLE_S)
    moves = [s for s in _sends(tmp_path) if s.startswith(PDB_CHDIR)]
    assert len(moves) == 1, moves
    assert moves[0].count("\\xe4\\xb8\\xad") == 20, moves[0]
    assert "startplan refused" not in _trace(tmp_path), _trace(tmp_path)
    assert xwpe.proc.poll() is None, "xwpe died starting pdb"


# @scenario SC-debug-132
@pytest.mark.skipif(not shutil.which("gcc"), reason="gcc not on PATH")
@pytest.mark.parametrize("xwpe", [LONG_STEM_SESSION], indirect=True)
def test_a_source_with_a_long_name_builds(tmp_path, xwpe):
    """F9 on a source whose name is 200 bytes: the link takes the program's
    name from the source's whole name, as the build's plan does, and makes
    the program.  The link used to keep that name in 80 bytes and refuse a
    name over 79 after compiling it."""
    _sleep(SETTLE_S)
    xwpe.key("F9", delay=0.6)
    program = tmp_path / (LONG_STEM + ".e")
    assert _wait(program.exists, BUILD_WAIT_S), \
        "F9 made no program for a 200-byte name:\n" + _trace(tmp_path)
    assert "startplan refused" not in _trace(tmp_path), _trace(tmp_path)
    assert xwpe.proc.poll() is None, "xwpe died building"
