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

from wpe_driver import WpeSession, ALT, WAIT_SCALE, tool_usable

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
    """Breakpoint on JAVA_BREAK_LINE, Ctrl-G R, dismiss the one-time notice
    that the Java file switches the backend to jdb, wait for the stop."""
    _move_down(w, JAVA_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: "switching to jdb" in _screen(w), STEP_TIMEOUT_S)
    w.key("\r", delay=0.5)
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
        assert _wait_for(w, lambda: "switching to pdb" in _screen(w),
                         STEP_TIMEOUT_S)
        w.key("\r", delay=0.5)
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
        assert _wait_for(w, lambda: "switching to jdb" in _screen(w),
                         STEP_TIMEOUT_S)
        w.key("\r", delay=0.5)
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
        assert _wait_for(w, lambda: "switching to pdb" in _screen(w),
                         STEP_TIMEOUT_S)
        w.key("\r", delay=0.5)
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
