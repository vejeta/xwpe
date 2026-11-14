"""A debugger session works when the editor has no controlling terminal (X11).

Started from a desktop launcher, the editor is the leader of a session that
has no controlling terminal.  A leader that opens a terminal device without
O_NOCTTY takes it as its controlling terminal.  The terminal the editor
opens is the debugger's program terminal (the slave of a pty), which it
hands to a child: the child then cannot take it itself (jdb's VM fails with
"cannot give P a terminal"), and closing the master when the session ends
makes the kernel send the editor SIGHUP.

Every row runs twice, the editor started as a session leader without a
controlling terminal (the case) and started as an ordinary child of the
test (the control, which has always worked).  The Wayland twin
(tests/wayland/test_debug_no_ctty.py) runs the same body.
"""
import os
import shutil
import sys
import time

import pytest

from conftest import _sleep

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import procinfo  # noqa: E402  (the tests/ directory is added just above)

DETACHED = "XWPE_TEST_DETACHED"  # present: started as a desktop launcher does
LAUNCHES = [True, False]        # a launcher's session, then an ordinary child
START_TIMEOUT_S = 60
END_TIMEOUT_S = 10
SETTLE_S = 3.0
TYPED = "MARK"

# A program that waits for a line on stdin: the session stays running.
BLOCKED_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  char line[100];\n"
    "  FILE *o;\n"
    "  o = fopen(\"started.txt\", \"w\");\n"
    "  fputs(\"1\", o);\n"
    "  fclose(o);\n"
    "  if (fgets(line, sizeof line, stdin))\n"
    "    return 1;\n"
    "  return 0;\n"
    "}\n"
)
BLOCKED_BREAK_LINE = 11        # return 0;  never reached while it waits

# A Java program that sleeps: its VM is the child that needs the terminal.
JDB_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        Thread.sleep(600000);\n"
    "    }\n"
    "}\n"
)
JDB_SLEEP_LINE = 3
# The first column of the first line of the source window at the fixture's
# pinned font size: the run put Messages on top, a click brings the source
# back and puts the cursor there.
SOURCE_CLICK_X = 12
SOURCE_CLICK_Y = 110
JDB_TREE_SIZE = 3             # script, jdb and the VM at least


def _launches(source, name=None):
    """The fixture parameters of a row: one launch per way of starting."""
    rows = []
    for detached in LAUNCHES:
        env = {"XWPE_TEST_SOURCE": source}
        if detached:
            env[DETACHED] = "1"
        if name:
            env["XWPE_TEST_FILE"] = name
        rows.append(env)
    return rows


LAUNCH_IDS = ["session-leader", "child"]
JDB_LAUNCHES = _launches(JDB_PROG, "P.java")
GDB_LAUNCHES = _launches(BLOCKED_PROG)


def _wait(predicate, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        _sleep(0.3)
    return predicate()


def _gdb(xwpe):
    """The live gdb among the editor's descendants."""
    return [pid for pid in procinfo.descendants(xwpe.proc.pid)
            if procinfo._live_and_named(pid, "gdb")]


def _run_gdb_until_blocked(xwpe):
    """Breakpoint after the read, Ctrl-G R; return once the program waits on
    its stdin, which is a running session."""
    for _ in range(BLOCKED_BREAK_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    started = os.path.join(os.path.dirname(xwpe.srcfile), "started.txt")
    assert _wait(lambda: os.path.exists(started), START_TIMEOUT_S), \
        "the program should build, start under gdb and run"
    assert _gdb(xwpe), "gdb should be running"
    _sleep(1.0)


def _have(*tools):
    return all(shutil.which(t) for t in tools)


# @scenario SC-debug-18
@pytest.mark.skipif(not _have("jdb", "javac"), reason="no jdb or javac")
@pytest.mark.parametrize("xwpe", JDB_LAUNCHES, ids=LAUNCH_IDS, indirect=True)
def test_jdb_starts_and_keys_reach_the_editor_without_a_controlling_tty(xwpe):
    """jdb starts, its VM stays up and keys typed meanwhile reach the
    editor: the text typed after the start is in the file saved."""
    for _ in range(JDB_SLEEP_LINE - 1):
        xwpe.key("Down", delay=0.1)
    xwpe.key("ctrl+g", "b")
    xwpe.key("ctrl+g", "r")
    assert _wait(lambda: len(procinfo.descendants(xwpe.proc.pid))
                 >= JDB_TREE_SIZE, START_TIMEOUT_S), \
        "jdb and its VM should be running: %s" \
        % procinfo.descendants(xwpe.proc.pid)
    _sleep(SETTLE_S)
    assert xwpe.proc.poll() is None, "the editor died starting jdb"
    assert len(procinfo.descendants(xwpe.proc.pid)) >= JDB_TREE_SIZE, \
        "the VM should still be up (it is gone when it cannot take its " \
        "terminal): %s" % procinfo.descendants(xwpe.proc.pid)
    xwpe.click(SOURCE_CLICK_X, SOURCE_CLICK_Y, delay=0.6)   # source on top
    xwpe.type(TYPED)
    xwpe.save()
    assert _wait(lambda: TYPED in xwpe.saved_text(), END_TIMEOUT_S), \
        "keys typed after the start should reach the editor (file on " \
        "disk:\n%r)" % xwpe.saved_text()


# @scenario SC-debug-18
@pytest.mark.skipif(not _have("gdb", "gcc"), reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", GDB_LAUNCHES, ids=LAUNCH_IDS, indirect=True)
def test_ctrl_f2_ends_a_gdb_session_without_a_controlling_tty(xwpe):
    """Ctrl-F2 ends the gdb session while the program runs, and the editor
    lives on: closing the debugger's terminal sends it no hangup."""
    _run_gdb_until_blocked(xwpe)
    xwpe.key("ctrl+F2", delay=0.8)      # asks whether to quit
    xwpe.key("y", delay=0.6)
    assert _wait(lambda: not _gdb(xwpe), END_TIMEOUT_S), \
        "Ctrl-F2 should end gdb while the program runs"
    _sleep(SETTLE_S)
    assert xwpe.proc.poll() is None, "the editor died ending the session"


# @scenario SC-debug-18
@pytest.mark.skipif(not _have("gdb", "gcc"), reason="no gdb or gcc")
@pytest.mark.parametrize("xwpe", GDB_LAUNCHES, ids=LAUNCH_IDS, indirect=True)
def test_debug_quit_while_the_program_runs_without_a_controlling_tty(xwpe):
    """Debug > Quit ends the session while the program runs, and the editor
    lives on."""
    _run_gdb_until_blocked(xwpe)
    xwpe.menu("d", "q")
    assert _wait(lambda: not _gdb(xwpe), END_TIMEOUT_S), \
        "Debug > Quit should end gdb while the program runs"
    _sleep(SETTLE_S)
    assert xwpe.proc.poll() is None, "the editor died ending the session"
