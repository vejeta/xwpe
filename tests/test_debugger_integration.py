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
import sys
import time

import pytest

import procinfo
from wpe_driver import (WpeSession, ALT, WAIT_SCALE, tool_usable,
                        CURRENT_LINE_BG, BREAKPOINT_BG, incoherence)

CTRL_G = "\x07"         # debugger command prefix
DOWN = "\033OB"
F7 = "\033[18~"         # Trace (step into)
F8 = "\033[19~"         # Step (step over)
ALT_F5 = "\033[15;3~"   # User Screen
ESC = "\033"

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
    """Screen rows of the topmost source window's text area."""
    return w.source_rows()


def _rows_with_bg(w, bg):
    return w.rows_with_bg(bg)


def _mark_line(w):
    """The source line carrying the current-line mark, or None."""
    return w.mark_line()


def _breakpoint_lines(w):
    return w.breakpoint_lines()


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
    return procinfo.children_named(w.proc.pid, name)


def _cpu_ticks(pid):
    """utime + stime in clock ticks; None once the process is gone."""
    return procinfo.cpu_ticks(pid)


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


# A debug adapter that never answers, on each transport a fake can stand in
# for: Rust's gdb speaks DAP on its stdin/stdout (the editor writes the
# initialize request and waits for the answer), Go's dlv is told a port and
# must dial back in (the editor waits for the connection).  Each fake is the
# adapter's name first on PATH, running `cat` into a log and never writing.
DAP_RUST_PROG = (
    "fn main() {\n"
    "    let x = 3;\n"
    "    println!(\"{}\", x);\n"
    "}\n"
)
DAP_GO_PROG = (
    "package main\n"
    "\n"
    "func main() {\n"
    "    x := 3\n"
    "    _ = x\n"
    "}\n"
)
# Rust is compiled by the editor before gdb starts: a stand-in rustc writes
# the binary the -o option names, so no toolchain is needed.
FAKE_RUSTC = (
    "#!/bin/sh\n"
    "while [ $# -gt 0 ]; do\n"
    "  if [ \"$1\" = -o ]; then : > \"$2\"; fi\n"
    "  shift\n"
    "done\n"
)
# "line" is where the cursor goes before Run, "typed" that line after a Q.
SILENT_ADAPTERS = {
    "stdio": {"adapter": "gdb", "file": "main.rs", "prog": DAP_RUST_PROG,
              "line": 2, "typed": "Q    let x = 3;"},
    "reverse-tcp": {"adapter": "dlv", "file": "main.go", "prog": DAP_GO_PROG,
                    "line": 4, "typed": "Q    x := 3"},
}


def _write_script(path, text):
    path.write_text(text)
    path.chmod(0o755)


def _silent_adapter_env(tmp_path, adapter, timeout_s):
    """The adapter's name first on PATH as a program that reads its input
    into a log and never writes, a stand-in rustc, and the start deadline.
    The adapter's own output stays open on descriptor 3, so the editor sees
    an adapter that is silent, not one that has hung up."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_script(bindir / adapter, "#!/bin/sh\nexec 3>&1\nexec cat > %s\n"
                  % (tmp_path / SILENT_DEBUGGER_LOG))
    _write_script(bindir / "rustc", FAKE_RUSTC)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
            "XWPE_DEBUG_START_TIMEOUT": str(timeout_s)}


def _dap_session(tmp_path, transport, timeout_s):
    spec = SILENT_ADAPTERS[transport]
    env = _silent_adapter_env(tmp_path, spec["adapter"], timeout_s)
    return WpeSession(str(tmp_path), spec["prog"], filename=spec["file"],
                      wait=1.5, env_extra=env)


def _start_silent_adapter(w, transport):
    """Ctrl-G R on the source, and wait for the waiting line; return the PID
    of the silent adapter (its program is `cat`)."""
    adapter = SILENT_ADAPTERS[transport]["adapter"]
    _move_down(w, SILENT_ADAPTERS[transport]["line"] - 1)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: "starting %s" % adapter in _screen(w),
                     START_TIMEOUT_S), \
        "a waiting line should say the adapter is starting:\n%s" % _screen(w)
    _wait_for(w, lambda: _children(w, "cat"), STEP_TIMEOUT_S)
    pids = _children(w, "cat")
    assert len(pids) == 1, "expected one silent adapter, saw %s" % pids
    return pids[0]


def _adapter_log(tmp_path):
    return (tmp_path / SILENT_DEBUGGER_LOG).read_text()


# @scenario SC-debug-17
@pytest.mark.parametrize("transport", sorted(SILENT_ADAPTERS))
def test_esc_ends_a_debug_adapter_that_never_answers(tmp_path, transport):
    """A debug adapter that never answers does not hold the editor: the
    waiting line counts, the editor takes keys, a debugger key is refused on
    the status line, Esc ends the adapter and Messages says the start was
    cancelled; nothing past the initialize request reached the adapter."""
    adapter = SILENT_ADAPTERS[transport]["adapter"]
    with _dap_session(tmp_path, transport, LONG_START_TIMEOUT_S) as w:
        pid = _start_silent_adapter(w, transport)
        assert _wait_for(w, lambda: re.search(r"starting %s \.\.\. [1-9]\d*s"
                                              % adapter, _screen(w)),
                         STEP_TIMEOUT_S), \
            "the waiting line should count the seconds:\n%s" % _screen(w)
        w.key(F8, delay=0.5)
        assert "debugger is starting" in w.display(drain=0)[-1], \
            "F8 during the start should be refused on the status line:\n%s" \
            % _screen(w)
        w.key(ALT_1, delay=0.4)         # back to the source window
        w.key("Q", delay=0.3)
        w.key(ESC, delay=0.5)
        assert _wait_for(w, lambda: "%s start cancelled" % adapter
                         in _screen(w), STEP_TIMEOUT_S), \
            "Esc should end the start and say so:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "the silent adapter (pid %d) should be gone" % pid
        w.save()
        assert SILENT_ADAPTERS[transport]["typed"] in w.text(), \
            "a key typed during the start should edit the source"
        assert '"launch"' not in _adapter_log(tmp_path), \
            "nothing past initialize may be sent to an adapter that has " \
            "not answered"


# @scenario SC-debug-17
@pytest.mark.parametrize("transport", sorted(SILENT_ADAPTERS))
def test_a_debug_adapter_that_never_answers_ends_at_the_deadline(tmp_path,
                                                                 transport):
    """Left alone, an adapter start that gets no answer ends at the deadline
    with a line that names the adapter and the time; the editor takes keys."""
    adapter = SILENT_ADAPTERS[transport]["adapter"]
    with _dap_session(tmp_path, transport, SHORT_START_TIMEOUT_S) as w:
        pid = _start_silent_adapter(w, transport)
        expected = "%s did not answer within %d s" % (adapter,
                                                      SHORT_START_TIMEOUT_S)
        assert _wait_for(w, lambda: expected in _screen(w), STEP_TIMEOUT_S), \
            "the start should end at the deadline:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "the silent adapter (pid %d) should be gone" % pid
        w.key(ALT_1, delay=0.4)
        w.key("Q", delay=0.3)
        w.save()
        assert SILENT_ADAPTERS[transport]["typed"] in w.text(), \
            "the editor should take keys again"


# A dlv stand-in that dials back in and answers initialize, then either says
# nothing more ("silent", a long build) or refuses the launch the way dlv
# does when the package does not build ("fail").  It writes its PID to a file
# so the test can check that exact process.
FAKE_DLV = r'''#!%(python)s -I
import json, os, socket, sys
MODE = %(mode)r
BUILD_ERROR = %(error)r
with open(%(pidfile)r, "w") as f:
    f.write(str(os.getpid()))
addr = [a for a in sys.argv if a.startswith("--client-addr=")][0]
host, port = addr.split("=", 1)[1].rsplit(":", 1)
conn = socket.create_connection((host, int(port)))
pending = b""

def read_message():
    global pending
    while True:
        if b"\r\n\r\n" in pending:
            head, rest = pending.split(b"\r\n\r\n", 1)
            size = int(head.split(b":")[1])
            if len(rest) >= size:
                pending = rest[size:]
                return json.loads(rest[:size])
        chunk = conn.recv(4096)
        if not chunk:
            return None
        pending += chunk

def answer(request, success, **extra):
    body = dict(seq=0, type="response", request_seq=request["seq"],
                command=request["command"], success=success, **extra)
    data = json.dumps(body).encode()
    conn.sendall(b"Content-Length: %%d\r\n\r\n" %% len(data) + data)

while True:
    request = read_message()
    if request is None:
        break
    if request.get("command") == "initialize":
        answer(request, True, body={})
    elif request.get("command") == "launch" and MODE == "fail":
        answer(request, False, message="Failed to launch",
               body={"error": {"id": 3000, "format": BUILD_ERROR}})
'''
DLV_BUILD_ERROR = "main.go:4:5: undefined: nosuchname"
DLV_PIDFILE = "dlv.pid"


def _answering_dlv_env(tmp_path, mode, timeout_s):
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_script(bindir / "dlv", FAKE_DLV % {
        "python": sys.executable, "mode": mode, "error": DLV_BUILD_ERROR,
        "pidfile": str(tmp_path / DLV_PIDFILE)})
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
            "XWPE_DEBUG_START_TIMEOUT": str(timeout_s)}


def _answering_dlv_session(tmp_path, mode, timeout_s):
    env = _answering_dlv_env(tmp_path, mode, timeout_s)
    return WpeSession(str(tmp_path), DAP_GO_PROG, filename="main.go",
                      wait=1.5, env_extra=env)


def _run_go(w):
    _move_down(w, SILENT_ADAPTERS["reverse-tcp"]["line"] - 1)
    w.key(CTRL_G, "r", delay=1.0)


def _dlv_pid(w, tmp_path):
    pidfile = tmp_path / DLV_PIDFILE
    assert _wait_for(w, pidfile.exists, STEP_TIMEOUT_S), \
        "the dlv stand-in never ran:\n%s" % _screen(w)
    _wait_for(w, lambda: pidfile.read_text().strip() != "", STEP_TIMEOUT_S)
    return int(pidfile.read_text())


def _type_in_source(w):
    w.key(ALT_1, delay=0.4)
    w.key("Q", delay=0.3)
    w.save()
    assert SILENT_ADAPTERS["reverse-tcp"]["typed"] in w.text(), \
        "the editor should take keys"


# @scenario SC-debug-17
def test_dlv_that_answered_is_not_ended_at_the_deadline(tmp_path):
    """Once dlv has answered initialize it is alive: a long build runs past
    the start deadline, the waiting line says it is building, and Esc still
    ends it."""
    with _answering_dlv_session(tmp_path, "silent",
                                SHORT_START_TIMEOUT_S) as w:
        _run_go(w)
        assert _wait_for(w, lambda: "dlv building" in _screen(w),
                         START_TIMEOUT_S), \
            "the waiting line should say dlv is building:\n%s" % _screen(w)
        pid = _dlv_pid(w, tmp_path)
        w._drain(SHORT_START_TIMEOUT_S + 2 * WAIT_SCALE)
        assert "did not answer" not in _screen(w), \
            "a dlv that answered must not be ended at the deadline:\n%s" \
            % _screen(w)
        assert "dlv building" in _screen(w), \
            "the waiting line should still say dlv is building:\n%s" \
            % _screen(w)
        assert not _process_gone(pid), "dlv (pid %d) should still run" % pid
        w.key(ALT_1, delay=0.4)
        w.key(ESC, delay=0.5)
        assert _wait_for(w, lambda: "dlv start cancelled" in _screen(w),
                         STEP_TIMEOUT_S), \
            "Esc should end the build and say so:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "dlv (pid %d) should be gone" % pid
        _type_in_source(w)


# @scenario SC-debug-17
def test_a_failed_dlv_launch_is_reported_and_ended(tmp_path):
    """dlv refusing the launch (the package does not build) ends the start:
    its error is shown in Messages, dlv is ended, and the editor takes keys."""
    with _answering_dlv_session(tmp_path, "fail", LONG_START_TIMEOUT_S) as w:
        _run_go(w)
        pid = _dlv_pid(w, tmp_path)
        assert _wait_for(w, lambda: DLV_BUILD_ERROR in _screen(w),
                         START_TIMEOUT_S), \
            "the build error should be shown in Messages:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "dlv (pid %d) should be gone" % pid
        _type_in_source(w)


# @scenario SC-debug-29
def test_a_session_runs_and_steps_on_an_80x24_screen(tmp_path):
    """The whole Ctrl-G R / F8 cycle works on the smallest supported screen."""
    _need("gcc", "gdb")
    with _c_session(tmp_path, cols=80, rows=24) as w:
        _start_c_session(w)
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 should move the mark on 80x24:\n%s" % _screen(w)


def _start_py_session(w):
    """Breakpoint on PY_BREAK_LINE, Ctrl-G R; the Python file switches the
    backend to pdb and says so on the status row, with nothing to dismiss."""
    _move_down(w, PY_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _status_row_says(w, "switching to pdb", STEP_TIMEOUT_S)
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


# @scenario SC-debug-30
def test_pdb_first_stop_on_the_breakpoint_is_marked(tmp_path):
    """Ctrl-G R on a Python file stops on the breakpoint with the line
    marked at once, as every later step is, without a key being pressed."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _start_py_session(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         STEP_TIMEOUT_S), \
            "the first stop (line %d) should carry the current-line mark; " \
            "mark is on %s:\n%s" % (PY_BREAK_LINE, _mark_line(w), _screen(w))


# Full path lengths of the source under test: past the 128 bytes the debugger
# start used to keep, past the 256-byte lines it used to read, and close to
# PATH_MAX (4096 on Linux) with every directory name well under NAME_MAX.
LONG_PATH_BYTES = [200, 400, 4000]
# jdb is checked at the two shorter lengths only: it names classes, not paths.
JDB_LONG_PATH_BYTES = [200, 400]


def _deep_name(tmp_path, leaf, total):
    """leaf under nested 40-byte directories until the full path is at least
    `total` bytes; creates the directories and returns the relative name."""
    dirs = []
    while len(os.path.join(str(tmp_path), *dirs, leaf)) < total:
        dirs.append("d" * 40)
    rel = os.path.join(*dirs, leaf)
    os.makedirs(os.path.join(str(tmp_path), os.path.dirname(rel)))
    assert total <= len(os.path.join(str(tmp_path), rel)) < total + 45
    return rel


