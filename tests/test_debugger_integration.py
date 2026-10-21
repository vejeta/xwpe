"""The Borland debugger keys, driven end to end in wpe (terminal mode).

A Turbo C user expects Ctrl-G R (Debug > Run) to build the program, start the
debugger and stop on the breakpoint with the line marked; F8 to step over,
F7 to step into, Ctrl-G U to run to the cursor; a watch to refresh at every
stop; the program's output to land in Messages and on the User Screen
(Alt-F5); and the editor to stay usable however the session ends.

Every scenario drives the real debugger and asserts on what the user sees:
the current-line mark (a bright green bar), the breakpoint mark (a red bar),
the Watches window's text, Messages, the process table and the file on disk.
A test whose debugger or compiler is missing skips and names the binary.
"""
import os
import re
import shutil
import signal
import subprocess
import time

import pytest

from wpe_driver import WpeSession, ALT, WAIT_SCALE

CTRL_G = "\x07"         # debugger command prefix
DOWN = "\033OB"
F7 = "\033[18~"         # Trace (step into)
F8 = "\033[19~"         # Step (step over)
ALT_F5 = "\033[15;3~"   # User Screen
ESC = "\033"

CURRENT_LINE_BG = "brightgreen"
BREAKPOINT_BG = "red"
POLL_S = 0.2
START_TIMEOUT_S = 20    # build + debugger start, before scaling
STEP_TIMEOUT_S = 8
IDLE_CPU_TICKS = 20     # below this per wall second the editor is idle

# Line numbers below are 1-based source lines of C_PROG.
C_PROG = (
    "#include <stdio.h>\n"
    "int sq(int v)\n"
    "{\n"
    "  return v * v;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  int y = sq(x);\n"
    "  printf(\"OUTMARK %d\\n\", y);\n"
    "  x = y + 1;\n"
    "  return 0;\n"
    "}\n"
)
C_BREAK_LINE = 8          # int x = 3;
C_NEXT_LINE = 9           # int y = sq(x);
C_CALLEE_LINE = 4         # return v * v;
C_AFTER_PRINT_LINE = 11   # x = y + 1;

PY_PROG = (
    "def sq(v):\n"
    "    return v * v\n"
    "\n"
    "x = 3\n"
    "y = sq(x)\n"
    "print('OUTMARK', y)\n"
    "x = y + 1\n"
)
PY_BREAK_LINE = 4
PY_NEXT_LINE = 5
PY_CALLEE_LINE = 1        # pdb stops on the "def" line of a called function


def _need(*tools):
    """Skip the test, naming the first missing binary."""
    for tool in tools:
        if shutil.which(tool) is None:
            pytest.skip("no %s" % tool)


def _source_rows(w):
    """Screen rows of the topmost source window's text area: between its top
    border (row 1) and the first bottom border below it."""
    disp = w.display(drain=0)
    last = len(disp) - 1
    for y in range(2, len(disp)):
        if disp[y].startswith("m"):
            last = y
            break
    return range(2, last)


def _rows_with_bg(w, bg):
    rows = []
    for y in _source_rows(w):
        row = w.screen.buffer[y]
        if any(row[x].bg == bg for x in range(1, w.screen.columns - 1)):
            rows.append(y)
    return rows


def _mark_line(w):
    """The source line carrying the current-line mark, or None."""
    rows = _rows_with_bg(w, CURRENT_LINE_BG)
    if not rows:
        return None
    return rows[0] - 1


def _breakpoint_lines(w):
    return [y - 1 for y in _rows_with_bg(w, BREAKPOINT_BG)]


def _wait_for(w, predicate, timeout):
    """Drain the editor until predicate() holds or the scaled timeout ends."""
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _screen(w):
    return "\n".join(w.display(drain=0))


def _children(w, name):
    """PIDs of the editor's direct children whose command is `name`."""
    out = subprocess.run(["ps", "-A", "-o", "pid=,ppid=,comm="],
                         capture_output=True, text=True).stdout
    pids = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and int(parts[1]) == w.proc.pid and \
                os.path.basename(parts[2].strip()) == name:
            pids.append(int(parts[0]))
    return pids


def _cpu_ticks(pid):
    """utime + stime in clock ticks, from /proc; None where there is none."""
    try:
        fields = open("/proc/%d/stat" % pid).read().split()
    except OSError:
        return None
    return int(fields[13]) + int(fields[14])


def _move_down(w, lines):
    for _ in range(lines):
        w.key(DOWN, delay=0.1)


