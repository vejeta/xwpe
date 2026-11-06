"""The keys reach the editor while gdb, pdb, jdb, a68g or dbx answers at a
stop (X11).

tests/stubs/gdb stands in front of the real gdb and holds back its `where`
until the test writes to its release pipe, so the editor waits for gdb's
answer at the first stop exactly as long as the test needs.  Meanwhile the
display's own event loop must hand every key to the editor: Ctrl-G K is
refused instead of being read behind the editor's back, typed text goes
into the file and File > Save writes it, all before gdb has answered.  The
terminal rows of the same scenario are in tests/test_debugger_integration.py.

The Wayland twin (tests/wayland/test_debug_answer_wait.py) runs the same
body; it skips there, as the other rows that need a debug session do (the
headless compositor harness cannot start one).
"""
import os
import shutil
import sys
import time

import pytest

from conftest import _sleep

HERE = os.path.dirname(os.path.abspath(__file__))
STUBS_DIR = os.path.join(HERE, "..", "stubs")
STUB_LOG = "gdb-stub.log"
STUB_RELEASE = "gdb-stub.release"
HOLD_TIMEOUT_S = 40
STOP_TIMEOUT_S = 20
POLL_S = 0.3
STOP_GREEN = (0, 255, 0)        # the stop line's bar
# A stop bar fills its line's cells, some 2000 pixels at the fixture's font.
# The same green is also the colour of "#include" text and of letters on the
# window's status row, about 75 pixels with no stop at all, so the bar is
# counted only well above that.
MIN_BAR_PIXELS = 400
TYPED = "MARK"
BREAK_LINE = 5
# The first column of BREAK_LINE in the source window, at the fixture's
# pinned font size: a click there brings the source back on top (the run
# put Messages there, for the program's input) and puts the cursor there.
BREAK_LINE_X = 12
BREAK_LINE_Y = 110

PROG = (
    "#include <stdio.h>\n"
    "int g = 42;\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"READY\\n\");\n"
    "  return g;\n"
    "}\n"
)
PROG_WITH_TYPED = PROG.replace("  printf", TYPED + "  printf", 1)

# The stand-in holds back gdb's line-interface commands (`where`, `set
# confirm off`), so its rows drive gdb over the line interface whatever
# XWPE_GDB_INTERFACE the run was given.
STUB_ENV = {
    "XWPE_GDB_INTERFACE": "cli",
    "PATH": os.path.abspath(STUBS_DIR) + ":" + os.environ.get("PATH", ""),
    "XWPE_STUB_REAL_GDB": shutil.which("gdb") or "",
    "XWPE_STUB_STALL": "where",
    "XWPE_STUB_STALL_NTH": "1",
    "XWPE_TEST_SOURCE": PROG,
}


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(POLL_S)
    return predicate()