# @scenario SC-debug-30
@pytest.mark.parametrize("total", LONG_PATH_BYTES)
def test_pdb_starts_on_a_source_under_a_very_long_path(tmp_path, total):
    """A Python file in a very deep directory still starts pdb and stops,
    marked, on its breakpoint (the path was cut short before)."""
    _need("python3")
    name = _deep_name(tmp_path, "p.py", total)
    with WpeSession(str(tmp_path), PY_PROG, filename=name, wait=1.5) as w:
        _start_py_session(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         STEP_TIMEOUT_S), \
            "pdb should stop on line %d of a long-path file; mark is on " \
            "%s:\n%s" % (PY_BREAK_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-01
@pytest.mark.parametrize("total", LONG_PATH_BYTES)
def test_gdb_starts_on_a_source_under_a_very_long_path(tmp_path, total):
    """A C file in a very deep directory builds, starts gdb and stops,
    marked, on its breakpoint: gdb's stop line carries the whole path."""
    _need("gcc", "gdb")
    name = _deep_name(tmp_path, "p.c", total)
    with WpeSession(str(tmp_path), C_PROG, filename=name, wait=1.5) as w:
        _start_c_session(w)
        assert _children(w, "gdb"), "gdb should be running while stopped"


# @scenario SC-debug-20
@pytest.mark.parametrize("total", JDB_LONG_PATH_BYTES)
def test_jdb_starts_on_a_source_under_a_very_long_path(tmp_path, total):
    """A Java file in a very deep directory starts jdb and stops, marked,
    on its breakpoint.  Coverage only: jdb is given and answers with class
    names, never the file's path, so this passed before the debugger start
    took long paths as well and guards no fix of its own."""
    _need("jdb", "javac")
    name = _deep_name(tmp_path, "P.java", total)
    with WpeSession(str(tmp_path), JAVA_PROG, filename=name, wait=1.5) as w:
        _start_java_session(w)


# A step whose program prints more than the debugger reader's buffer holds
# before pdb's next prompt: a long line and then an ordinary one, or one line
# with no end at all, so that pdb's stop line is glued to the program's
# output.  Line numbers are 1-based lines of the program built from each row.
PY_FLOOD_ROWS = {
    "long-then-ended": "print('x' * 6000, end=''); print('y' * 6000)",
    "one-unended-line": "print('x' * 20000, end='')",
}
PY_FLOOD_BREAK_LINE = 2
PY_FLOOD_NEXT_LINE = 3
PY_FLOOD_LAST_LINE = 4


def _py_flood_prog(flood):
    return ("x = 1\n"
            "%s\n"
            "x = 2\n"
            "print('DONE', x)\n" % flood)


def _start_py_session_on(w, line):
    """Breakpoint on @line, Ctrl-G R, and wait for the marked stop there."""
    _move_down(w, line - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _children(w, "python3"), START_TIMEOUT_S), \
        "pdb should be running:\n%s" % _screen(w)
    assert _wait_for(w, lambda: _mark_line(w) == line, STEP_TIMEOUT_S), \
        "pdb should stop on line %d; mark is on %s:\n%s" \
        % (line, _mark_line(w), _screen(w))


# @scenario SC-debug-19
@pytest.mark.parametrize("flood", sorted(PY_FLOOD_ROWS))
def test_pdb_steps_over_a_line_that_prints_more_than_the_reader_holds(
        tmp_path, flood):
    """F8 over a line that prints thousands of bytes (with or without a line
    end before pdb's prompt) moves the mark to the next line, the session
    stays open, and the next F8 still steps."""
    _need("python3")
    prog = _py_flood_prog(PY_FLOOD_ROWS[flood])
    with WpeSession(str(tmp_path), prog, filename="p.py", wait=1.5) as w:
        _start_py_session_on(w, PY_FLOOD_BREAK_LINE)
        assert _step(w, F8, PY_FLOOD_NEXT_LINE), \
            "F8 over the long output should stop on line %d; mark is on " \
            "%s:\n%s" % (PY_FLOOD_NEXT_LINE, _mark_line(w), _screen(w))
        assert _children(w, "python3"), \
            "pdb should still run after the long output:\n%s" % _screen(w)
        assert _step(w, F8, PY_FLOOD_LAST_LINE), \
            "the next F8 should stop on line %d:\n%s" \
            % (PY_FLOOD_LAST_LINE, _screen(w))


# A program that makes pdb print a prompt longer than any line the editor
# reads, with no line end before it: the shape of a program that prints a
# long line without a newline just before pdb asks for its next command.
# Every answer pdb gives from line 3 on ends that way.
PY_LONG_PROMPT_PROG = (
    "import gc, pdb\n"
    "[setattr(o, 'prompt', 'x' * 6000 + '(Pdb) ') for o in gc.get_objects()"
    " if isinstance(o, pdb.Pdb)]\n"
    "x = 3\n"
    "y = x * x\n"
    "print('DONE', y)\n"
)
PY_LONG_PROMPT_BREAK_LINE = 4     # y = x * x


# @scenario SC-debug-28
def test_pdb_watch_reads_an_answer_ended_by_a_prompt_after_a_long_line(
        tmp_path):
    """A watch is read even when pdb's prompt comes right after a line longer
    than the reader's buffer, with no line end between them: the Watches
    window shows the value, the editor stays responsive, and F8 refreshes it
    (the reader used to wait for a line end that never came)."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_LONG_PROMPT_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, PY_LONG_PROMPT_BREAK_LINE)
        _add_watch(w, "x")
        assert _wait_for(w, lambda: _watch_value(w, "x") == 3,
                         STEP_TIMEOUT_S), \
            "the Watches window should show x: 3:\n%s" % _screen(w)
        _add_watch(w, "y")          # y = x * x has not run yet
        assert _wait_for(w, lambda: _watch_row(w, "y") is not None,
                         STEP_TIMEOUT_S), \
            "the Watches window should show a row for y:\n%s" % _screen(w)
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _watch_value(w, "y") == 9,
                         STEP_TIMEOUT_S), \
            "after y = x * x ran, the watch should read 9:\n%s" % _screen(w)
        assert _children(w, "python3"), "pdb should still be running"


def _deep_dir(tmp_path, total):
    """A directory under nested 40-byte directories whose path is at least
    `total` bytes; created, and returned as a string."""
    path = str(tmp_path)
    while len(path) < total:
        path = os.path.join(path, "d" * 40)
    os.makedirs(path)
    return path


# The error stream line gdb writes when it declines a script that sits next
# to the program: it names the script's whole path, so in a deep directory it
# is longer than the buffer the start reads the error stream into.
GDB_DECLINED_SCRIPT = "p.e-gdb.gdb"
GDB_STDERR_PATH_BYTES = 400


# @scenario SC-debug-01
def test_gdb_start_reads_an_error_line_longer_than_its_buffer(tmp_path):
    """gdb warns on its error stream, naming a 400-byte path, while it
    starts: the session still stops, marked, on the breakpoint (the reader
    wrote one byte past its buffer on such a line; the sanitizer build is
    the one that sees it)."""
    _need("gcc", "gdb")
    deep = _deep_dir(tmp_path, GDB_STDERR_PATH_BYTES)
    with open(os.path.join(deep, GDB_DECLINED_SCRIPT), "w") as script:
        script.write("echo never run\\n\n")
    with WpeSession(deep, C_PROG, filename="p.c", wait=1.5) as w:
        _start_c_session(w)
        assert w.alive(), "the editor died while gdb started"
        assert _children(w, "gdb"), "gdb should be running while stopped"


def _watch_row(w, name):
    """The Watches window's text after "name: ", or None while it shows none."""
    pat = re.compile(r"(?:^|\s)%s: (\S.*)$" % re.escape(name))
    for row in w.display(drain=0):
        m = pat.search(row[1:].rstrip(" x^av"))
        if m:
            return m.group(1)
    return None


def _add_watch(w, expr):
    w.key(CTRL_G, "w", delay=0.8)
    for ch in expr:
        w.key(ch, delay=0.05)
    w.key("\r", delay=0.5)


# A C program with a function whose name is longer than the debugger
# readers' buffers (E_D_LINE_MAX, PATH_MAX + 512 bytes, and twice that for
# the reader of a run or a step): gdb prints a watch on a pointer to it, its
# frame in "bt" and a stop in it, each as one line of over 12000 bytes.  The
# name is pasted together by the preprocessor so that no source line is long
# and the source window never shows a run of z: only the name's end has them.
LONG_NAME_PROG = (
    "int leaf(int v)\n"
    "{\n"
    "  return v * v;\n"
    "}\n"
    "#define CAT_(a, b) a##b\n"
    "#define CAT(a, b) CAT_(a, b)\n"
    "#define A1 aa\n"
    + "".join("#define A%d CAT(A%d, A%d)\n" % (i, i - 1, i - 1)
              for i in range(2, 14))
    + "#define Z1 zz\n"
    + "".join("#define Z%d CAT(Z%d, Z%d)\n" % (i, i - 1, i - 1)
              for i in range(2, 8))
    + "#define LONG_NAME CAT(CAT(A13, A12), Z7)\n"
    "int LONG_NAME(int v)\n"
    "{\n"
    "  return leaf(v) + 1;\n"
    "}\n"
    "int (*g)(int) = LONG_NAME;\n"
    "int main(void)\n"
    "{\n"
    "  return g(3);\n"
    "}\n"
)
LONG_NAME_BREAK_LINE = 3          # return v * v; in leaf
LONG_NAME_END = "z" * 10          # the name's end, never on a source line
PGDN = "\033[6~"
PGDN_MAX = 400                    # more pages than the longest row fills


def _start_long_name_session(w):
    """Breakpoint in leaf, Ctrl-G R, and wait for the marked stop there."""
    _move_down(w, LONG_NAME_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == LONG_NAME_BREAK_LINE,
                     START_TIMEOUT_S), \
        "Ctrl-G R should stop on line %d:\n%s" \
        % (LONG_NAME_BREAK_LINE, _screen(w))


def _page_down_to(w, text):
    """Page the active window down until @text is on the screen or the window
    ends, and say whether it is.  The Watches and Stack windows take PgDn,
    not Ctrl-PgDn, so the paging stops where the screen stops changing."""
    before = None
    for _ in range(PGDN_MAX):
        screen = _screen(w)
        if text in screen or screen == before:
            break
        before = screen
        w.key(PGDN, delay=0.1)
    return text in _screen(w)


def _shows_the_long_name_end(w):
    """Page the active window down to the long name's last characters and
    say whether they are on the screen."""
    return _page_down_to(w, LONG_NAME_END)


# @scenario SC-debug-10
def test_gdb_watch_shows_a_value_longer_than_the_reader_holds(tmp_path):
    """A watch whose value gdb prints as one line of over 12000 bytes shows
    the whole value in the Watches window, its end included.  A guard on the
    whole-line read: a reader that cuts long lines fails it; the older
    reader, which handed such a line out in pieces the watch put back
    together, passed it too."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), LONG_NAME_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_long_name_session(w)
        _add_watch(w, "g")
        assert _wait_for(w, lambda: _watch_row(w, "g") is not None,
                         STEP_TIMEOUT_S), \
            "the Watches window should show a row for g:\n%s" % _screen(w)
        assert _shows_the_long_name_end(w), \
            "the end of g's value (the name's run of z) should be in the " \
            "Watches window:\n%s" % _screen(w)
        assert _children(w, "gdb"), "gdb should still be running"


# @scenario SC-debug-33
def test_gdb_stack_shows_a_row_longer_than_the_reader_holds(tmp_path):
    """A "bt" row of over 12000 bytes (a frame of a function with a very long
    name) is shown in the Stack window to its end, and the frame below it
    still follows it.  A guard on the whole-line read: a reader that cuts
    long lines fails it; the older reader, which handed the row out in
    pieces, passed it too."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), LONG_NAME_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_long_name_session(w)
        w.key(CTRL_G, "k", delay=0.8)
        assert _wait_for(w, lambda: " Stack " in _screen(w), STEP_TIMEOUT_S), \
            "Ctrl-G K should open the Stack window:\n%s" % _screen(w)
        assert _shows_the_long_name_end(w), \
            "the end of the long frame row (the name's run of z) should be " \
            "in the Stack window:\n%s" % _screen(w)
        assert _page_down_to(w, "in main ()"), \
            "main's frame should follow the long row:\n%s" % _screen(w)
        assert _children(w, "gdb"), "gdb should still be running"


# Lines of LONG_NAME_PROG inside the long-named function and in main.  gdb
# names a stop there with a line longer than the readers' buffers, its
# "at p.c:N" at the end: "Breakpoint 1, aaa...zzz (v=3) at .../p.c:30".
# gdb only keeps "at p.c:N" on that line when it does not wrap its output,
# which is what "set width 0" in ~/.gdbinit, a common setting, asks for.
LONG_NAME_BODY_LINE = 30          # return leaf(v) + 1;
LONG_NAME_CALL_LINE = 35          # return g(3); in main
LONG_NAME_ROWS = 60               # the source window shows every line
GDBINIT_NO_WRAP = "set width 0\n"


def _long_name_session(tmp_path, **kw):
    """A session on LONG_NAME_PROG whose gdb, like many users', never wraps
    a line (the test HOME is tmp_path)."""
    (tmp_path / ".gdbinit").write_text(GDBINIT_NO_WRAP)
    return WpeSession(str(tmp_path), LONG_NAME_PROG, filename="p.c",
                      wait=1.5, rows=LONG_NAME_ROWS, **kw)


def _stop_on(w, line):
    """Breakpoint on @line, Ctrl-G R, and wait for the marked stop there."""
    _move_down(w, line - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == line, START_TIMEOUT_S), \
        "Ctrl-G R should stop, marked, on line %d (mark on %s):\n%s" \
        % (line, _mark_line(w), _screen(w))


# @scenario SC-debug-01
def test_gdb_run_marks_a_stop_whose_line_is_longer_than_the_reader_holds(
        tmp_path):
    """A breakpoint inside a function whose name makes gdb's stop line over
    12000 bytes long: Ctrl-G R marks the line, read from the end of the stop
    line (it was cut, and the stop was not marked)."""
    _need("gcc", "gdb")
    with _long_name_session(tmp_path) as w:
        _stop_on(w, LONG_NAME_BODY_LINE)
        assert _children(w, "gdb"), "gdb should still be running"


# @scenario SC-debug-02
def test_gdb_step_into_a_function_whose_stop_line_is_longer_than_the_reader(
        tmp_path):
    """F7 from main into the long-named function marks its first line: the
    step's stop line is over 12000 bytes long and names the line at its
    end."""
    _need("gcc", "gdb")
    with _long_name_session(tmp_path) as w:
        _stop_on(w, LONG_NAME_CALL_LINE)
        assert _step(w, F7, LONG_NAME_BODY_LINE), \
            "F7 should step into the long-named function and mark line %d " \
            "(mark on %s):\n%s" % (LONG_NAME_BODY_LINE, _mark_line(w),
                                    _screen(w))


# A Python function renamed to over 12000 characters: pdb names a stop in it
# as "> .../p.py(2)aaa...zzz()", the position at the start of the line.
PY_LONG_NAME_PROG = (
    "def sq(v):\n"
    "    return v * v\n"
    "\n"
    "sq.__code__ = sq.__code__.replace(co_name='a' * 12000 + 'z' * 256)\n"
    "y = sq(3)\n"
    "print('DONE', y)\n"
)
PY_LONG_NAME_CALL_LINE = 5        # y = sq(3)
PY_LONG_NAME_BODY_LINE = 2        # return v * v


# @scenario SC-debug-19
def test_pdb_steps_through_a_function_whose_stop_line_is_longer(tmp_path):
    """F7 into a function whose name makes pdb's stop line over 12000 bytes
    long, then F8, marks its lines: pdb names the position at the start of
    the line, so the reader's head keeps it."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_LONG_NAME_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, PY_LONG_NAME_CALL_LINE)
        assert _step(w, F7, PY_CALLEE_LINE), \
            "F7 should stop on the def line (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        assert _step(w, F8, PY_LONG_NAME_BODY_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (PY_LONG_NAME_BODY_LINE, _mark_line(w), _screen(w))


def _py_print_then_step(text, end="\\n"):
    """A program that prints @text on line 3 between two plain lines."""
    return ("x = 1\n"
            "y = 2\n"
            "print(%r, end=\"%s\")\n"
            "z = 3\n" % (text, end))


# Programs whose answers from pdb hold "> name(N" outside its stop line: in
# the "-> source" line that follows it, in a return value pdb prints on the
# stop line itself ("> .../p.py(2)f()->'x > g(5)'"), or in the program's
# output, glued to the stop line or not.  Each row is (program, breakpoint
# line, then each key with the line it must mark).
PY_RETURN_ARROW_STEPS = [(F7, 1), (F8, 2), (F8, 2), (F8, 4)]
PY_ARROW_ROWS = {
    "comparison-with-a-call": (
        "def f(n):\n"
        "    return n\n"
        "x = 5\n"
        "if x > f(1):\n"
        "    print(x)\n",
        3, [(F8, 4), (F8, 5)]),
    "printed-arrow-and-call": (
        _py_print_then_step("> word(3)"), 2, [(F8, 3), (F8, 4)]),
    "printed-stop-like-text": (
        _py_print_then_step("> step(3)"), 2, [(F8, 3), (F8, 4)]),
    "printed-source-like-text": (
        _py_print_then_step("-> x > g(7)"), 2, [(F8, 3), (F8, 4)]),
    "output-glued-to-the-stop": (
        _py_print_then_step("abc > k(2)", end=""), 2, [(F8, 3), (F8, 4)]),
    "return-value-with-an-arrow-and-a-call": (
        "def f():\n"
        "    return \"x > g(5)\"\n"
        "y = f()\n"
        "print(y)\n",
        3, PY_RETURN_ARROW_STEPS),
    "return-value-with-a-source-arrow": (
        "def f():\n"
        "    return \"-> a > h(4)\"\n"
        "y = f()\n"
        "print(y)\n",
        3, PY_RETURN_ARROW_STEPS),
    # F7 into code run by exec() or eval(): pdb names it "<string>(0)" (3.14)
    # and the session steps on to the program's next line.
    "step-into-exec": (
        "x = 1\n"
        "exec(\"y = 2\\nz = 3\")\n"
        "w = 4\n",
        2, [(F7, 3)]),
    "step-into-eval": (
        "x = 1\n"
        "v = eval(\"x + 1\")\n"
        "w = 4\n",
        2, [(F7, 3)]),
}
KEY_NAMES = {F7: "F7", F8: "F8"}


# @scenario SC-debug-19
@pytest.mark.parametrize("row", sorted(PY_ARROW_ROWS))
def test_pdb_step_onto_a_line_holding_an_arrow_and_a_call(tmp_path, row):
    """Each step marks the line pdb stopped on: a "> f(1" in pdb's "-> source"
    line, in a return value on the stop line, or in the program's output is
    not taken for a stop in a file "f"."""
    _need("python3")
    prog, break_line, steps = PY_ARROW_ROWS[row]
    with WpeSession(str(tmp_path), prog, filename="p.py", wait=1.5) as w:
        _start_py_session_on(w, break_line)
        for key, line in steps:
            w.key(key, delay=0.3)
            assert _wait_for(w, lambda: _mark_line(w) == line,
                             STEP_TIMEOUT_S), \
                "%s should mark line %d (mark on %s):\n%s" \
                % (KEY_NAMES[key], line, _mark_line(w), _screen(w))


# A source under a directory whose name holds "(1)": pdb's stop line reads
# "> .../a b(1)/p.py(3)<module>()", with a "(digits)" before the line's own.
PY_PAREN_DIR = "a b(1)"


# @scenario SC-debug-19
def test_pdb_steps_in_a_source_under_a_directory_named_with_a_number(
        tmp_path):
    """F8 marks the next line of a Python file under "a b(1)/": the "(1)" of
    the directory is not taken for the line pdb stopped on."""
    _need("python3")
    os.makedirs(os.path.join(str(tmp_path), PY_PAREN_DIR))
    name = os.path.join(PY_PAREN_DIR, "p.py")
    with WpeSession(str(tmp_path), _py_print_then_step("hello"),
                    filename=name, wait=1.5) as w:
        _start_py_session_on(w, 2)
        assert _step(w, F8, 3), \
            "F8 should mark line 3 (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))


# A step over a line that prints 5 MB under pdb, which shares its output
# pipe with the program: pdb must wait for the editor to read, not fail.
PY_FLOOD_PROG = (
    "a = 0\n"
    "x = 1\n"
    "print('x' * 5000000)\n"
    "y = 2\n"
)
PY_FLOOD_TIMEOUT_S = 30


# @scenario SC-debug-19
def test_pdb_steps_over_a_line_that_prints_megabytes(tmp_path):
    """F8 over print('x' * 5000000) marks the next line and pdb still runs
    (pdb wrote to a non-blocking pipe, failed with BlockingIOError and the
    session ended as if the program had exited)."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_FLOOD_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, 2)
        assert _step(w, F8, 3), \
            "F8 should mark line 3 (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == 4, PY_FLOOD_TIMEOUT_S), \
            "F8 over the 5 MB print should mark line 4 (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        assert _children(w, "python3"), "pdb should still be running"


# A breakpoint whose gdb commands print a million-element array, about 8 MB
# in 100000 lines of 80 columns, in one burst, and go on.
FLOOD_COMMANDS_LINE = 6           # i = 1;  the printing breakpoint
FLOOD_STOP_LINE = 8               # i = 3;  the user's breakpoint
FLOOD_C_PROG = (
    "int big[1000000];\n"
    "int main(void)\n"
    "{\n"
    "  int i;\n"
    "  for (i = 0; i < 1000000; i++) big[i] = i;\n"
    "  i = 1;\n"
    "  i = 2;\n"
    "  i = 3;\n"
    "  return i;\n"
    "}\n"
)
FLOOD_GDBINIT = (
    "set breakpoint pending on\n"
    "set print elements unlimited\n"
    "set print repeats unlimited\n"
    "set max-value-size unlimited\n"
    "set width 80\n"
    "break p.c:%d\n"
    "commands\n"
    "silent\n"
    "print big\n"
    "continue\n"
    "end\n" % FLOOD_COMMANDS_LINE
)
FLOOD_TIMEOUT_S = 60
FLOOD_HOLD_S = 6                  # longer than gdb takes to print it all


def _hold_the_editor(w, seconds):
    """Stop the editor's own process for @seconds, as a long redraw or a
    slow terminal would, so whatever the debugger prints meanwhile has to
    wait in the pipe."""
    os.kill(w.proc.pid, signal.SIGSTOP)
    try:
        time.sleep(seconds)
    finally:
        os.kill(w.proc.pid, signal.SIGCONT)


# @scenario SC-debug-01
def test_gdb_run_through_megabytes_of_breakpoint_output_reaches_the_stop(
        tmp_path):
    """A breakpoint's commands print 100000 lines (about 8 MB) and continue
    while the editor is busy elsewhere: once it reads again, the session
    reads them all, stops on the next breakpoint, marked, and gdb still runs.
    The editor is held stopped during the burst so the pipe really fills:
    gdb wrote to a non-blocking pipe and dropped what did not fit, the stop
    line among it, and the session waited for ever."""
    _need("gcc", "gdb")
    (tmp_path / ".gdbinit").write_text(FLOOD_GDBINIT)
    with WpeSession(str(tmp_path), FLOOD_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _move_down(w, FLOOD_STOP_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=0.2)
        assert _wait_for(w, lambda: "Starting program" in _screen(w),
                         START_TIMEOUT_S), \
            "gdb should start the program:\n%s" % _screen(w)
        _hold_the_editor(w, FLOOD_HOLD_S)
        assert _wait_for(w, lambda: _mark_line(w) == FLOOD_STOP_LINE,
                         FLOOD_TIMEOUT_S), \
            "the run should stop, marked, on line %d (mark on %s):\n%s" \
            % (FLOOD_STOP_LINE, _mark_line(w), _screen(w))
        assert _children(w, "gdb"), "gdb should still be running"


# A program that is running, not stopped: it waits for a line on its stdin.
BLOCKED_C_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  char line[100];\n"
    "  printf(\"READY\\n\");\n"
    "  if (fgets(line, sizeof line, stdin))\n"
    "    printf(\"GOT:%s\", line);\n"
    "  return 0;\n"
    "}\n"
)


BLOCKED_BREAK_LINE = 8            # return 0;  after the line is read


def _start_blocked_run(w):
    """A breakpoint after the read, then Ctrl-G R: gdb runs the program,
    which waits on its stdin; return once it printed READY, so it is
    running, not stopped."""
    _move_down(w, BLOCKED_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=0.2)
    assert _wait_for(w, lambda: "READY" in _screen(w), START_TIMEOUT_S), \
        "the program should be running and print READY:\n%s" % _screen(w)


# @scenario SC-debug-18
def test_debug_quit_from_the_menu_while_the_program_runs_ends_the_session(
        tmp_path):
    """Debug > Quit (Alt-D, Q) while the program is running ends gdb and
    leaves the editor usable. The menu key was typed into the program's
    console as a character (Alt-D arrived as a g with a breve), so gdb ran
    on and Messages showed that character."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), BLOCKED_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_blocked_run(w)
        assert _children(w, "gdb"), "gdb should be running"
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Debug > Quit should end gdb while the program runs:\n%s" \
            % _screen(w)
        assert "ğ" not in _screen(w), \
            "the menu key must not be typed into the program:\n%s" % _screen(w)
        w.key("Q", delay=0.3)
        w.save()
        assert "Q" in w.text(), \
            "typing after the session should edit the source"


# @scenario SC-debug-18
def test_ctrl_g_q_while_the_program_runs_ends_the_session(tmp_path):
    """Ctrl-G Q (Quit) ends a running session too."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), BLOCKED_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_blocked_run(w)
        w.key(CTRL_G, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Ctrl-G Q should end gdb while the program runs:\n%s" \
            % _screen(w)


# @scenario SC-debug-35
def test_text_typed_while_the_program_runs_reaches_the_program(tmp_path):
    """The user types a line, including a character above Latin-1, and
    Enter while the program waits for input: the program reads and prints
    it back. Only the editor's own keys stay with the editor."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), BLOCKED_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_blocked_run(w)
        w.key("h", "i", "ğ", delay=0.15)
        w.key("\r", delay=0.3)
        assert _wait_for(w, lambda: "GOT:hiğ" in _screen(w),
                         STEP_TIMEOUT_S), \
            "the program should read what was typed:\n%s" % _screen(w)


RUNNING_REFUSAL = "The program is running"
BLOCKED_AFTER_LINE = 9            # }  the line after BLOCKED_BREAK_LINE


def _answer_the_blocked_program(w):
    """Type the line the blocked program waits for and wait for its stop."""
    w.key("x", delay=0.15)
    w.key("\r", delay=0.3)
    assert _wait_for(w, lambda: _mark_line(w) == BLOCKED_BREAK_LINE,
                     STEP_TIMEOUT_S), \
        "the run should stop on line %d (mark on %s):\n%s" \
        % (BLOCKED_BREAK_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-36
def test_a_step_while_the_program_runs_is_refused_and_the_session_keeps_its_place(
        tmp_path):
    """F8 while the program waits for input says so on the status row and
    sends gdb nothing: after the program is answered and stops, the next F8
    marks the following line (the step used to go to gdb mid-run, and the
    next F8 then ended the session)."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), BLOCKED_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_blocked_run(w)
        w.key(F8, delay=0.5)
        assert RUNNING_REFUSAL in _screen(w), \
            "F8 should be refused with a line on the status row:\n%s" \
            % _screen(w)
        _answer_the_blocked_program(w)
        assert _step(w, F8, BLOCKED_AFTER_LINE), \
            "the next F8 should mark line %d (mark on %s):\n%s" \
            % (BLOCKED_AFTER_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-36
def test_show_stack_while_the_program_runs_is_refused(tmp_path):
    """Ctrl-G K while the program runs says so on the status row and opens
    no Stack window; gdb is asked nothing until the program stops."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), BLOCKED_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_blocked_run(w)
        w.key(CTRL_G, "k", delay=0.8)
        assert RUNNING_REFUSAL in _screen(w), \
            "Ctrl-G K should be refused with a line on the status row:\n%s" \
            % _screen(w)
        assert " Stack " not in _screen(w), \
            "no Stack window should open:\n%s" % _screen(w)
        _answer_the_blocked_program(w)


# The build keys of the Run menu. Each one asks whether to end the debug
# session first, as Turbo C asked to reset the program before a rebuild.
F9 = "\033[20~"                   # Make
ALT_C = "\033c"                   # Compile
ALT_U = "\033u"                   # Run (Ctrl-F9)
ALT_A = "\033a"                   # Execute Make
QUIT_DEBUGGING_QUESTION = "Quit Debugging"


def _ask_to_build_and_answer(w, key, answer):
    """Press a build key during a session, wait for the question whether to
    end the session, and answer it."""
    w.key(key, delay=0.5)
    assert _wait_for(w, lambda: QUIT_DEBUGGING_QUESTION in _screen(w),
                     STEP_TIMEOUT_S), \
        "the build key should ask whether to quit debugging:\n%s" % _screen(w)
    w.key(answer, delay=0.6)


# @scenario SC-debug-37
def test_a_build_declined_while_the_program_runs_keeps_the_session(tmp_path):
    """F9 while the program waits for input, answered No: the session goes
    on as if F9 had not been pressed. The program, once answered, stops on
    the breakpoint with the line marked, and F8 steps (the build used to
    drop gdb's answers before asking, so the stop was never seen and every
    debugger key said the program was running until Debug > Quit)."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), BLOCKED_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _start_blocked_run(w)
        _ask_to_build_and_answer(w, F9, "n")
        assert _children(w, "gdb"), \
            "No should leave gdb running:\n%s" % _screen(w)
        _answer_the_blocked_program(w)
        assert _step(w, F8, BLOCKED_AFTER_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (BLOCKED_AFTER_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-37
@pytest.mark.parametrize("key,answer", [
    (F9, "n"), (F9, ESC), (ALT_C, "n"), (ALT_U, "n"), (ALT_A, "n"),
], ids=["f9-no", "f9-esc", "alt-c-no", "alt-u-no", "alt-a-no"])
def test_a_build_declined_at_a_stop_keeps_the_session(tmp_path, key, answer):
    """A build key at a stop, answered No or Esc: the mark stays and F8
    steps to the next line (the build used to drop gdb's answers before
    asking, and F8 then said it could not watch the debugger's answer)."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        _ask_to_build_and_answer(w, key, answer)
        assert _mark_line(w) == C_BREAK_LINE and _children(w, "gdb"), \
            "declining should leave the session at its stop:\n%s" % _screen(w)
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (C_NEXT_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-37
def test_a_build_accepted_at_a_stop_ends_the_session_and_builds(tmp_path):
    """F9 at a stop, answered Yes: gdb ends, the mark goes and the build
    runs (the program was just built, so it says there is nothing to
    build)."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        _ask_to_build_and_answer(w, F9, "y")
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Yes should end gdb:\n%s" % _screen(w)
        assert _wait_for(w, lambda: "nothing to build" in _screen(w),
                         STEP_TIMEOUT_S), \
            "the build should run after the session ends:\n%s" % _screen(w)
        assert _mark_line(w) is None, \
            "the current-line mark should go:\n%s" % _screen(w)


# @scenario SC-debug-37
def test_a_build_declined_at_a_pdb_stop_keeps_the_session(tmp_path):
    """Parity with gdb: F9 at a pdb stop, answered No, leaves pdb at its
    stop and F8 steps to the next line."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _start_py_session(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         START_TIMEOUT_S), \
            "pdb should stop on line %d:\n%s" % (PY_BREAK_LINE, _screen(w))
        _ask_to_build_and_answer(w, F9, "n")
        assert _children(w, "python3"), \
            "No should leave pdb running:\n%s" % _screen(w)
        assert _step(w, F8, PY_NEXT_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (PY_NEXT_LINE, _mark_line(w), _screen(w))


# Programs that print, then sleep for ten minutes. The breakpoint is on the
# print: after the stop there, Ctrl-G R leaves the program running (not
# stopped) for as long as the test needs.
SLEEPING_PROGS = {
    "pdb": {"file": "p.py", "line": 2, "child": "python3", "env": {},
            "need": ("python3",), "start_s": 15,
            "prog": "import time\nprint('ready', flush=True)\n"
                    "time.sleep(600)\nx = 1\n"},
    "jdb": {"file": "P.java", "line": 3, "child": "jdb", "env": {},
            "need": ("jdb", "javac"), "start_s": 30,
            "prog": "public class P {\n"
                    "    public static void main(String[] a) throws Exception {\n"
                    "        System.out.println(\"ready\");\n"
                    "        Thread.sleep(600000);\n"
                    "        int x = 3;\n"
                    "    }\n}\n"},
    "dlv": {"file": "main.go", "line": 9, "child": "dlv", "env": {},
            "need": ("dlv", "go"), "start_s": 40,
            "prog": "package main\n\nimport (\n\t\"fmt\"\n\t\"time\"\n)\n\n"
                    "func main() {\n\tfmt.Println(\"ready\")\n"
                    "\ttime.Sleep(600 * time.Second)\n\tx := 3\n"
                    "\tfmt.Println(x)\n}\n"},
    "lldb-dap": {"file": "main.rs", "line": 2, "child": "lldb-dap",
                 "env": {"XWPE_DAP_ADAPTER": "lldb"},
                 "need": ("rustc", "lldb-dap"), "start_s": 60,
                 "prog": "fn main() {\n    println!(\"ready\");\n"
                         "    std::thread::sleep("
                         "std::time::Duration::from_secs(600));\n"
                         "    let x = 3;\n    println!(\"{}\", x);\n}\n"},
}
RUNNING_SETTLE_S = 10   # the program is asleep, past any start-up output


# @scenario SC-debug-18
def test_debug_quit_while_the_program_runs_under_a68g(tmp_path):
    """a68g reads the keyboard while the program runs: Debug > Quit ends it.
    (A busy loop stands in for the sleep, which a68g has no call for.)"""
    _need("a68g")
    prog = ("print ((\"rea\" + \"dy\", new line));\nINT n := 0;\n"
            "WHILE n < 2000000000 DO n +:= 1 OD;\nINT x := 3;\n"
            "print ((x, new line))\n")
    with WpeSession(str(tmp_path), prog, filename="p.a68", wait=2.0) as w:
        _move_down(w, 3)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _children(w, "a68g"), START_TIMEOUT_S), \
            "a68g should be running:\n%s" % _screen(w)
        assert _wait_for(w, lambda: "ready" in _screen(w), START_TIMEOUT_S), \
            "the program should have started and printed:\n%s" % _screen(w)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "a68g"), STEP_TIMEOUT_S), \
            "Debug > Quit should end a68g while the program runs:\n%s" \
            % _screen(w)


# @scenario SC-debug-18
@pytest.mark.parametrize("debugger", [
    "jdb",
    pytest.param("pdb", marks=incoherence(
        "pdb's continue is read by e_read_output, whose key reader "
        "(e_d_getchar) only knows Ctrl-C and writes every other byte to "
        "pdb's stdin, so Alt-D q reaches pdb as ESC d q")),
    pytest.param("dlv", marks=incoherence(
        "the adapter's run waits in poll() on the adapter's descriptors "
        "only, so no editor key is read while the program runs")),
    pytest.param("lldb-dap", marks=incoherence(
        "the adapter's run waits in poll() on the adapter's descriptors "
        "only, so no editor key is read while the program runs")),
])
def test_debug_quit_while_the_program_runs_under_every_debugger(
        tmp_path, debugger):
    """Parity with gdb: Debug > Quit (Alt-D, Q) ends the session while the
    program is running, whichever debugger runs it."""
    spec = SLEEPING_PROGS[debugger]
    _need(*spec["need"])
    if debugger == "dlv":
        (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), spec["prog"], filename=spec["file"],
                    wait=2.0, env_extra=spec["env"] or None) as w:
        _move_down(w, spec["line"] - 1)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == spec["line"],
                         spec["start_s"]), \
            "%s should stop on line %d:\n%s" \
            % (spec["child"], spec["line"], _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        w._drain(RUNNING_SETTLE_S)   # asleep for ten minutes (drain scales)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, spec["child"]),
                         STEP_TIMEOUT_S), \
            "Debug > Quit should end %s while the program runs:\n%s" \
            % (spec["child"], _screen(w))


# A program that sleeps between two breakpoints, so it is running, not
# stopped, for a few seconds after a continue.
SLEEPY_C_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  sleep(4);\n"
    "  x = x + 1;\n"
    "  printf(\"%d\\n\", x);\n"
    "  return 0;\n"
    "}\n"
)
SLEEPY_FIRST_LINE = 5     # int x = 3;
SLEEPY_AFTER_LINE = 7     # x = x + 1;  after the sleep
SLEEPY_RUN_S = 4
CTRL_W = "\x17"           # close the window on top


# @scenario SC-debug-18
def test_closing_the_window_while_the_program_runs_keeps_the_editor(
        tmp_path):
    """Ctrl-W closes the source window while gdb's program sleeps on its
    way to the next breakpoint: when it stops, the stop is marked in the
    file opened again, and the editor and gdb are still there.  The stop
    used to be shown through the window that was closed, freed memory."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _move_down(w, SLEEPY_FIRST_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        _move_down(w, SLEEPY_AFTER_LINE - SLEEPY_FIRST_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == SLEEPY_FIRST_LINE,
                         START_TIMEOUT_S), \
            "gdb should stop on line %d:\n%s" % (SLEEPY_FIRST_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        w.key(CTRL_W, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == SLEEPY_AFTER_LINE,
                         STEP_TIMEOUT_S + SLEEPY_RUN_S), \
            "the stop on line %d should be marked after the window closed " \
            "(editor alive: %s):\n%s" \
            % (SLEEPY_AFTER_LINE, w.alive(), _screen(w))
        assert w.alive(), "the editor should still run"
        assert _children(w, "gdb"), "gdb should still run the session"


C_CURSOR_FIRST_LINE = 11  # x = y + 1;  the cursor for Go to cursor
C_MAIN_LINE = 8           # the first line of main, where the start stops


def _go_to_cursor_as_the_first_command(w):
    """No breakpoint, cursor on line 11, Ctrl-G U before any session."""
    _move_down(w, C_CURSOR_FIRST_LINE - 1)
    w.key(CTRL_G, "u", delay=1.0)


# @scenario SC-debug-05
def test_go_to_cursor_as_the_first_command_starts_a_session_that_works(
        tmp_path):
    """Go to cursor before any session starts gdb and stops at main, and
    the session goes on: F8 marks the next line.  The session used to wait
    for ever: the start breakpoint's removal was sent while the program
    ran, and its answer swallowed the stop."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _go_to_cursor_as_the_first_command(w)
        assert _wait_for(w, lambda: _mark_line(w) == C_MAIN_LINE,
                         START_TIMEOUT_S), \
            "Go to cursor should start the session and stop on line %d " \
            "(mark on %s):\n%s" % (C_MAIN_LINE, _mark_line(w), _screen(w))
        assert _children(w, "gdb"), "gdb should still run the session"
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (C_NEXT_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-05
@incoherence("Go to cursor as the first command runs to main and stops "
             "there, as the start of any session does; reaching the cursor "
             "needs gdb's until on the run controller")
def test_go_to_cursor_as_the_first_command_stops_at_the_cursor(tmp_path):
    """Borland's F4 before any session runs to the cursor's line."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _go_to_cursor_as_the_first_command(w)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_FIRST_LINE,
                         START_TIMEOUT_S), \
            "Go to cursor should stop on line %d (mark on %s):\n%s" \
            % (C_CURSOR_FIRST_LINE, _mark_line(w), _screen(w))


def _cursor_row_text(w):
    """The screen row the cursor is on."""
    return w.display(drain=0)[w.screen.cursor.y]


# @scenario SC-debug-33
def test_gdb_stack_enter_follows_a_row_longer_than_the_reader_holds(
        tmp_path):
    """Stopped in leaf, Ctrl-G K, Enter on the frame of the long-named
    function (a row of over 12000 bytes, wrapped over many window lines,
    its path among them) marks the call in it (the row was refused as having
    no source)."""
    _need("gcc", "gdb")
    with _long_name_session(tmp_path, cols=STACK_COLS) as w:
        _stop_on(w, LONG_NAME_BREAK_LINE)
        w.key(CTRL_G, "k", delay=0.8)
        assert _wait_for(w, lambda: "#1  " in _screen(w), STEP_TIMEOUT_S), \
            "Ctrl-G K should list the long frame:\n%s" % _screen(w)
        for _ in range(PGDN_MAX):
            if "#1  " in _cursor_row_text(w):
                break
            w.key(DOWN, delay=0.1)
        assert "#1  " in _cursor_row_text(w), \
            "the cursor should reach the long frame's row:\n%s" % _screen(w)
        w.key("\r", delay=0.8)
        assert _wait_for(w, lambda: _mark_line(w) == LONG_NAME_BODY_LINE,
                         STEP_TIMEOUT_S), \
            "Enter on the long frame should mark line %d (mark on %s):\n%s" \
            % (LONG_NAME_BODY_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-28
def test_pdb_watch_shows_its_value_and_refreshes_at_the_next_stop(tmp_path):
    """Under pdb a watch opens the Watches window with pdb's answer, the
    editor stays responsive, and F8 refreshes it to the new value."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _start_py_session(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         STEP_TIMEOUT_S), _screen(w)
        _add_watch(w, "x")          # x = 3 has not run yet
        assert _wait_for(w, lambda: _watch_row(w, "x") is not None,
                         STEP_TIMEOUT_S), \
            "the Watches window should show a row for x:\n%s" % _screen(w)
        assert _watch_value(w, "x") is None, \
            "x is not assigned yet, so no number should show:\n%s" % _screen(w)
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _watch_value(w, "x") == 3, STEP_TIMEOUT_S), \
            "after x = 3 ran, the watch should read 3:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_NEXT_LINE,
                         STEP_TIMEOUT_S), \
            "the mark should be on line %d of p.py, not in Watches:\n%s" \
            % (PY_NEXT_LINE, _screen(w))


# Line numbers below are 1-based source lines of JAVA_PROG.
JAVA_PROG = (
    "public class P {\n"
    "    static int sq(int v) {\n"
    "        return v * v;\n"
    "    }\n"
    "    public static void main(String[] a) {\n"
    "        int x = 3;\n"
    "        int y = sq(x);\n"
    "        System.out.println(\"OUTMARK \" + y);\n"
    "        x = y + 1;\n"
    "    }\n"
    "}\n"
)
JAVA_BREAK_LINE = 6       # int x = 3;
JAVA_NEXT_LINE = 7        # int y = sq(x);
JAVA_AFTER_SQ_LINE = 8    # System.out.println(...)
JDB_START_TIMEOUT_S = 25  # javac + the JVM under jdb, before scaling


def _messages_rows(w):
    """Text and backgrounds of the Messages window's rows, for comparing
    before and after a stop; empty when Messages is not on screen."""
    disp = w.display(drain=0)
    rows = []
    inside = False
    for y, line in enumerate(disp):
        if " Messages " in line:
            inside = True
            continue
        if inside and line.startswith("m"):
            break
        if inside:
            bgs = tuple(w.screen.buffer[y][x].bg
                        for x in range(1, w.screen.columns - 1))
            rows.append((line, bgs))
    return rows


def _start_java_session(w):
    """Breakpoint on JAVA_BREAK_LINE, Ctrl-G R; the Java file switches the
    backend to jdb and says so on the status row, then wait for the stop."""
    _move_down(w, JAVA_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _status_row_says(w, "switching to jdb", STEP_TIMEOUT_S)
    assert _wait_for(w, lambda: _children(w, "jdb"), START_TIMEOUT_S), \
        "jdb should be running:\n%s" % _screen(w)
    assert _wait_for(w, lambda: _mark_line(w) == JAVA_BREAK_LINE,
                     JDB_START_TIMEOUT_S), \
        "jdb stopped; line %d of P.java should be marked:\n%s" \
        % (JAVA_BREAK_LINE, _screen(w))


# @scenario SC-debug-20
def test_jdb_stop_marks_the_source_window_never_messages(tmp_path):
    """jdb's breakpoint stop and the next F8 put the green bar in P.java;
    the Messages window below keeps its text and colours."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5) as w:
        _start_java_session(w)
        messages = _messages_rows(w)
        assert messages, "Messages should be on screen:\n%s" % _screen(w)
        assert _step(w, F8, JAVA_NEXT_LINE), \
            "F8 should move the mark to line %d of P.java:\n%s" \
            % (JAVA_NEXT_LINE, _screen(w))
        assert _messages_rows(w) == messages, \
            "the stop changed Messages (a mark painted there?):\n%s" % _screen(w)


# @scenario SC-debug-28
def test_jdb_watch_shows_its_value_and_refreshes_at_the_next_stop(tmp_path):
    """Under jdb a watch shows jdb's print answer and F8 refreshes it."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5) as w:
        _start_java_session(w)
        assert _step(w, F8, JAVA_NEXT_LINE), _screen(w)
        _add_watch(w, "x")
        assert _wait_for(w, lambda: _watch_value(w, "x") == 3, STEP_TIMEOUT_S), \
            "the Watches window should show x: 3:\n%s" % _screen(w)
        _add_watch(w, "y")          # int y = sq(x) has not run yet
        assert _wait_for(w, lambda: _watch_row(w, "y") is not None,
                         STEP_TIMEOUT_S), \
            "the Watches window should show a row for y:\n%s" % _screen(w)
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _watch_value(w, "y") == 9, STEP_TIMEOUT_S), \
            "after y = sq(x) ran, the watch should read 9:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == JAVA_AFTER_SQ_LINE,
                         STEP_TIMEOUT_S), \
            "the mark should be on line %d of P.java:\n%s" \
            % (JAVA_AFTER_SQ_LINE, _screen(w))


# Line numbers below are 1-based source lines of RUST_PROG.
RUST_PROG = (
    "fn sq(v: i32) -> i32 {\n"
    "    v * v\n"
    "}\n"
    "fn main() {\n"
    "    let mut x = 3;\n"
    "    let y = sq(x);\n"
    "    println!(\"OUTMARK {}\", y);\n"
    "    x = y + 1;\n"
    "    println!(\"DONE {}\", x);\n"
    "}\n"
)
RUST_ONCE_LINE = 5          # let mut x = 3;  runs exactly once
RUST_LAST_OUTPUT = "DONE 10"  # what the program prints last, x = 3*3 + 1
RUST_START_TIMEOUT_S = 30   # rustc + the adapter, before scaling


# @scenario SC-debug-23
@pytest.mark.parametrize("adapter,binary", [("gdb", "gdb"), ("lldb", "lldb-dap")])
def test_rust_dap_stops_on_a_breakpoint_on_a_line_that_runs_once(
        tmp_path, adapter, binary):
    """Under either DAP adapter, Ctrl-G R stops on a breakpoint whose line
    runs only once: the editor never continues a stop the user asked for."""
    _need("rustc", binary)
    with WpeSession(str(tmp_path), RUST_PROG, filename="main.rs", wait=2.0,
                    env_extra={"XWPE_DAP_ADAPTER": adapter}) as w:
        _move_down(w, RUST_ONCE_LINE - 1)
        w.key(CTRL_G, "b", delay=0.8)
        assert RUST_ONCE_LINE in _breakpoint_lines(w), _screen(w)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == RUST_ONCE_LINE,
                         RUST_START_TIMEOUT_S), \
            "Ctrl-G R should stop on line %d (%s):\n%s" \
            % (RUST_ONCE_LINE, binary, _screen(w))
        assert RUST_LAST_OUTPUT not in _screen(w), \
            "the program ran to its end past the breakpoint:\n%s" % _screen(w)


def _status_field_line(w):
    """The line number in the topmost source window's line:column field."""
    for row in w.display(drain=0)[2:]:
        if row.startswith("m"):
            m = re.search(r"\s(\d+):\d+\s", row)
            return int(m.group(1)) if m else None
    return None


# @scenario SC-debug-04
def test_the_line_column_field_follows_every_stop(tmp_path):
    """After F8 and F7 the window's line:column field names the line the
    green bar is on, without a key being pressed after the stop."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        for key, line in ((F8, C_NEXT_LINE), (F7, C_CALLEE_LINE)):
            assert _step(w, key, line), _screen(w)
            assert _wait_for(w, lambda: _status_field_line(w) == line,
                             STEP_TIMEOUT_S), \
                "the mark is on line %d, the field says %s:\n%s" \
                % (line, _status_field_line(w), _screen(w))


# @scenario SC-debug-14
def test_the_end_of_the_program_names_its_exit_code(tmp_path):
    """A C program that returns 3, run to its end: Messages says "Program
    exited with code 3" (gdb prints the code in octal)."""
    _need("gcc", "gdb")
    prog = C_PROG.replace("return 0;", "return 11;")
    with WpeSession(str(tmp_path), prog, filename="p.c", wait=1.5) as w:
        _start_c_session(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: "Program exited" in _screen(w),
                         STEP_TIMEOUT_S), _screen(w)
        assert "Program exited with code 11" in _screen(w), \
            "Messages should name the exit code 11 (gdb says 013):\n%s" \
            % _screen(w)


# @scenario SC-debug-13
def test_a_normal_end_says_exit_code_zero(tmp_path):
    """gdb's "exited normally" is read as exit code 0."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: "Program exited with code 0" in _screen(w),
                         STEP_TIMEOUT_S), _screen(w)


PY_EXIT_PROG = (
    "import sys\n"
    "x = 3\n"
    "sys.exit(x)\n"
)
PY_EXIT_BREAK_LINE = 2


# @scenario SC-debug-14
def test_pdb_names_the_code_of_sys_exit(tmp_path):
    """A Python script that ends with sys.exit(3), continued from a
    breakpoint: Messages says "Program exited with code 3"."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_EXIT_PROG, filename="p.py",
                    wait=1.5) as w:
        _move_down(w, PY_EXIT_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _status_row_says(w, "switching to pdb", STEP_TIMEOUT_S)
        assert _wait_for(w, lambda: _mark_line(w) == PY_EXIT_BREAK_LINE,
                         START_TIMEOUT_S), _screen(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: "Program exited with code 3" in _screen(w),
                         STEP_TIMEOUT_S), \
            "Messages should name the exit code 3:\n%s" % _screen(w)


# @scenario SC-debug-16
def test_a_debugger_that_dies_is_named_with_its_exit_status(tmp_path):
    """gdb killed (SIGKILL) while stopped, then F8: Messages and the status
    row say "gdb ended unexpectedly (exit status 137)", never "End of
    Debugging", and the session is gone."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        pids = _children(w, "gdb")
        assert len(pids) == 1, "expected one gdb child, saw %s" % pids
        os.kill(pids[0], signal.SIGKILL)
        w.key(F8, delay=0.5)
        expected = "gdb ended unexpectedly (exit status %d)" \
            % (128 + signal.SIGKILL)
        assert _wait_for(w, lambda: expected in _screen(w), STEP_TIMEOUT_S), \
            "Messages should say the debugger died:\n%s" % _screen(w)
        assert expected in w.display(drain=0)[-1], \
            "the status row should say it too:\n%s" % _screen(w)
        assert "End of Debugging" not in _screen(w), _screen(w)
        assert _mark_line(w) is None, _screen(w)
        assert not _children(w, "gdb")


# @scenario SC-debug-12
def test_dap_program_output_has_no_carriage_return_mark(tmp_path):
    """Under lldb-dap the program's lines reach Messages without the "^M"
    the pty's CR LF used to leave at their end."""
    if not tool_usable("rustc"):
        pytest.skip("no rustc")
    _need("lldb-dap")
    with WpeSession(str(tmp_path), RUST_PROG, filename="main.rs", wait=2.0,
                    env_extra={"XWPE_DAP_ADAPTER": "lldb"}) as w:
        _move_down(w, RUST_ONCE_LINE - 1)
        w.key(CTRL_G, "b", delay=0.8)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == RUST_ONCE_LINE,
                         RUST_START_TIMEOUT_S), _screen(w)
        w.key(CTRL_G, "r", delay=1.0)       # on to the end
        assert _wait_for(w, lambda: RUST_LAST_OUTPUT in _screen(w),
                         RUST_START_TIMEOUT_S), _screen(w)
        rows = [r for r in w.display(drain=0) if "OUTMARK 9" in r or
                RUST_LAST_OUTPUT in r]
        assert rows and not any("^M" in r for r in rows), \
            "program output shows a literal ^M:\n%s" % _screen(w)


# Line numbers below are 1-based source lines of the files they name.
JAVA_PKG_MAIN = (
    "import com.x.Q;\n"
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 3;\n"
    "        int y = Q.sq(x);\n"
    "        int z = R.one();\n"
    "        System.out.println(\"OUTMARK \" + (y + z));\n"
    "    }\n"
    "}\n"
    "class R {\n"
    "    static int one() {\n"
    "        return 1;\n"
    "    }\n"
    "}\n"
)
JAVA_PKG_CLASS = (
    "package com.x;\n"
    "public class Q {\n"
    "    public static int sq(int v) {\n"
    "        return v * v;\n"
    "    }\n"
    "}\n"
)
JAVA_PKG_CALL_LINE = 5      # int y = Q.sq(x);
JAVA_PKG_CALLEE_TEXT = "return v * v;"   # line 4 of com/x/Q.java
JAVA_SECOND_CALL_LINE = 6   # int z = R.one();
JAVA_SECOND_CLASS_NOTE = "R: source not found"


def _marked_text(w):
    """The screen text of the row carrying the current-line mark, wherever
    the marked window sits (a file the debugger opens is not at the top
    left), or "" when no row is marked."""
    for y in range(1, w.screen.lines - 1):
        row = w.screen.buffer[y]
        if any(row[x].bg == CURRENT_LINE_BG
               for x in range(1, w.screen.columns - 1)):
            return w.display(drain=0)[y]
    return ""


def _java_package_session(tmp_path, break_line):
    """P.java in the work directory calling com.x.Q from com/x/Q.java (and
    R, a second class written in P.java); stopped on break_line."""
    pkg = tmp_path / "com" / "x"
    pkg.mkdir(parents=True)
    (pkg / "Q.java").write_text(JAVA_PKG_CLASS)
    w = WpeSession(str(tmp_path), JAVA_PKG_MAIN, filename="P.java", wait=1.5)
    try:
        _move_down(w, break_line - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _status_row_says(w, "switching to jdb", STEP_TIMEOUT_S)
        assert _wait_for(w, lambda: _mark_line(w) == break_line,
                         JDB_START_TIMEOUT_S), _screen(w)
    except BaseException:
        w.close()
        raise
    return w


# @scenario SC-debug-20
def test_jdb_stop_in_a_package_class_opens_its_source(tmp_path):
    """F7 into com.x.Q.sq() opens com/x/Q.java, which is neither open nor
    in the work directory, and marks the line; no "Can't find file"."""
    _need("jdb", "javac")
    with _java_package_session(tmp_path, JAVA_PKG_CALL_LINE) as w:
        w.key(F7, delay=0.3)
        assert _wait_for(w, lambda: JAVA_PKG_CALLEE_TEXT in _marked_text(w),
                         STEP_TIMEOUT_S), \
            "F7 should mark \"%s\" in com/x/Q.java:\n%s" \
            % (JAVA_PKG_CALLEE_TEXT, _screen(w))
        assert "package com.x;" in _screen(w), _screen(w)
        assert "Can't find" not in _screen(w), _screen(w)


# @scenario SC-debug-20
def test_jdb_stop_without_source_says_so_once_and_runs_on(tmp_path):
    """F7 into R, a second class written in P.java: the status row says
    "R: source not found", no dialog opens at this or the next stop, and
    the session goes on."""
    _need("jdb", "javac")
    with _java_package_session(tmp_path, JAVA_SECOND_CALL_LINE) as w:
        w.key(F7, delay=0.3)
        assert _wait_for(w, lambda: JAVA_SECOND_CLASS_NOTE in
                         w.display(drain=0)[-1], STEP_TIMEOUT_S), \
            "the status row should say the source is missing:\n%s" % _screen(w)
        assert "Can't find" not in _screen(w), _screen(w)
        w.key(F8, delay=0.3)
        w._drain(2.0)
        assert "Can't find" not in _screen(w), \
            "the next stop in R raised a dialog:\n%s" % _screen(w)
        assert _children(w, "jdb"), "the session should still run"


PY_WATCH_PROG = (
    "class M:\n"
    "    def __repr__(self):\n"
    "        return 'M(\\n  first,\\n  second)'\n"
    "m = M()\n"
    "x = 3\n"
    "y = 4\n"
)
PY_WATCH_BREAK_LINE = 6


# @scenario SC-debug-28
def test_pdb_one_line_watch_after_a_multi_line_one(tmp_path):
    """Two watches under pdb, the first answered over three lines, the
    second over one: both rows show their whole answer."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_WATCH_PROG, filename="p.py",
                    wait=1.5) as w:
        _move_down(w, PY_WATCH_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _status_row_says(w, "switching to pdb", STEP_TIMEOUT_S)
        assert _wait_for(w, lambda: _mark_line(w) == PY_WATCH_BREAK_LINE,
                         START_TIMEOUT_S), _screen(w)
        _add_watch(w, "m")
        _add_watch(w, "x")
        assert _wait_for(w, lambda: _watch_value(w, "x") == 3,
                         STEP_TIMEOUT_S), \
            "the one-line watch should read x: 3:\n%s" % _screen(w)
        row = _watch_row(w, "m") or ""
        assert row.startswith("M(") and "second)" in row, \
            "the multi-line watch should show its whole answer:\n%s" \
            % _screen(w)


# @scenario SC-debug-28
def test_a_debugger_without_watches_says_so(tmp_path):
    """An xwperc naming a debugger type the editor does not know: Ctrl-G W
    says on the status row that watches are not available, and no Watches
    window opens."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text("[Programming]\nDebugger : 99\n")
    with _c_session(tmp_path) as w:
        _add_watch(w, "x")
        assert _wait_for(w, lambda: "watches are not available with"
                         in w.display(drain=0)[-1], STEP_TIMEOUT_S), \
            _screen(w)
        assert "Watches" not in w.display(drain=0)[1], _screen(w)


F4 = "\033OS"         # Search; Go to cursor while a debug session runs
ALT_2 = "\0332"       # window 2
ALT_C = "\033c"       # in Options > Editor: the CUA-Style entry
# What the source window's bottom row names while a session runs.
DEBUG_BAR_LABELS = ("F7 Trace", "F8 Step", "F4 Go to cursor",
                    "^F7 Make Watch", "^F2 Quit", "Alt-X Quit")


def _bottom_bar(w):
    """The screen's bottom row: the active window's key hints, or the line a
    status message put there."""
    return w.display(drain=0)[-1]


def _status_row_says(w, text, timeout):
    """Wait until the bottom row carries @text.  A line the editor says while
    it goes on working stays there until the next key, so the ordinary wait
    sees it."""
    return _wait_for(w, lambda: text in _bottom_bar(w), timeout)


def _sgr(button, col, row, press=True):
    """An SGR 1006 mouse report at 1-based screen (col, row)."""
    return "\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")


def _click_bottom_bar(w, label):
    """Click the bottom-row entry whose label is @label."""
    bar = _bottom_bar(w)
    assert label in bar, "the bottom row should name %r:\n%s" % (label, _screen(w))
    col = bar.index(label) + 1
    row = w.screen.lines
    w.key(_sgr(0, col, row, True), _sgr(0, col, row, False), delay=0.6)


def _switch_to_cua(w):
    """Options > Editor > CUA-Style, Ok."""
    w.key(ALT.OPTIONS, delay=0.7)
    w.key("e", delay=0.9)
    w.key(ALT_C, delay=0.5)
    w.key("\r", delay=1.0)


def _select_sdb(w):
    """Debug > Options: pick Sdb, a debugger that is not installed here."""
    w.key(CTRL_G, "o", delay=0.6)
    w.key(DOWN, delay=0.3)          # focus moves to Sdb
    w.key(" ", delay=0.3)           # select it
    w.key("\033o", delay=0.5)       # Ok


def _source_window_on_top(w):
    """Make the source window the active one, so the bottom row is its bar."""
    w.key(ALT_1, delay=0.4)


# @scenario SC-debug-02
def test_the_source_bar_names_the_debugger_keys_while_a_session_runs(tmp_path):
    """While a session runs the source window's bottom row names the keys that
    drive it, as Turbo C's did; when the session ends the editing keys come
    back."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _source_window_on_top(w)
        assert "F4 Search" in _bottom_bar(w), \
            "with no session the bar should name the editing keys:\n%s" \
            % _screen(w)
        _start_c_session(w)
        _source_window_on_top(w)
        bar = _bottom_bar(w)
        for label in DEBUG_BAR_LABELS:
            assert label in bar, \
                "stopped in the debugger, the bottom row should name %r; " \
                "it reads:\n%r\n%s" % (label, bar, _screen(w))
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Debug > Quit should end gdb"
        _source_window_on_top(w)
        assert "F4 Search" in _bottom_bar(w), \
            "the editing keys should come back when the session ends:\n%s" \
            % _screen(w)


# @scenario SC-debug-02
def test_the_cua_bar_is_left_alone_by_a_session(tmp_path):
    """The CUA style keeps its editing bar while a session runs: its debugger
    keys are not decided yet, so the bar must not name keys that do nothing."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _switch_to_cua(w)
        assert "Alt-F2" in _bottom_bar(w), \
            "the CUA bar did not take effect:\n%s" % _screen(w)
        _start_c_session(w)
        _source_window_on_top(w)
        bar = _bottom_bar(w)
        assert "Alt-F2" in bar, \
            "the CUA style should keep its editing bar in a session:\n%s" \
            % _screen(w)
        assert "F7 Trace" not in bar, \
            "the CUA bar named a debugger key that is not decided:\n%s" \
            % _screen(w)


# @scenario SC-debug-06
def test_f4_runs_to_the_cursor_while_a_session_runs(tmp_path):
    """F4 while stopped runs the program to the line under the cursor, the
    same command as Debug > Goto cursor, and the bar names it."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        _source_window_on_top(w)
        assert "F4 Go to cursor" in _bottom_bar(w), \
            "the bar should name F4 as Go to cursor:\n%s" % _screen(w)
        _move_down(w, C_AFTER_PRINT_LINE - C_BREAK_LINE)
        w.key(F4, delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == C_AFTER_PRINT_LINE,
                         STEP_TIMEOUT_S), \
            "F4 should run to line %d:\n%s" % (C_AFTER_PRINT_LINE, _screen(w))


# @scenario SC-debug-06
def test_f4_stays_search_with_no_session(tmp_path):
    """With no debug session F4 still opens Find, as the editor's bar says."""
    _need("gcc")
    with _c_session(tmp_path) as w:
        w.key(F4, delay=0.6)
        assert "Find" in _screen(w), \
            "F4 with no session should open Find:\n%s" % _screen(w)


# @scenario SC-debug-27
def test_a_click_on_the_run_entry_of_the_watches_bar_runs(tmp_path):
    """Clicking "^F10 Run" on the Watches window's bar runs the program: the
    entry sends the key its label names, not the plain F10 that opens the
    menu."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        _add_watch(w, "x")
        assert _wait_for(w, lambda: "Watches" in _screen(w), STEP_TIMEOUT_S), \
            "the Watches window should be open:\n%s" % _screen(w)
        _click_bottom_bar(w, "^F10 Run")
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "the click should run the program to its end and end gdb:\n%s" \
            % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) is None, STEP_TIMEOUT_S), \
            "the current-line mark should go once the program ran:\n%s" \
            % _screen(w)


# @scenario SC-debug-27
def test_a_click_on_the_cua_make_watch_entry_makes_a_watch(tmp_path):
    """In the CUA style, clicking "^F5 Make Watch" opens the Add Watch dialog:
    the entry sends ^F5, the key its label names."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _switch_to_cua(w)
        _start_c_session(w)
        _add_watch(w, "x")
        assert _wait_for(w, lambda: "Watches" in _screen(w), STEP_TIMEOUT_S), \
            "the Watches window should be open:\n%s" % _screen(w)
        _click_bottom_bar(w, "^F5 Make Watch")
        assert _wait_for(w, lambda: "Add Watch" in _screen(w),
                         STEP_TIMEOUT_S), \
            "the click should open the Add Watch dialog:\n%s" % _screen(w)


# @scenario SC-debug-31
def test_a_backend_switch_is_a_status_line_not_a_box(tmp_path):
    """Ctrl-G R on a Python file says on the status row which debugger starts;
    no modal has to be dismissed and the session goes on by itself."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _move_down(w, PY_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _status_row_says(w, "switching to pdb", STEP_TIMEOUT_S), \
            "the status row should name the debugger that starts:\n%s" \
            % _screen(w)
        assert _wait_for(w, lambda: _children(w, "python3"), START_TIMEOUT_S), \
            "pdb should start with no box to dismiss:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         START_TIMEOUT_S), \
            "the session should go on to the breakpoint:\n%s" % _screen(w)
        w._drain(1.0)
        assert "switching to pdb" in _bottom_bar(w), \
            "the start's repaints should not erase the line before a key " \
            "is pressed:\n%s" % _screen(w)
        w.key(DOWN, delay=0.5)
        assert _wait_for(w, lambda: "F7 Trace" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "the next key should bring the debugger's keys back:\n%s" \
            % _screen(w)


# @scenario SC-debug-32
def test_a_debugger_that_never_started_prints_no_end_line(tmp_path):
    """A debugger that is not on PATH: only the reason is printed, never
    "End of Debugging", because no session ever ran."""
    _need("gcc")
    if shutil.which("sdb") is not None:
        pytest.skip("sdb is installed here")
    with _c_session(tmp_path) as w:
        _select_sdb(w)
        _move_down(w, C_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: "'sdb' not in Path" in _screen(w),
                         START_TIMEOUT_S), \
            "the missing debugger should be named:\n%s" % _screen(w)
        w.key("\r", delay=0.5)
        w._drain(1.0)
        assert "End of Debugging" not in _screen(w), \
            "nothing started, so no session ended:\n%s" % _screen(w)


# @scenario SC-debug-32
def test_a_cancelled_start_prints_no_end_line(tmp_path):
    """Esc while the debugger is starting: only the cancel line is printed,
    never "End of Debugging", because the start never became a session."""
    _need("gcc")
    env = _silent_gdb_env(tmp_path, LONG_START_TIMEOUT_S)
    with _c_session(tmp_path, env_extra=env) as w:
        _start_silent_session(w)
        w.key(ALT_1, delay=0.4)         # Messages is on top: back to p.c
        w.key(ESC, delay=0.5)
        assert _wait_for(w, lambda: "gdb start cancelled" in _screen(w),
                         STEP_TIMEOUT_S), \
            "Esc should end the start and say so:\n%s" % _screen(w)
        w._drain(1.0)
        assert "End of Debugging" not in _screen(w), \
            "a cancelled start ran no session, so none ended:\n%s" % _screen(w)


# @scenario SC-debug-32
def test_a_start_that_timed_out_prints_no_end_line(tmp_path):
    """A start that passed its deadline: only the deadline line is printed,
    never "End of Debugging", because the start never became a session."""
    _need("gcc")
    env = _silent_gdb_env(tmp_path, SHORT_START_TIMEOUT_S)
    with _c_session(tmp_path, env_extra=env) as w:
        _start_silent_session(w)
        expected = "gdb did not answer within %d s" % SHORT_START_TIMEOUT_S
        assert _wait_for(w, lambda: expected in _screen(w), STEP_TIMEOUT_S), \
            "the start should end at the deadline:\n%s" % _screen(w)
        w._drain(1.0)
        assert "End of Debugging" not in _screen(w), \
            "a start that timed out ran no session, so none ended:\n%s" \
            % _screen(w)


# @scenario SC-debug-13
def test_jdb_run_to_the_end_says_the_program_exited(tmp_path):
    """Under jdb, Ctrl-G R that runs the program to its end says so, as every
    other backend does."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5) as w:
        _start_java_session(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: "Program exited" in _screen(w),
                         JDB_START_TIMEOUT_S), \
            "Messages should say the program exited:\n%s" % _screen(w)
        assert _wait_for(w, lambda: not _children(w, "jdb"),
                         JDB_START_TIMEOUT_S), \
            "jdb should be gone after the program ended"


PRJ_TEXT = ("#\n"
            "# xwpe - project-file: p.prj\n"
            "CMP=\tgcc\n"
            "EXENAME=\tp\n"
            "FILES=\tp.c\n")


def _open_the_project(w):
    """The editor started on p.prj shows the project's file list; Enter opens
    its only file, p.c.  A second Enter would land in p.c as a new line and
    move every line below it."""
    assert _wait_for(w, lambda: "Project: p.prj" in _screen(w), STEP_TIMEOUT_S), \
        "the editor should open p.prj as a project:\n%s" % _screen(w)
    w.key("\r", delay=0.8)          # p.c


# @scenario SC-debug-09
def test_breakpoints_survive_an_editor_restart_through_the_project(tmp_path):
    """A breakpoint set with a project open is written to the project file and
    is back in the margin when the editor is started on that project again."""
    _need("gcc")
    (tmp_path / "p.c").write_text(C_PROG)
    (tmp_path / "p.prj").write_text(PRJ_TEXT)
    with WpeSession(str(tmp_path), PRJ_TEXT, filename="p.prj", wait=1.5) as w:
        _open_the_project(w)
        assert _wait_for(w, lambda: "p.c" in _screen(w), STEP_TIMEOUT_S), \
            "the project should open p.c:\n%s" % _screen(w)
        w.key(ALT_2, delay=0.5)      # the project window is 1, p.c is 2
        _move_down(w, C_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.5)
        assert C_BREAK_LINE in _breakpoint_lines(w), \
            "the breakpoint should be in the margin:\n%s" % _screen(w)
        w.key("\033x", delay=1.0)    # Alt-X: quit, writing the project
        assert _wait_for(w, lambda: not w.alive(), STEP_TIMEOUT_S), \
            "Alt-X should end the editor:\n%s" % _screen(w)
    saved = (tmp_path / "p.prj").read_text()
    assert "BREAKPOINTS" in saved, \
        "the project file should hold the breakpoint:\n%s" % saved
    # The session writes its seed to p.prj, so seed it with what was saved.
    with WpeSession(str(tmp_path), saved, filename="p.prj", wait=1.5) as w:
        _open_the_project(w)
        assert _wait_for(w, lambda: C_BREAK_LINE in _breakpoint_lines(w),
                         STEP_TIMEOUT_S), \
            "the breakpoint should be back in the margin:\n%s" % _screen(w)


# Go to cursor on every debugger.  Each program calls one function twice; the
# user's breakpoint is on main's first statement and the cursor goes to a line
# of the function's body, in another frame than the stop.  F4 must stop there,
# and a later Ctrl-G R must run to the end without stopping there again.
GOTO_TIMEOUT_S = 25         # a JVM, a build or an adapter, before scaling
SCALA_START_TIMEOUT_S = 150 # a build server, cold, before scaling

GOTO_PY = (
    "def sq(v):\n"
    "    r = v * v\n"
    "    return r\n"
    "\n"
    "x = 3\n"
    "y = sq(x)\n"
    "z = sq(y)\n"
    "print('END', z)\n"
)
GOTO_JAVA = (
    "public class P {\n"
    "    static int sq(int v) {\n"
    "        int r = v * v;\n"
    "        return r;\n"
    "    }\n"
    "    public static void main(String[] a) {\n"
    "        int x = 3;\n"
    "        int y = sq(x);\n"
    "        int z = sq(y);\n"
    "        System.out.println(\"END \" + z);\n"
    "    }\n"
    "}\n"
)
JAVA_RESTART_LINE = 8       # int y = sq(x); -- "stop in P.main" stops on 7
JAVA_SQ_LINE = 3            # int r = v * v;
GOTO_A68 = (
    "PROC sq = (INT v) INT:\n"
    "BEGIN\n"
    "   INT r = v * v;\n"
    "   r\n"
    "END;\n"
    "INT x := 3;\n"
    "INT y := sq (x);\n"
    "INT z := sq (y);\n"
    "print ((z, new line))\n"
)
GOTO_GO = (
    "package main\n"
    "\n"
    "import \"fmt\"\n"
    "\n"
    "func sq(v int) int {\n"
    "\tr := v * v\n"
    "\treturn r\n"
    "}\n"
    "\n"
    "func main() {\n"
    "\tx := 3\n"
    "\ty := sq(x)\n"
    "\tz := sq(y)\n"
    "\tfmt.Println(\"END\", z)\n"
    "}\n"
)
GO_SQ_LINE = 6              # r := v * v
GOTO_RUST = (
    "fn sq(v: i32) -> i32 {\n"
    "    let r = v * v;\n"
    "    r\n"
    "}\n"
    "fn main() {\n"
    "    let x = 3;\n"
    "    let y = sq(x);\n"
    "    let z = sq(y);\n"
    "    println!(\"END {}\", z);\n"
    "}\n"
)
GOTO_SCALA = (
    "object P:\n"
    "  def sq(v: Int): Int =\n"
    "    val r = v * v\n"
    "    r\n"
    "  def main(args: Array[String]): Unit =\n"
    "    val x = 3\n"
    "    val y = sq(x)\n"
    "    val z = sq(y)\n"
    "    println(s\"END $z\")\n"
)


def _go_to_line(w, line):
    """Search > Go to Line: put the cursor on @line of the window on top."""
    w.key(ALT.SEARCH, delay=0.5)
    w.key("g", delay=0.6)
    w.key(str(line))
    w.key("\r", delay=0.6)


def _go_to_cursor_and_run_on(w, break_line, cursor_line, ended, start_timeout):
    """Stop on the user's breakpoint, F4 to @cursor_line, then Ctrl-G R to the
    end: the mark lands on the cursor's line once and never again."""
    _go_to_line(w, break_line)
    w.key(CTRL_G, "b", delay=0.5)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == break_line, start_timeout), \
        "Ctrl-G R should stop on line %d:\n%s" % (break_line, _screen(w))
    _go_to_line(w, cursor_line)
    assert _wait_for(w, lambda: "F4 Go to cursor" in _bottom_bar(w),
                     STEP_TIMEOUT_S), \
        "the bar should offer F4 Go to cursor:\n%s" % _screen(w)
    w.key(F4, delay=0.5)
    assert _wait_for(w, lambda: _mark_line(w) == cursor_line, GOTO_TIMEOUT_S), \
        "F4 should stop on line %d:\n%s" % (cursor_line, _screen(w))
    assert break_line in _breakpoint_lines(w), \
        "the user's breakpoint should still be there:\n%s" % _screen(w)
    assert cursor_line not in _breakpoint_lines(w), \
        "Go to cursor's breakpoint should be gone at the stop:\n%s" % _screen(w)
    w.key(CTRL_G, "r", delay=0.5)
    assert _wait_for(w, ended, GOTO_TIMEOUT_S), \
        "Ctrl-G R should run to the end without stopping on line %d again " \
        "(mark on %s):\n%s" % (cursor_line, _mark_line(w), _screen(w))


def _pdb_ended(w):
    """pdb's end of program: its box says so before pdb is let go."""
    return "End of code" in _screen(w) or not _children(w, "python3")


# @scenario SC-debug-06
def test_f4_runs_to_the_cursor_under_pdb(tmp_path):
    _need("python3")
    with WpeSession(str(tmp_path), GOTO_PY, filename="p.py", wait=1.5) as w:
        _go_to_cursor_and_run_on(w, 5, 2, lambda: _pdb_ended(w),
                                 START_TIMEOUT_S)


# @scenario SC-debug-06
def test_f4_keeps_the_users_breakpoint_on_the_cursor_line_under_pdb(tmp_path):
    """The user's own breakpoint on the cursor's line survives Go to cursor:
    the second call stops there again."""
    _need("python3")
    with WpeSession(str(tmp_path), GOTO_PY, filename="p.py", wait=1.5) as w:
        _go_to_line(w, 2)
        w.key(CTRL_G, "b", delay=0.5)
        _go_to_line(w, 5)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == 5, START_TIMEOUT_S), \
            _screen(w)
        _go_to_line(w, 2)
        w.key(F4, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == 2, STEP_TIMEOUT_S), \
            _screen(w)
        w.key(F8, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == 3, STEP_TIMEOUT_S), \
            _screen(w)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == 2, STEP_TIMEOUT_S), \
            "the user's breakpoint on line 2 should stop the second call:\n%s" \
            % _screen(w)


# @scenario SC-debug-06
def test_f4_runs_to_the_cursor_under_jdb(tmp_path):
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), GOTO_JAVA, filename="P.java", wait=1.5) as w:
        _go_to_cursor_and_run_on(w, 7, 3, lambda: not _children(w, "jdb"),
                                 JDB_START_TIMEOUT_S)