def _start_c_session(w):
    """Breakpoint on C_BREAK_LINE, then Ctrl-G R; wait for the stop."""
    _move_down(w, C_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == C_BREAK_LINE,
                     START_TIMEOUT_S), \
        "Ctrl-G R should stop on the breakpoint (line %d):\n%s" \
        % (C_BREAK_LINE, _screen(w))


def _step(w, key, line):
    w.key(key, delay=0.3)
    return _wait_for(w, lambda: _mark_line(w) == line, STEP_TIMEOUT_S)


def _c_session(tmp_path, **kw):
    return WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5, **kw)


# @scenario SC-debug-01
def test_ctrl_g_r_builds_and_stops_on_the_breakpoint(tmp_path):
    """Ctrl-G R on a C source with a breakpoint builds the program, starts
    gdb and stops on the breakpoint line, marked as the current line."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        assert os.path.exists(str(tmp_path / "p.e")), \
            "Ctrl-G R should have built the program first"
        assert _children(w, "gdb"), "gdb should be running while stopped"


# @scenario SC-debug-03
def test_f8_steps_over_and_f7_steps_into(tmp_path):
    """F8 moves the mark to the next line; F7 on a call enters the callee."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 should move the mark to line %d:\n%s" % (C_NEXT_LINE, _screen(w))
        assert _step(w, F7, C_CALLEE_LINE), \
            "F7 on the call should stop inside sq() on line %d:\n%s" \
            % (C_CALLEE_LINE, _screen(w))


# @scenario SC-debug-05
def test_ctrl_g_u_runs_to_the_cursor_line(tmp_path):
    """Ctrl-G U (Goto cursor) runs to the line under the cursor; the printf
    on the way has run, so its output is in Messages."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        _move_down(w, C_AFTER_PRINT_LINE - C_BREAK_LINE)
        w.key(CTRL_G, "u", delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == C_AFTER_PRINT_LINE,
                         STEP_TIMEOUT_S), \
            "Ctrl-G U should stop on the cursor line %d:\n%s" \
            % (C_AFTER_PRINT_LINE, _screen(w))
        assert _wait_for(w, lambda: "OUTMARK 9" in _screen(w), STEP_TIMEOUT_S), \
            "the printf that ran should show in Messages:\n%s" % _screen(w)


# @scenario SC-debug-08
def test_breakpoint_survives_a_debugger_restart(tmp_path):
    """After the program ran to its end and the debugger stopped, the red
    breakpoint mark is still there and the next Ctrl-G R stops on it again."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "running to the end should stop gdb:\n%s" % _screen(w)
        assert C_BREAK_LINE in _breakpoint_lines(w), \
            "the breakpoint mark should survive the session:\n%s" % _screen(w)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == C_BREAK_LINE,
                         START_TIMEOUT_S), \
            "the restarted session should stop on the same breakpoint:\n%s" \
            % _screen(w)


def _watch_value(w, name):
    pat = re.compile(r"(?:^|\s)%s: (-?\d+)" % re.escape(name))
    for row in w.display(drain=0):
        m = pat.search(row[1:])         # past the window's left border
        if m:
            return int(m.group(1))
    return None


# @scenario SC-debug-10
def test_watch_shows_its_value_and_refreshes_at_the_next_stop(tmp_path):
    """Ctrl-G W x opens the Watches window with x's value; after the
    assignment runs (F8) the window shows the new value."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_G, "w", delay=0.8)
        w.key("x", delay=0.1)
        w.key("\r", delay=0.5)
        assert _wait_for(w, lambda: _watch_value(w, "x") is not None,
                         STEP_TIMEOUT_S), \
            "the Watches window should show x:\n%s" % _screen(w)
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _watch_value(w, "x") == 3, STEP_TIMEOUT_S), \
            "after int x = 3 ran, the watch should read 3:\n%s" % _screen(w)


# @scenario SC-debug-11
def test_program_output_goes_to_messages_and_the_user_screen(tmp_path):
    """The program's printf lands in Messages, not in the source window, and
    Alt-F5 shows it on the User Screen; a key returns to the editor."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        for line in (C_NEXT_LINE, C_NEXT_LINE + 1, C_AFTER_PRINT_LINE):
            assert _step(w, F8, line), \
                "F8 should reach line %d:\n%s" % (line, _screen(w))
        assert _wait_for(w, lambda: "OUTMARK 9" in _screen(w), STEP_TIMEOUT_S), \
            "printf output should be in Messages:\n%s" % _screen(w)
        source = [w.display(drain=0)[y] for y in _source_rows(w)]
        assert not any("OUTMARK 9" in row for row in source), \
            "program output must not be written into the source window"
        w.key(ALT_F5, delay=1.0)
        assert "OUTMARK 9" in _screen(w), \
            "Alt-F5 should show the program's output:\n%s" % _screen(w)
        w.key("\r", delay=0.8)
        assert "Debug" in w.display(drain=0)[0], "a key should return to the editor"