def _stub_log(home):
    try:
        with open(os.path.join(home, STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


def _count(img, rgb):
    colours = img.getcolors(maxcolors=img.width * img.height) or []
    return sum(count for count, px in colours if px == rgb)


def _go_to_line(xwpe, line):
    xwpe.menu("s", "g", delay=0.6)
    xwpe.type(str(line), delay=0.4)
    xwpe.key("Return", delay=0.6)


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("gdb") or not shutil.which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [STUB_ENV], indirect=True)
def test_gdb_keys_reach_the_editor_while_it_answers(xwpe):
    """gdb holds back its answer to `where` at the first stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text and no "k", before gdb answers.  Once it answers the
    stop's green bar is drawn."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: "STALL where" in _stub_log(home), HOLD_TIMEOUT_S), \
        "the editor should ask gdb `where` at the stop:\n%s" % _stub_log(home)
    xwpe.click(BREAK_LINE_X, BREAK_LINE_Y, delay=0.6)
    xwpe.key("ctrl+g", "k", delay=0.6)
    xwpe.type(TYPED, delay=0.4)
    xwpe.save()
    assert _wait(lambda: xwpe.saved_text() == PROG_WITH_TYPED, STOP_TIMEOUT_S), \
        "keys typed while gdb answers should reach the editor, and Ctrl-G K " \
        "should type nothing (file on disk:\n%r)" % xwpe.saved_text()
    with open(os.path.join(home, STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 STOP_TIMEOUT_S), \
        "once gdb answers, the stop should be drawn as the green bar"


# Four steps in a row: F8 kept during the start has a line to land on.
STEPS_PROG = (
    "int main(void)\n"
    "{\n"
    "  int x = 1;\n"
    "  x = x + 1;\n"
    "  return x;\n"
    "}\n"
)
STEPS_BREAK_LINE = 3
SET_UP_FIRST = "set confirm off"     # the first command after gdb's banner
STEP_SENT = "CMD n\n"
STACK_ASKED = "CMD bt\n"
START_STUB_ENV = dict(STUB_ENV, XWPE_STUB_STALL=SET_UP_FIRST,
                      XWPE_TEST_SOURCE=STEPS_PROG)


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("gdb") or not shutil.which("gcc"),
                    reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", [START_STUB_ENV], indirect=True)
def test_gdb_keys_typed_during_the_start_are_kept_or_refused(xwpe):
    """gdb holds back its answer to the first command of the start.  F8
    typed meanwhile reaches the editor and is kept: once the start has
    stopped on the breakpoint it steps.  Ctrl-G K is refused, never asked
    of gdb."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, STEPS_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: ("STALL " + SET_UP_FIRST) in _stub_log(home),
                 HOLD_TIMEOUT_S), \
        "the start should ask gdb %r:\n%s" % (SET_UP_FIRST, _stub_log(home))
    xwpe.key("F8", delay=0.6)
    xwpe.key("ctrl+g", "k", delay=0.6)
    with open(os.path.join(home, STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    assert _wait(lambda: STEP_SENT in _stub_log(home), STOP_TIMEOUT_S), \
        "the F8 typed during the start should step after the first stop:\n%s" \
        % _stub_log(home)
    assert STACK_ASKED not in _stub_log(home), \
        "Ctrl-G K during the start should be refused, not asked:\n%s" \
        % _stub_log(home)


# pdb: tests/stubs/pdb/python3 stands in front of the real python3 when it
# runs pdb, holding one command as the gdb stub does.  The held command is
# the answer to a breakpoint set at a stop, so the source window, where the
# stop put the cursor, is the one the keys go to.
PDB_STUB_LOG = "pdb-stub.log"
PDB_STUB_RELEASE = "pdb-stub.release"
PDB_PROG = (
    "g = 42\n"
    "x = 1\n"
    "x = x + 1\n"
    "x = x + 1\n"
    "print(x, g)\n"
)
PDB_BREAK_LINE = 2
PDB_TOGGLE_LINE = 4
PDB_BREAK_ASKED = "b "                  # pdb's command that sets a breakpoint
PDB_STEP_SENT = "CMD n\n"
PDB_STACK_ASKED = "CMD where\n"
PDB_PROG_WITH_TYPED = "".join(
    TYPED + row if n == PDB_TOGGLE_LINE else row
    for n, row in enumerate(PDB_PROG.splitlines(True), 1))
PDB_STUB_ENV = {
    "PATH": os.path.abspath(os.path.join(STUBS_DIR, "pdb")) + ":"
            + os.environ.get("PATH", ""),
    "XWPE_STUB_REAL_PYTHON": shutil.which("python3") or "",
    "XWPE_STUB_STALL": PDB_BREAK_ASKED,
    "XWPE_STUB_STALL_NTH": "2",          # the first is the start's
    "XWPE_TEST_SOURCE": PDB_PROG,
    "XWPE_TEST_FILE": "p.py",
}


def _pdb_stub_log(home):
    try:
        with open(os.path.join(home, PDB_STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("python3"), reason="no python3")
@pytest.mark.parametrize("xwpe", [PDB_STUB_ENV], indirect=True)
def test_pdb_keys_reach_the_editor_while_it_answers(xwpe):
    """pdb holds back its answer to a breakpoint set at a stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text and no "k", before pdb answers, and pdb is never
    asked for the stack.  Once it answers, F8 steps.  (gdb's row holds
    the stop's own question; pdb's stop needs none for its mark, so the
    held answer is the one to a breakpoint set at the stop.)"""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, PDB_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 HOLD_TIMEOUT_S), \
        "Ctrl-G R should stop on the breakpoint:\n%s" % _pdb_stub_log(home)
    _go_to_line(xwpe, PDB_TOGGLE_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    assert _wait(lambda: ("STALL " + PDB_BREAK_ASKED) in _pdb_stub_log(home),
                 HOLD_TIMEOUT_S), \
        "the editor should ask pdb to set the breakpoint:\n%s" \
        % _pdb_stub_log(home)
    xwpe.key("ctrl+g", "k", delay=0.6)
    xwpe.type(TYPED, delay=0.4)
    xwpe.save()
    assert _wait(lambda: xwpe.saved_text() == PDB_PROG_WITH_TYPED,
                 STOP_TIMEOUT_S), \
        "keys typed while pdb answers should reach the editor, and Ctrl-G K " \
        "should type nothing (file on disk:\n%r)" % xwpe.saved_text()
    assert PDB_STACK_ASKED not in _pdb_stub_log(home), \
        "Ctrl-G K while pdb answers should be refused, not asked:\n%s" \
        % _pdb_stub_log(home)
    with open(os.path.join(home, PDB_STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    xwpe.key("F8", delay=0.6)
    assert _wait(lambda: PDB_STEP_SENT in _pdb_stub_log(home), STOP_TIMEOUT_S), \
        "once pdb answers, F8 should step:\n%s" % _pdb_stub_log(home)


# jdb: tests/stubs/jdb/jdb stands in front of the real jdb, holding one
# command as the gdb and pdb stubs do.  As for pdb, the held command is the
# answer to a breakpoint set at a stop, so the source window is the one the
# keys go to.
JDB_STUB_LOG = "jdb-stub.log"
JDB_STUB_RELEASE = "jdb-stub.release"
JDB_START_TIMEOUT_S = 45                # javac and the VM's start
JDB_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 1;\n"
    "        x = x + 1;\n"
    "        x = x + 1;\n"
    "        System.out.println(\"X \" + x);\n"
    "    }\n"
    "}\n"
)
JDB_BREAK_LINE = 4
JDB_TOGGLE_LINE = 5
JDB_BREAK_ASKED = "stop at "            # jdb's command that sets a breakpoint
JDB_STEP_SENT = "CMD next\n"
JDB_STACK_ASKED = "CMD where\n"
JDB_PROG_WITH_TYPED = "".join(
    TYPED + row if n == JDB_TOGGLE_LINE else row
    for n, row in enumerate(JDB_PROG.splitlines(True), 1))
JDB_STUB_ENV = {
    "PATH": os.path.abspath(os.path.join(STUBS_DIR, "jdb")) + ":"
            + os.environ.get("PATH", ""),
    "XWPE_STUB_REAL_JDB": shutil.which("jdb") or "",
    "XWPE_STUB_STALL": JDB_BREAK_ASKED,
    "XWPE_STUB_STALL_NTH": "2",          # the first is the start's
    "XWPE_TEST_SOURCE": JDB_PROG,
    "XWPE_TEST_FILE": "P.java",
}


def _jdb_stub_log(home):
    try:
        with open(os.path.join(home, JDB_STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("jdb") or not shutil.which("javac"),
                    reason="no jdb or javac")
@pytest.mark.parametrize("xwpe", [JDB_STUB_ENV], indirect=True)
def test_jdb_keys_reach_the_editor_while_it_answers(xwpe):
    """jdb holds back its answer to a breakpoint set at a stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text and no "k", before jdb answers, and jdb is never
    asked for the stack.  Once it answers, F8 steps."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, JDB_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 JDB_START_TIMEOUT_S), \
        "Ctrl-G R should stop on the breakpoint:\n%s" % _jdb_stub_log(home)
    _go_to_line(xwpe, JDB_TOGGLE_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    assert _wait(lambda: ("STALL " + JDB_BREAK_ASKED) in _jdb_stub_log(home),
                 HOLD_TIMEOUT_S), \
        "the editor should ask jdb to set the breakpoint:\n%s" \
        % _jdb_stub_log(home)
    xwpe.key("ctrl+g", "k", delay=0.6)
    xwpe.type(TYPED, delay=0.4)
    xwpe.save()
    assert _wait(lambda: xwpe.saved_text() == JDB_PROG_WITH_TYPED,
                 STOP_TIMEOUT_S), \
        "keys typed while jdb answers should reach the editor, and Ctrl-G K " \
        "should type nothing (file on disk:\n%r)" % xwpe.saved_text()
    assert JDB_STACK_ASKED not in _jdb_stub_log(home), \
        "Ctrl-G K while jdb answers should be refused, not asked:\n%s" \
        % _jdb_stub_log(home)
    with open(os.path.join(home, JDB_STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    xwpe.key("F8", delay=0.6)
    assert _wait(lambda: JDB_STEP_SENT in _jdb_stub_log(home), STOP_TIMEOUT_S), \
        "once jdb answers, F8 should step:\n%s" % _jdb_stub_log(home)


# a68g: tests/stubs/a68g/a68g stands in front of the real a68g, holding one
# command as the other stubs do.  As for pdb and jdb, the held command is
# the answer to a breakpoint set at a stop, so the source window is the one
# the keys go to.
A68G_STUB_LOG = "a68g-stub.log"
A68G_STUB_RELEASE = "a68g-stub.release"
A68G_PROG = (
    "INT g := 42;\n"
    "INT x := 1;\n"
    "x := x + 1;\n"
    "x := x + 1;\n"
    "print ((x, g))\n"
)
A68G_BREAK_LINE = 2
A68G_TOGGLE_LINE = 4
A68G_BREAK_ASKED = "breakpoint "        # a68g's command that sets a breakpoint
A68G_STEP_SENT = "CMD next\n"
A68G_STACK_ASKED = "CMD calls"
A68G_PROG_WITH_TYPED = "".join(
    TYPED + row if n == A68G_TOGGLE_LINE else row
    for n, row in enumerate(A68G_PROG.splitlines(True), 1))
A68G_STUB_ENV = {
    "PATH": os.path.abspath(os.path.join(STUBS_DIR, "a68g")) + ":"
            + os.environ.get("PATH", ""),
    "XWPE_STUB_REAL_A68G": shutil.which("a68g") or "",
    "XWPE_STUB_STALL": A68G_BREAK_ASKED,
    "XWPE_STUB_STALL_NTH": "2",          # the first is the start's
    "XWPE_TEST_SOURCE": A68G_PROG,
    "XWPE_TEST_FILE": "p.a68",
}


def _a68g_stub_log(home):
    try:
        with open(os.path.join(home, A68G_STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("a68g"), reason="no a68g")
@pytest.mark.parametrize("xwpe", [A68G_STUB_ENV], indirect=True)
def test_a68g_keys_reach_the_editor_while_it_answers(xwpe):
    """a68g holds back its answer to a breakpoint set at a stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text and no "k", before a68g answers, and a68g is never
    asked for the stack.  Once it answers, F8 steps."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, A68G_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 HOLD_TIMEOUT_S), \
        "Ctrl-G R should stop on the breakpoint:\n%s" % _a68g_stub_log(home)
    _go_to_line(xwpe, A68G_TOGGLE_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    assert _wait(lambda: ("STALL " + A68G_BREAK_ASKED) in _a68g_stub_log(home),
                 HOLD_TIMEOUT_S), \
        "the editor should ask a68g to set the breakpoint:\n%s" \
        % _a68g_stub_log(home)
    xwpe.key("ctrl+g", "k", delay=0.6)
    xwpe.type(TYPED, delay=0.4)
    xwpe.save()
    assert _wait(lambda: xwpe.saved_text() == A68G_PROG_WITH_TYPED,
                 STOP_TIMEOUT_S), \
        "keys typed while a68g answers should reach the editor, and Ctrl-G K " \
        "should type nothing (file on disk:\n%r)" % xwpe.saved_text()
    assert A68G_STACK_ASKED not in _a68g_stub_log(home), \
        "Ctrl-G K while a68g answers should be refused, not asked:\n%s" \
        % _a68g_stub_log(home)
    with open(os.path.join(home, A68G_STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    xwpe.key("F8", delay=0.6)
    assert _wait(lambda: A68G_STEP_SENT in _a68g_stub_log(home),
                 STOP_TIMEOUT_S), \
        "once a68g answers, F8 should step:\n%s" % _a68g_stub_log(home)


# dbx: tests/stubs/legacy/legacy-debugger stands in for it (dbx is a
# debugger of commercial UNIX, absent here), copied under the name dbx and
# chosen through the options file.  As for pdb, jdb and a68g, the held
# command is the answer to a breakpoint set at a stop.
DBX_STUB = os.path.join(STUBS_DIR, "legacy", "legacy-debugger")
DBX_STUB_LOG = "dbx-stub.log"
DBX_STUB_RELEASE = "dbx-stub.release"
DBX_DEBUGGER_TYPE = 2                   # the options file's number for dbx
DBX_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  int y = x * x;\n"
    "  printf(\"%d\\n\", y);\n"
    "  return 0;\n"
    "}\n"
)
DBX_BREAK_LINE = 4
DBX_TOGGLE_LINE = 6
DBX_BREAK_ASKED = "stop at "            # dbx's command that sets a breakpoint
DBX_STEP_SENT = "CMD next\n"
DBX_STACK_ASKED = "CMD where\n"
DBX_PROG_WITH_TYPED = "".join(
    TYPED + row if n == DBX_TOGGLE_LINE else row
    for n, row in enumerate(DBX_PROG.splitlines(True), 1))
DBX_STUB_ENV = {
    "XWPE_STUB_STALL": DBX_BREAK_ASKED,
    "XWPE_STUB_STALL_NTH": "2",          # the first is the start's
    "XWPE_STUB_SOURCE": "p.c",
    "XWPE_TEST_SOURCE": DBX_PROG,
    "XWPE_TEST_FILE": "p.c",
}


@pytest.fixture
def dbx_stub(tmp_path, monkeypatch):
    """The stand-in dbx on PATH and dbx chosen in the options file, both in
    the home the editor is started with (asked for before the editor)."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    with open(DBX_STUB) as fh:
        body = fh.read().split("\n", 1)[1]
    stub = bindir / "dbx"
    stub.write_text("#!%s\n%s" % (sys.executable, body))
    stub.chmod(0o755)
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text(
        "[Programming]\nDebugger : %d\n" % DBX_DEBUGGER_TYPE)
    monkeypatch.setenv("PATH", str(bindir) + ":" + os.environ.get("PATH", ""))
    return bindir


def _dbx_stub_log(home):
    try:
        with open(os.path.join(home, DBX_STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


# @scenario SC-debug-49
@pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")
@pytest.mark.parametrize("xwpe", [DBX_STUB_ENV], indirect=True)
def test_dbx_keys_reach_the_editor_while_it_answers(dbx_stub, xwpe):
    """dbx holds back its answer to a breakpoint set at a stop.  Ctrl-G K,
    text and File > Save typed meanwhile all reach the editor: the file on
    disk gets the text and no "k", before dbx answers, and dbx is never
    asked for the stack.  Once it answers, F8 steps."""
    if not getattr(xwpe, "can_debug", True):
        pytest.skip("no debug session under the headless compositor harness")
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, DBX_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 HOLD_TIMEOUT_S), \
        "Ctrl-G R should stop on the breakpoint:\n%s" % _dbx_stub_log(home)
    _go_to_line(xwpe, DBX_TOGGLE_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    assert _wait(lambda: ("STALL " + DBX_BREAK_ASKED) in _dbx_stub_log(home),
                 HOLD_TIMEOUT_S), \
        "the editor should ask dbx to set the breakpoint:\n%s" \
        % _dbx_stub_log(home)
    xwpe.key("ctrl+g", "k", delay=0.6)
    xwpe.type(TYPED, delay=0.4)
    xwpe.save()
    assert _wait(lambda: xwpe.saved_text() == DBX_PROG_WITH_TYPED,
                 STOP_TIMEOUT_S), \
        "keys typed while dbx answers should reach the editor, and Ctrl-G K " \
        "should type nothing (file on disk:\n%r)" % xwpe.saved_text()
    assert DBX_STACK_ASKED not in _dbx_stub_log(home), \
        "Ctrl-G K while dbx answers should be refused, not asked:\n%s" \
        % _dbx_stub_log(home)
    with open(os.path.join(home, DBX_STUB_RELEASE), "w") as fh:
        fh.write("go\n")
    xwpe.key("F8", delay=0.6)
    assert _wait(lambda: DBX_STEP_SENT in _dbx_stub_log(home),
                 STOP_TIMEOUT_S), \
        "once dbx answers, F8 should step:\n%s" % _dbx_stub_log(home)