# @scenario SC-debug-06
def test_f4_runs_to_the_cursor_under_a68g(tmp_path):
    _need("a68g")
    with WpeSession(str(tmp_path), GOTO_A68, filename="p.a68", wait=1.5) as w:
        _go_to_cursor_and_run_on(w, 6, 3, lambda: not _children(w, "a68g"),
                                 START_TIMEOUT_S)


TWO_FILE_MAIN = (
    "import m\n"
    "x = 3\n"
    "y = m.f(x)\n"
    "print('END', y)\n"
)
TWO_FILE_MODULE = (
    "def f(v):\n"
    "    r = v + 1\n"
    "    s = r * 2\n"
    "    return s\n"
)


def _window_mark_line(w, name):
    """The marked source line of the window titled @name, wherever it sits
    on screen (a window opened later is cascaded below the first)."""
    disp = w.display(drain=0)
    top = next((y for y, row in enumerate(disp) if " %s " % name in row), None)
    if top is None:
        return None
    for y in range(top + 1, len(disp)):
        row = w.screen.buffer[y]
        if disp[y].lstrip("x").startswith("m"):
            return None
        if any(row[x].bg == CURRENT_LINE_BG
               for x in range(2, w.screen.columns - 1)):
            return y - top
    return None


# @scenario SC-debug-02
def test_a_file_opened_during_a_session_gets_the_debugger_bar(tmp_path):
    """F7 into a function of another file opens that file with the
    debugger's keys on its bottom row, and F4 there runs to the cursor; when
    the session ends both files show their editing keys again."""
    _need("python3")
    (tmp_path / "m.py").write_text(TWO_FILE_MODULE)
    with WpeSession(str(tmp_path), TWO_FILE_MAIN, filename="p.py",
                    wait=1.5) as w:
        _go_to_line(w, 2)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == 2, START_TIMEOUT_S), \
            _screen(w)
        assert _step(w, F8, 3), _screen(w)
        w.key(F7, delay=0.5)
        assert _wait_for(w, lambda: " m.py " in _screen(w), STEP_TIMEOUT_S), \
            "F7 should open m.py:\n%s" % _screen(w)
        assert _wait_for(w, lambda: "F4 Go to cursor" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "m.py should show the debugger's keys:\n%s" % _screen(w)
        _go_to_line(w, 3)
        w.key(F4, delay=0.5)
        assert _wait_for(w, lambda: _window_mark_line(w, "m.py") == 3,
                         STEP_TIMEOUT_S), \
            "F4 in m.py should run to line 3:\n%s" % _screen(w)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "python3"),
                         STEP_TIMEOUT_S), _screen(w)
        assert _wait_for(w, lambda: "F4 Search" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "m.py should show the editing keys after the session:\n%s" \
            % _screen(w)
        _source_window_on_top(w)
        assert _wait_for(w, lambda: "F4 Search" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "p.py should show the editing keys after the session:\n%s" \
            % _screen(w)


# @scenario SC-debug-07
def test_a_breakpoint_toggled_off_under_a68g_no_longer_stops(tmp_path):
    """Under a68g a breakpoint removed with Ctrl-G B while the session runs
    stops the program no more: Ctrl-G R goes on to the breakpoint that is
    left."""
    _need("a68g")
    with WpeSession(str(tmp_path), GOTO_A68, filename="p.a68", wait=1.5) as w:
        for line in (6, 3, 8):
            _go_to_line(w, line)
            w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == 6, START_TIMEOUT_S), \
            _screen(w)
        _go_to_line(w, 3)
        w.key(CTRL_G, "b", delay=0.5)           # toggle line 3 off
        assert 3 not in _breakpoint_lines(w), _screen(w)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) in (3, 8), STEP_TIMEOUT_S), \
            _screen(w)
        assert _mark_line(w) == 8, \
            "the program should stop only on line 8, not on the removed " \
            "breakpoint on line 3:\n%s" % _screen(w)