# @scenario SC-debug-13
def test_continue_to_the_end_stops_the_debugger_and_frees_the_editor(tmp_path):
    """Ctrl-G R from a stop runs to the end: Messages says the program exited,
    the mark goes, gdb is gone, and typing edits the source again."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: "Program exited" in _screen(w),
                         STEP_TIMEOUT_S), \
            "Messages should say the program exited:\n%s" % _screen(w)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "gdb should be gone after the program ended"
        assert _mark_line(w) is None, \
            "the current-line mark should go:\n%s" % _screen(w)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q  int x = 3;" in w.text(), \
            "typing after the session should edit the source"


# @scenario SC-debug-15
def test_a_killed_debugger_leaves_the_editor_idle_and_usable(tmp_path):
    """gdb killed while stopped: the next F8 ends the session, the editor does
    not spin, and typing edits the source."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        pids = _children(w, "gdb")
        assert len(pids) == 1, "expected one gdb child, saw %s" % pids
        os.kill(pids[0], signal.SIGKILL)
        w.key(F8, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) is None, STEP_TIMEOUT_S), \
            "the session should end once gdb is gone:\n%s" % _screen(w)
        before = _cpu_ticks(w.proc.pid)
        w._drain(1.0)
        after = _cpu_ticks(w.proc.pid)
        if before is not None and after is not None:
            assert after - before < IDLE_CPU_TICKS * WAIT_SCALE, \
                "the editor should be idle, used %d ticks in 1s" % (after - before)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q  int x = 3;" in w.text(), "the editor should take keys again"


# @scenario SC-debug-18
def test_debug_quit_while_stopped_ends_the_session_cleanly(tmp_path):
    """Esc while stopped leaves the session alone (as in Turbo C, where Esc is
    not Program reset); Debug > Quit ends it, gdb is gone, the mark goes."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(ESC, delay=0.6)
        assert _mark_line(w) == C_BREAK_LINE and _children(w, "gdb"), \
            "Esc alone should not end the session:\n%s" % _screen(w)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Debug > Quit should end gdb"
        assert "End of Debugging" in _screen(w)
        assert _mark_line(w) is None, \
            "the current-line mark should go:\n%s" % _screen(w)


# @scenario SC-debug-25
def test_a_debugger_not_installed_is_named_and_nothing_hangs(tmp_path):
    """Debug > Options set to Sdb, which is not installed: Ctrl-G R says
    which debugger is missing, and the editor takes keys again."""
    _need("gcc")
    if shutil.which("sdb") is not None:
        pytest.skip("sdb is installed here")
    with _c_session(tmp_path) as w:
        w.key(CTRL_G, "o", delay=0.6)
        w.key(DOWN, delay=0.3)          # focus moves to Sdb
        w.key(" ", delay=0.3)           # select it
        w.key("\033o", delay=0.5)       # Ok
        _move_down(w, C_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: "'sdb' not in Path" in _screen(w),
                         START_TIMEOUT_S), \
            "the missing debugger should be named:\n%s" % _screen(w)
        w.key("\r", delay=0.5)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q  int x = 3;" in w.text(), "the editor should take keys again"
        assert not _children(w, "sdb")


ALT_1 = "\0331"            # window 1, the source
SILENT_DEBUGGER_LOG = "debugger-stdin.log"
LONG_START_TIMEOUT_S = 600   # far past any test: only Esc may end the start
# Long enough to see the waiting line first, also on a slow machine.
SHORT_START_TIMEOUT_S = int(4 * WAIT_SCALE)


def _silent_gdb_env(tmp_path, timeout_s):
    """A `gdb` first on PATH that reads its input into a log and never
    writes, so the start never gets an answer; plus the start deadline."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    fake = bindir / "gdb"
    fake.write_text("#!/bin/sh\nexec cat > %s\n"
                    % (tmp_path / SILENT_DEBUGGER_LOG))
    fake.chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
            "XWPE_DEBUG_START_TIMEOUT": str(timeout_s)}


def _start_silent_session(w):
    """Breakpoint, Ctrl-G R, and wait for the waiting line; return the PID
    of the silent debugger (its program is `cat`)."""
    _move_down(w, C_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: "starting gdb" in _screen(w), START_TIMEOUT_S), \
        "a waiting line should say the debugger is starting:\n%s" % _screen(w)
    # The script becomes `cat` only once its shell has run the exec.
    _wait_for(w, lambda: _children(w, "cat"), STEP_TIMEOUT_S)
    pids = _children(w, "cat")
    assert len(pids) == 1, "expected one silent debugger, saw %s" % pids
    return pids[0]