# @scenario SC-debug-06
def test_f4_runs_to_the_cursor_under_dlv(tmp_path):
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GOTO_GO, filename="main.go", wait=2.0) as w:
        _go_to_cursor_and_run_on(w, 11, 6, lambda: not _children(w, "dlv"),
                                 GOTO_TIMEOUT_S)


# @scenario SC-debug-06
@pytest.mark.parametrize("adapter,binary", [("gdb", "gdb"), ("lldb", "lldb-dap")])
def test_f4_runs_to_the_cursor_under_a_rust_adapter(tmp_path, adapter, binary):
    _need("rustc", binary)
    with WpeSession(str(tmp_path), GOTO_RUST, filename="main.rs", wait=2.0,
                    env_extra={"XWPE_DAP_ADAPTER": adapter}) as w:
        _go_to_cursor_and_run_on(w, 6, 2, lambda: not _children(w, binary),
                                 RUST_START_TIMEOUT_S)


# @scenario SC-debug-06
def test_f4_runs_to_the_cursor_under_scala(tmp_path):
    _need("scala-cli")
    with WpeSession(str(tmp_path), GOTO_SCALA, filename="p.scala",
                    wait=2.0) as w:
        _go_to_cursor_and_run_on(w, 6, 3,
                                 lambda: "Program exited" in _screen(w),
                                 SCALA_START_TIMEOUT_S)


# @scenario SC-debug-02
def test_a_debug_adapter_session_shows_the_debugger_bar(tmp_path):
    """A Go session started through dlv names the debugger's keys on the
    source window's bottom row once it stops, as every other session does."""
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GOTO_GO, filename="main.go", wait=2.0) as w:
        _go_to_line(w, 11)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == 11, GOTO_TIMEOUT_S), \
            _screen(w)
        _source_window_on_top(w)
        assert _wait_for(w, lambda: "F7 Trace" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "the source window should show the debugger's keys:\n%s" \
            % _screen(w)


# @scenario SC-debug-06
def test_f4_in_messages_does_not_run_the_program(tmp_path):
    """F4 with Messages on top during a session is not Go to cursor: the
    program stays stopped where it was."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(ALT_2, delay=0.5)                 # Messages on top
        assert "F4 Go to cursor" not in _bottom_bar(w), _screen(w)
        w.key(F4, delay=1.0)
        w.key("\033", delay=0.5)                # close Find if it opened
        w._drain(1.0)
        assert _children(w, "gdb"), \
            "the program should not have run to its end:\n%s" % _screen(w)
        _source_window_on_top(w)
        assert _wait_for(w, lambda: _mark_line(w) == C_BREAK_LINE,
                         STEP_TIMEOUT_S), \
            "the stop should still be on line %d:\n%s" \
            % (C_BREAK_LINE, _screen(w))


# Breakpoints changed while a session runs, on every debugger.  The same
# programs as Go to cursor.  The line a restarted session must stop on is
# never main's first statement: a debugger the user gave no breakpoint gets
# one of the editor's own there, so a test waiting on that line would also
# pass when the user's breakpoints were lost across the restart.  Each pair
# below is a line main reaches only after its first statement, and a line
# inside the function main calls there.
GOTO_C = (
    "#include <stdio.h>\n"
    "int sq(int v)\n"
    "{\n"
    "  int r = v * v;\n"
    "  return r;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  int y = sq(x);\n"
    "  int z = sq(y);\n"
    "  printf(\"END %d\\n\", z);\n"
    "  return 0;\n"
    "}\n"
)
C_RESTART_LINE = 10         # int y = sq(x); -- "b main" stops on 9, never here
C_SQ_LINE = 4               # int r = v * v;
HEALTH_LINE = 1             # where the cursor goes to prove the editor answers


def _takes_keys(w):
    """The editor still answers: Go to Line moves the cursor, and the
    line:column field follows."""
    _go_to_line(w, HEALTH_LINE)
    return _wait_for(w, lambda: _status_field_line(w) == HEALTH_LINE,
                     STEP_TIMEOUT_S)


END_BOXES = ("End of code", "Debugger stopped")


def _dismiss_the_end_box(w):
    """Some debuggers' end of program is said in a box: Enter closes it, so
    the next Ctrl-G R reaches the editor."""
    w._drain(1.0)
    if any(text in _screen(w) for text in END_BOXES):
        w.key("\r", delay=0.5)


def _runs_to_the_end_past(w, line, ended, timeout):
    """Wait for @ended; False as soon as the mark lands on @line instead."""
    deadline = time.time() + timeout * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if _mark_line(w) == line:
            return False
        if ended():
            return True
    return False


def _toggle_and_remove_in_a_session(w, first, second, ended, start_timeout):
    """Stopped on @first: (a) Ctrl-G B on @second arms it and Ctrl-G R stops
    there; (b) after a restart, Ctrl-G B on @second disarms it and Ctrl-G R
    runs to the end; (c) after another start, @second set again and Ctrl-G M
    removes every breakpoint, so Ctrl-G R runs to the end.  @ended is a
    factory: called before a run, it returns the test for that run's end."""
    _go_to_line(w, first)
    w.key(CTRL_G, "b", delay=0.5)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == first, start_timeout), \
        "Ctrl-G R should stop on line %d:\n%s" % (first, _screen(w))
    # (a) a breakpoint set while the session runs stops the program
    _go_to_line(w, second)
    w.key(CTRL_G, "b", delay=0.5)
    assert second in _breakpoint_lines(w), _screen(w)
    w.key(CTRL_G, "r", delay=0.5)
    assert _wait_for(w, lambda: _mark_line(w) == second, GOTO_TIMEOUT_S), \
        "the breakpoint set during the session should stop the program on " \
        "line %d:\n%s" % (second, _screen(w))
    # (b) the same breakpoint toggled off stops it no more
    done = ended()
    w.key(CTRL_G, "q", delay=0.5)
    assert _wait_for(w, done, GOTO_TIMEOUT_S), \
        "Ctrl-G Q should end the session:\n%s" % _screen(w)
    _dismiss_the_end_box(w)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == first, start_timeout), \
        "the restarted session should stop on line %d:\n%s" \
        % (first, _screen(w))
    _go_to_line(w, second)
    w.key(CTRL_G, "b", delay=0.5)
    assert second not in _breakpoint_lines(w), _screen(w)
    assert _takes_keys(w), \
        "the editor should take keys after the breakpoint went:\n%s" \
        % _screen(w)
    done = ended()
    w.key(CTRL_G, "r", delay=0.5)
    assert _runs_to_the_end_past(w, second, done, GOTO_TIMEOUT_S), \
        "Ctrl-G R should run to the end, not stop on the removed breakpoint " \
        "on line %d (mark on %s):\n%s" % (second, _mark_line(w), _screen(w))
    # (c) Remove all breakpoints, the one set during the session included.
    # A session some adapters keep open after the end is ended first, so the
    # mark the next stop puts is not one left from this run.
    _dismiss_the_end_box(w)
    w.key(CTRL_G, "q", delay=0.5)
    assert _wait_for(w, lambda: _mark_line(w) is None, GOTO_TIMEOUT_S), \
        "no line should be marked once the session ended:\n%s" % _screen(w)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == first, start_timeout), \
        "the third session should stop on line %d:\n%s" % (first, _screen(w))
    _go_to_line(w, second)
    w.key(CTRL_G, "b", delay=0.5)
    assert second in _breakpoint_lines(w), _screen(w)
    w.key(CTRL_G, "m", delay=0.5)
    assert _breakpoint_lines(w) == [], \
        "Ctrl-G M should clear the margin:\n%s" % _screen(w)
    assert _takes_keys(w), \
        "the editor should take keys after Remove all:\n%s" % _screen(w)
    done = ended()
    w.key(CTRL_G, "r", delay=0.5)
    assert _runs_to_the_end_past(w, second, done, GOTO_TIMEOUT_S), \
        "after Remove all, Ctrl-G R should run to the end (mark on %s):\n%s" \
        % (_mark_line(w), _screen(w))