def _process_gone(pid):
    """True once `pid` is no longer in the process table (a zombie that was
    not reaped is still listed)."""
    return subprocess.run(["ps", "-p", str(pid)],
                          capture_output=True).returncode != 0


# @scenario SC-debug-17
def test_esc_ends_a_debugger_that_never_answers(tmp_path):
    """A debugger that never answers does not hold the editor: the waiting
    line counts, the editor is idle and takes keys, a debugger key is
    refused on the status line, Esc ends the debugger and Messages says the
    start was cancelled; no key reached the debugger."""
    _need("gcc")
    env = _silent_gdb_env(tmp_path, LONG_START_TIMEOUT_S)
    with _c_session(tmp_path, env_extra=env) as w:
        pid = _start_silent_session(w)
        assert _wait_for(w, lambda: re.search(r"starting gdb \.\.\. [1-9]\d*s",
                                              _screen(w)), STEP_TIMEOUT_S), \
            "the waiting line should count the seconds:\n%s" % _screen(w)
        before = _cpu_ticks(w.proc.pid)
        w._drain(1.0)
        after = _cpu_ticks(w.proc.pid)
        if before is not None and after is not None:
            assert after - before < IDLE_CPU_TICKS * WAIT_SCALE, \
                "the editor should be idle while it waits, used %d ticks in 1s" \
                % (after - before)
        w.key(F8, delay=0.5)
        assert "debugger is starting" in w.display(drain=0)[-1], \
            "F8 during the start should be refused on the status line:\n%s" \
            % _screen(w)
        w.key(ALT_1, delay=0.4)         # Messages is on top: back to p.c
        w.key("Q", delay=0.3)
        w.key(ESC, delay=0.5)
        assert _wait_for(w, lambda: "gdb start cancelled" in _screen(w),
                         STEP_TIMEOUT_S), \
            "Esc should end the start and say so:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "the silent debugger (pid %d) should be gone" % pid
        w.save()
        assert "Q  int x = 3;" in w.text(), \
            "a key typed during the start should edit the source"
        assert (tmp_path / SILENT_DEBUGGER_LOG).read_text() == "", \
            "no key may be written to a debugger that has not answered"


# @scenario SC-debug-17
def test_a_debugger_that_never_answers_ends_at_the_deadline(tmp_path):
    """Left alone, a start that gets no answer ends at the deadline with a
    line that names the debugger and the time; the editor takes keys."""
    _need("gcc")
    env = _silent_gdb_env(tmp_path, SHORT_START_TIMEOUT_S)
    with _c_session(tmp_path, env_extra=env) as w:
        pid = _start_silent_session(w)
        expected = "gdb did not answer within %d s" % SHORT_START_TIMEOUT_S
        assert _wait_for(w, lambda: expected in _screen(w), STEP_TIMEOUT_S), \
            "the start should end at the deadline:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "the silent debugger (pid %d) should be gone" % pid
        w.key("Q", delay=0.3)
        w.save()
        assert "Q  int x = 3;" in w.text(), "the editor should take keys again"


# @scenario SC-debug-29
def test_a_session_runs_and_steps_on_an_80x24_screen(tmp_path):
    """The whole Ctrl-G R / F8 cycle works on the smallest supported screen."""
    _need("gcc", "gdb")
    with _c_session(tmp_path, cols=80, rows=24) as w:
        _start_c_session(w)
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 should move the mark on 80x24:\n%s" % _screen(w)


def _start_py_session(w):
    """Breakpoint on PY_BREAK_LINE, Ctrl-G R, dismiss the one-time notice
    that the Python file switches the backend to pdb."""
    _move_down(w, PY_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: "switching to pdb" in _screen(w), STEP_TIMEOUT_S)
    w.key("\r", delay=0.5)
    assert _wait_for(w, lambda: _children(w, "python3"), START_TIMEOUT_S), \
        "pdb should be running:\n%s" % _screen(w)


# @scenario SC-debug-19
def test_pdb_f8_steps_over_and_f7_steps_into(tmp_path):
    """The same keys on a Python file: F8 moves the mark to the next line,
    F7 on a call stops in the called function."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _start_py_session(w)
        w._drain(2.0)
        assert _step(w, F8, PY_NEXT_LINE), \
            "F8 should move the mark to line %d:\n%s" % (PY_NEXT_LINE, _screen(w))
        assert _step(w, F7, PY_CALLEE_LINE), \
            "F7 should stop in sq() on line %d:\n%s" % (PY_CALLEE_LINE, _screen(w))