def _process_ended(w, name):
    """The end of a session whose debugger is the editor's child @name."""
    return lambda: (lambda: not _children(w, name))


def _last_message(w):
    """The last line of text in the Messages window, or "" when it has none."""
    lines = [row[1:-1].strip() for row, _ in _messages_rows(w)]
    lines = [line for line in lines if line]
    return lines[-1] if lines else ""


def _says_last(w, *texts):
    """The end of a run told by Messages: its last line becomes one of
    @texts, where it was another line before the run."""
    def says_end():
        return any(text in _last_message(w) for text in texts)

    def factory():
        before = _last_message(w)
        return lambda: says_end() and _last_message(w) != before
    return factory


# @scenario SC-debug-07
def test_breakpoints_changed_in_a_session_under_gdb(tmp_path):
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GOTO_C, filename="p.c", wait=1.5) as w:
        _toggle_and_remove_in_a_session(w, C_RESTART_LINE, C_SQ_LINE,
                                        _process_ended(w, "gdb"),
                                        START_TIMEOUT_S)


# @scenario SC-debug-07
def test_breakpoints_changed_in_a_session_under_pdb(tmp_path):
    _need("python3")
    with WpeSession(str(tmp_path), GOTO_PY, filename="p.py", wait=1.5) as w:
        _toggle_and_remove_in_a_session(w, 5, 2,
                                        lambda: (lambda: _pdb_ended(w)),
                                        START_TIMEOUT_S)


# @scenario SC-debug-07
def test_breakpoints_changed_in_a_session_under_jdb(tmp_path):
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), GOTO_JAVA, filename="P.java", wait=1.5) as w:
        _toggle_and_remove_in_a_session(w, JAVA_RESTART_LINE, JAVA_SQ_LINE,
                                        _process_ended(w, "jdb"),
                                        JDB_START_TIMEOUT_S)


# @scenario SC-debug-07
def test_breakpoints_changed_in_a_session_under_a68g(tmp_path):
    _need("a68g")
    with WpeSession(str(tmp_path), GOTO_A68, filename="p.a68", wait=1.5) as w:
        _toggle_and_remove_in_a_session(w, 6, 3, _process_ended(w, "a68g"),
                                        START_TIMEOUT_S)


# @scenario SC-debug-07
def test_breakpoints_changed_in_a_session_under_dlv(tmp_path):
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GOTO_GO, filename="main.go", wait=2.0) as w:
        _toggle_and_remove_in_a_session(w, 11, 6, _process_ended(w, "dlv"),
                                        GOTO_TIMEOUT_S)


# @scenario SC-debug-07
def test_remove_all_under_an_adapter_after_a_start_with_no_breakpoint(tmp_path):
    """Remove all in an adapter session that stopped at the program's start
    because the user had marked nothing: the one line he then marks goes, the
    margin clears, the program runs to its end and the editor still takes
    keys.  The session's first stop is the adapter's own, not a line of the
    user's, which is the case the other adapter tests do not reach."""
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GOTO_GO, filename="main.go", wait=2.0) as w:
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) is not None, GOTO_TIMEOUT_S), \
            "the adapter should stop at the program's start:\n%s" % _screen(w)
        _go_to_line(w, GO_SQ_LINE)
        w.key(CTRL_G, "b", delay=0.5)
        assert GO_SQ_LINE in _breakpoint_lines(w), _screen(w)
        w.key(CTRL_G, "m", delay=0.5)
        assert _breakpoint_lines(w) == [], \
            "Ctrl-G M should clear the margin:\n%s" % _screen(w)
        assert _takes_keys(w), \
            "the editor should take keys after Remove all:\n%s" % _screen(w)
        done = _process_ended(w, "dlv")()
        w.key(CTRL_G, "r", delay=0.5)
        assert _runs_to_the_end_past(w, GO_SQ_LINE, done, GOTO_TIMEOUT_S), \
            "after Remove all, Ctrl-G R should run to the end, not stop on " \
            "line %d (mark on %s):\n%s" \
            % (GO_SQ_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-07
@pytest.mark.parametrize("adapter,binary", [("gdb", "gdb"), ("lldb", "lldb-dap")])
def test_breakpoints_changed_in_a_session_under_a_rust_adapter(
        tmp_path, adapter, binary):
    _need("rustc", binary)
    with WpeSession(str(tmp_path), GOTO_RUST, filename="main.rs", wait=2.0,
                    env_extra={"XWPE_DAP_ADAPTER": adapter}) as w:
        _toggle_and_remove_in_a_session(w, 6, 2, _process_ended(w, binary),
                                        RUST_START_TIMEOUT_S)


# @scenario SC-debug-07
def test_breakpoints_changed_in_a_session_under_scala(tmp_path):
    _need("scala-cli")
    with WpeSession(str(tmp_path), GOTO_SCALA, filename="p.scala",
                    wait=2.0) as w:
        _toggle_and_remove_in_a_session(w, 6, 3,
                                        _says_last(w, "End of Debugging",
                                                   "Program exited"),
                                        SCALA_START_TIMEOUT_S)


# Show stack, Finish function and the program's end on every debugger, with
# the programs of Go to cursor: the program stops in the body of sq, which
# main calls on CALL_LINE.  gdb is the reference: its Stack window lists
# "#0  sq (v=3) at p.c:4" over "#1  main () at p.c:10", Enter on a row marks
# that frame's line, Finish function marks the caller's line, and a program
# that ends says so in Messages.  The others must do the same.
FINISH_OUTERMOST_LINE = "Finish function: not meaningful in the outermost frame."
OUTPUT_HINT = " Program output: Ctrl-G P"
MODAL_MARKS = (" OK ", "End of code", "Debugger stopped")
FRAME_ROW = re.compile(r"#\d+  \S+(?: \S+)*")


def _stack_frames(w):
    """The Stack window's frame rows, top to bottom ("#0  sq at p.py:2")."""
    frames = []
    inside = False
    for line in w.display(drain=0):
        if " Stack " in line:
            inside = True
            continue
        if not inside:
            continue
        found = FRAME_ROW.search(line)
        if found:
            frames.append(found.group(0))
        elif frames:
            break
    return frames


def _messages_lines(w):
    """Every line of text in the Messages window."""
    return [row[1:-1].strip() for row, _ in _messages_rows(w)]


def _no_modal(w):
    """No box waits for a key: no OK button, no end-of-program box."""
    screen = _screen(w)
    return not any(mark in screen for mark in MODAL_MARKS)


def _stop_in_the_function(w, body_line, start_timeout):
    """Breakpoint on @body_line, Ctrl-G R, wait for the stop there."""
    _go_to_line(w, body_line)
    w.key(CTRL_G, "b", delay=0.5)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == body_line, start_timeout), \
        "Ctrl-G R should stop on line %d:\n%s" % (body_line, _screen(w))


def _shows_the_stack(w, body_line, inner, outer, call_line, start_timeout):
    """Stopped in sq: Ctrl-G K lists sq over its caller, each with its
    function and its file:line (@inner, @outer: texts each row holds), and
    Enter on the caller's row marks @call_line (None: the caller's row names
    no line to mark)."""
    _stop_in_the_function(w, body_line, start_timeout)
    w.key(CTRL_G, "k", delay=0.5)
    assert _wait_for(w, lambda: len(_stack_frames(w)) >= 2, STEP_TIMEOUT_S), \
        "Ctrl-G K should list at least two frames:\n%s" % _screen(w)
    frames = _stack_frames(w)
    assert frames[0].startswith("#0") and \
        all(text in frames[0] for text in inner), \
        "the innermost frame should come first, %s:\n%s" % (inner, _screen(w))
    assert frames[1].startswith("#1") and \
        all(text in frames[1] for text in outer), \
        "its caller should come next, %s:\n%s" % (outer, _screen(w))
    assert _no_modal(w), "Show stack should open no box:\n%s" % _screen(w)
    if call_line is None:
        return
    w.key(DOWN, delay=0.4)
    w.key("\r", delay=0.8)
    assert _wait_for(w, lambda: _mark_line(w) == call_line, STEP_TIMEOUT_S), \
        "Enter on the caller's row should mark line %d (mark on %s):\n%s" \
        % (call_line, _mark_line(w), _screen(w))


def _finishes_to_the_caller(w, body_line, caller_line, running, start_timeout):
    """Stopped in sq, the cursor moved away: Ctrl-G F marks @caller_line in
    the caller.  Ctrl-G F there, in the outermost frame, says so in one line
    and leaves the session where it was (@running: the session is alive)."""
    _stop_in_the_function(w, body_line, start_timeout)
    _go_to_line(w, HEALTH_LINE)
    w.key(CTRL_G, "f", delay=0.5)
    assert _wait_for(w, lambda: _mark_line(w) == caller_line, GOTO_TIMEOUT_S), \
        "Ctrl-G F should mark line %d of the caller (mark on %s):\n%s" \
        % (caller_line, _mark_line(w), _screen(w))
    assert _no_modal(w), "Finish function should open no box:\n%s" % _screen(w)
    w.key(CTRL_G, "f", delay=0.5)
    assert _wait_for(w, lambda: FINISH_OUTERMOST_LINE in _messages_lines(w),
                     STEP_TIMEOUT_S), \
        "Ctrl-G F in the outermost frame should say so in Messages:\n%s" \
        % _screen(w)
    assert _no_modal(w), \
        "the outermost frame should be said without a box:\n%s" % _screen(w)
    assert _mark_line(w) == caller_line and running(), \
        "the session should stay stopped on line %d (mark on %s):\n%s" \
        % (caller_line, _mark_line(w), _screen(w))


def _ends_with_one_line(w, body_line, end_line, ended, start_timeout):
    """Stopped in sq: Ctrl-G R runs to the end; Messages holds @end_line,
    no box waits for a key, the mark goes and the editor takes keys."""
    _stop_in_the_function(w, body_line, start_timeout)
    _go_to_line(w, body_line)
    w.key(CTRL_G, "b", delay=0.5)
    w.key(CTRL_G, "r", delay=0.5)
    assert _wait_for(w, lambda: end_line in _messages_lines(w),
                     GOTO_TIMEOUT_S), \
        "Messages should say %r:\n%s" % (end_line, _screen(w))
    assert _no_modal(w), "the end should open no box:\n%s" % _screen(w)
    assert _wait_for(w, ended, GOTO_TIMEOUT_S), \
        "the session should end with the program:\n%s" % _screen(w)
    assert _wait_for(w, lambda: _mark_line(w) is None, STEP_TIMEOUT_S), \
        "no line should stay marked:\n%s" % _screen(w)
    assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)


STACK_COLS = 160   # the Stack window takes a third: room for a whole row


def _go_session(tmp_path, cols=80):
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    return WpeSession(str(tmp_path), GOTO_GO, filename="main.go", wait=2.0,
                      cols=cols)


def _rust_session(tmp_path, adapter, cols=80):
    return WpeSession(str(tmp_path), GOTO_RUST, filename="main.rs", wait=2.0,
                      cols=cols, env_extra={"XWPE_DAP_ADAPTER": adapter})


def _scala_session(tmp_path, cols=80):
    return WpeSession(str(tmp_path), GOTO_SCALA, filename="p.scala", wait=2.0,
                      cols=cols)


def _alive(w, name):
    return lambda: bool(_children(w, name))


# @scenario SC-debug-33
def test_show_stack_under_pdb(tmp_path):
    _need("python3")
    with WpeSession(str(tmp_path), GOTO_PY, filename="p.py", wait=1.5,
                    cols=STACK_COLS) as w:
        _shows_the_stack(w, 2, ("sq", "p.py:2"), ("<module>", "p.py:6"), 6,
                         START_TIMEOUT_S)


# @scenario SC-debug-33
def test_show_stack_under_jdb(tmp_path):
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), GOTO_JAVA, filename="P.java", wait=1.5,
                    cols=STACK_COLS) as w:
        _shows_the_stack(w, JAVA_SQ_LINE, ("P.sq", "P.java:3"),
                         ("P.main", "P.java:8"), JAVA_RESTART_LINE,
                         JDB_START_TIMEOUT_S)


# @scenario SC-debug-33
def test_show_stack_under_a68g(tmp_path):
    """a68g names no call sites: the main program's row has no line."""
    _need("a68g")
    with WpeSession(str(tmp_path), GOTO_A68, filename="p.a68", wait=1.5,
                    cols=STACK_COLS) as w:
        _shows_the_stack(w, 3, ("sq", "p.a68:3"), ("(main program) in p.a68",),
                         None, START_TIMEOUT_S)


# @scenario SC-debug-33
def test_show_stack_under_dlv(tmp_path):
    _need("dlv", "go")
    with _go_session(tmp_path, STACK_COLS) as w:
        _shows_the_stack(w, GO_SQ_LINE, ("main.sq", "main.go:6"),
                         ("main.main", "main.go:12"), 12, GOTO_TIMEOUT_S)


# @scenario SC-debug-33
@pytest.mark.parametrize("adapter,binary", [("gdb", "gdb"), ("lldb", "lldb-dap")])
def test_show_stack_under_a_rust_adapter(tmp_path, adapter, binary):
    _need("rustc", binary)
    with _rust_session(tmp_path, adapter, STACK_COLS) as w:
        _shows_the_stack(w, 2, ("sq", "main.rs:2"), ("main", "main.rs:7"), 7,
                         RUST_START_TIMEOUT_S)


# @scenario SC-debug-33
def test_show_stack_under_scala(tmp_path):
    _need("scala-cli")
    with _scala_session(tmp_path, STACK_COLS) as w:
        _shows_the_stack(w, 3, ("sq", "p.scala:3"), ("main", "p.scala:7"), 7,
                         SCALA_START_TIMEOUT_S)


# @scenario SC-debug-34
def test_finish_function_under_pdb(tmp_path):
    """pdb's "return" stops at the end of sq; the caller's line is marked."""
    _need("python3")
    with WpeSession(str(tmp_path), GOTO_PY, filename="p.py", wait=1.5) as w:
        _finishes_to_the_caller(w, 2, 6, _alive(w, "python3"),
                                START_TIMEOUT_S)


# @scenario SC-debug-34
def test_finish_function_under_jdb(tmp_path):
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), GOTO_JAVA, filename="P.java", wait=1.5) as w:
        _finishes_to_the_caller(w, JAVA_SQ_LINE, JAVA_RESTART_LINE,
                                _alive(w, "jdb"), JDB_START_TIMEOUT_S)


# @scenario SC-debug-34
def test_finish_function_under_a68g(tmp_path):
    """a68g stops at the caller's next unit, the line after the call."""
    _need("a68g")
    with WpeSession(str(tmp_path), GOTO_A68, filename="p.a68", wait=1.5) as w:
        _finishes_to_the_caller(w, 3, 8, _alive(w, "a68g"), START_TIMEOUT_S)


# @scenario SC-debug-34
def test_finish_function_under_dlv(tmp_path):
    _need("dlv", "go")
    with _go_session(tmp_path) as w:
        _finishes_to_the_caller(w, GO_SQ_LINE, 12, _alive(w, "dlv"),
                                GOTO_TIMEOUT_S)


# @scenario SC-debug-34
@pytest.mark.parametrize("adapter,binary", [("gdb", "gdb"), ("lldb", "lldb-dap")])
def test_finish_function_under_a_rust_adapter(tmp_path, adapter, binary):
    _need("rustc", binary)
    with _rust_session(tmp_path, adapter) as w:
        _finishes_to_the_caller(w, 2, 7, _alive(w, binary),
                                RUST_START_TIMEOUT_S)


# @scenario SC-debug-34
def test_finish_function_under_scala(tmp_path):
    _need("scala-cli")
    with _scala_session(tmp_path) as w:
        _finishes_to_the_caller(
            w, 3, 7, lambda: "End of Debugging" not in _messages_lines(w),
            SCALA_START_TIMEOUT_S)


# @scenario SC-debug-13
def test_pdb_end_is_one_line_with_the_exit_code(tmp_path):
    """A script that runs off its end exits with 0; its output is under
    Ctrl-G P, and the line says so."""
    _need("python3")
    with WpeSession(str(tmp_path), GOTO_PY, filename="p.py", wait=1.5) as w:
        _ends_with_one_line(w, 2, "Program exited with code 0." + OUTPUT_HINT,
                            lambda: not _children(w, "python3"),
                            START_TIMEOUT_S)


# @scenario SC-debug-14
def test_pdb_sys_exit_is_one_line_with_its_code(tmp_path):
    _need("python3")
    with WpeSession(str(tmp_path), PY_EXIT_PROG, filename="p.py",
                    wait=1.5) as w:
        _ends_with_one_line(w, PY_EXIT_BREAK_LINE,
                            "Program exited with code 3." + OUTPUT_HINT,
                            lambda: not _children(w, "python3"),
                            START_TIMEOUT_S)


# @scenario SC-debug-13
def test_jdb_end_is_one_line(tmp_path):
    """jdb names no exit code: the plain line, and where the output is."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), GOTO_JAVA, filename="P.java", wait=1.5) as w:
        _ends_with_one_line(w, JAVA_SQ_LINE, "Program exited." + OUTPUT_HINT,
                            lambda: not _children(w, "jdb"),
                            JDB_START_TIMEOUT_S)


# @scenario SC-debug-13
def test_a68g_end_is_one_line(tmp_path):
    """a68g names no exit code; its output is in Messages already."""
    _need("a68g")
    with WpeSession(str(tmp_path), GOTO_A68, filename="p.a68", wait=1.5) as w:
        _ends_with_one_line(w, 3, "Program exited.",
                            lambda: not _children(w, "a68g"),
                            START_TIMEOUT_S)


# @scenario SC-debug-14
def test_dlv_end_is_one_line_with_the_exit_code(tmp_path):
    _need("dlv", "go")
    with _go_session(tmp_path) as w:
        _ends_with_one_line(w, GO_SQ_LINE, "Program exited with code 0",
                            lambda: not _children(w, "dlv"), GOTO_TIMEOUT_S)


# @scenario SC-debug-14
@pytest.mark.parametrize("adapter,binary", [("gdb", "gdb"), ("lldb", "lldb-dap")])
def test_rust_adapter_end_is_one_line_with_the_exit_code(tmp_path, adapter,
                                                         binary):
    _need("rustc", binary)
    with _rust_session(tmp_path, adapter) as w:
        _ends_with_one_line(w, 2, "Program exited with code 0",
                            lambda: not _children(w, binary),
                            RUST_START_TIMEOUT_S)


# @scenario SC-debug-14
def test_scala_end_is_one_line_with_the_exit_code(tmp_path):
    _need("scala-cli")
    with _scala_session(tmp_path) as w:
        _ends_with_one_line(w, 3, "Program exited with code 0",
                            lambda: "End of Debugging" in _messages_lines(w),
                            SCALA_START_TIMEOUT_S)


GO_EXIT_3 = GOTO_GO.replace("import \"fmt\"", "import (\n\t\"fmt\"\n\t\"os\"\n)") \
    .replace("\tfmt.Println(\"END\", z)\n", "\tfmt.Println(\"END\", z)\n\tos.Exit(3)\n")
GO_EXIT_3_SQ_LINE = GO_SQ_LINE + 3   # the import block took three more lines


# @scenario SC-debug-14
def test_dlv_end_names_a_nonzero_exit_code(tmp_path):
    """A Go program that ends with os.Exit(3): the line names code 3."""
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GO_EXIT_3, filename="main.go",
                    wait=2.0) as w:
        _ends_with_one_line(w, GO_EXIT_3_SQ_LINE, "Program exited with code 3",
                            lambda: not _children(w, "dlv"), GOTO_TIMEOUT_S)


# A program whose only breakpoint is in a function it never calls: it ends
# while the adapter starts, before any stop.
RUST_NEVER_STOPS = (
    "fn never() -> i32 {\n"
    "    let r = 7;\n"
    "    r\n"
    "}\n"
    "fn main() {\n"
    "    let x = 3;\n"
    "    println!(\"END {}\", x);\n"
    "}\n"
)
RUST_NEVER_LINE = 2


def _first_command_ends_the_session(w, letter, binary):
    """Breakpoint on a line that never runs, then Ctrl-G @letter as the
    session's first command: the program ends during the start, and that
    command ends the session with the end line, no box and no mark left."""
    _go_to_line(w, RUST_NEVER_LINE)
    w.key(CTRL_G, "b", delay=0.5)
    w.key(CTRL_G, letter, delay=1.0)
    assert _wait_for(w, lambda: "Program exited with code 0"
                     in _messages_lines(w), RUST_START_TIMEOUT_S), \
        "Messages should say the program exited with code 0:\n%s" % _screen(w)
    assert _no_modal(w), "the end should open no box:\n%s" % _screen(w)
    assert _wait_for(w, lambda: not _children(w, binary), STEP_TIMEOUT_S), \
        "the session should end with the program:\n%s" % _screen(w)
    assert _mark_line(w) is None, "no line should be marked:\n%s" % _screen(w)
    assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)


# @scenario SC-debug-13
def test_an_adapter_program_that_ends_at_the_start_ends_at_the_first_step(
        tmp_path):
    _need("rustc", "lldb-dap")
    with WpeSession(str(tmp_path), RUST_NEVER_STOPS, filename="main.rs",
                    wait=2.0, env_extra={"XWPE_DAP_ADAPTER": "lldb"}) as w:
        _first_command_ends_the_session(w, "s", "lldb-dap")


# @scenario SC-debug-13
def test_an_adapter_program_that_ends_at_the_start_ends_at_the_first_finish(
        tmp_path):
    _need("rustc", "lldb-dap")
    with WpeSession(str(tmp_path), RUST_NEVER_STOPS, filename="main.rs",
                    wait=2.0, env_extra={"XWPE_DAP_ADAPTER": "lldb"}) as w:
        _first_command_ends_the_session(w, "f", "lldb-dap")
