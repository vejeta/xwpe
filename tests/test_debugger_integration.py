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
import socket
import subprocess
import sys
import time

import pytest

import procinfo
from wpe_driver import (WpeSession, ALT, WAIT_SCALE, tool_usable,
                        CURRENT_LINE_BG, BREAKPOINT_BG,
                        UNCONFIRMED_BREAKPOINT_FG, incoherence,
                        late_reader_c_program, paste_text, end_paste_until,
                        PASTE_LINES,
                        PASTE_BYTES_PER_LINE)

CTRL_G = "\x07"         # debugger command prefix
DOWN = "\033OB"
UP = "\033OA"
F7 = "\033[18~"         # Trace (step into)
F8 = "\033[19~"         # Step (step over)
ALT_F5 = "\033[15;3~"   # User Screen
ESC = "\033"
F3 = "\033OR"             # the File Manager
F6 = "\033[17~"           # next window

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


def _python3_run_name():
    """The name the `python3` on PATH runs under.  pkgsrc's python3 is a
    shell wrapper that execs python3.N, so pdb's process is python3.N
    there; elsewhere it is python3."""
    python3 = shutil.which("python3")
    if python3 is None:
        return "python3"
    out = subprocess.run([python3, "-c",
                          "import os, sys; "
                          "print(os.path.basename(sys.executable))"],
                         capture_output=True, text=True).stdout.strip()
    return out or "python3"


# The command the editor runs -> the name its process runs under.
RUN_NAMES = {"python3": _python3_run_name()}


def _children(w, name):
    """PIDs of the editor's direct children started as the command `name`."""
    return procinfo.children_named(w.proc.pid, RUN_NAMES.get(name, name))


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
    an adapter that is silent, not one that has hung up.  The second exec
    redirects descriptor 3 again because NetBSD's sh closes it on exec."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_script(bindir / adapter, "#!/bin/sh\nexec 3>&1\nexec cat 3>&3 > %s\n"
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


DEEP_NAME_SLACK = 45    # the longest a path may overshoot `total`


PDB_CACHE_SLACK = 48    # py_compile writes __pycache__/p.cpython-3NN.pyc.<temp>
                        # beside the source: about 46 bytes more than p.py

def _capped_path_bytes(tmp_path, total, extra=0):
    """`total`, or the longest path the platform allows (PATH_MAX is 1024 on
    BSD, 4096 on Linux) if that is shorter, so the row tests a path at the
    limit and not past it.  `extra` keeps that many bytes free for files the
    tool writes beside the source."""
    limit = os.pathconf(str(tmp_path), "PC_PATH_MAX")
    return min(total, limit - DEEP_NAME_SLACK - 1 - extra)


def _deep_name(tmp_path, leaf, total, extra=0):
    """leaf under nested 40-byte directories until the full path is at least
    `total` bytes; creates the directories and returns the relative name."""
    total = _capped_path_bytes(tmp_path, total, extra)
    dirs = []
    while len(os.path.join(str(tmp_path), *dirs, leaf)) < total:
        dirs.append("d" * 40)
    rel = os.path.join(*dirs, leaf)
    os.makedirs(os.path.join(str(tmp_path), os.path.dirname(rel)))
    assert total <= len(os.path.join(str(tmp_path), rel)) < total + DEEP_NAME_SLACK
    return rel


# @scenario SC-debug-30
@pytest.mark.parametrize("total", LONG_PATH_BYTES)
def test_pdb_starts_on_a_source_under_a_very_long_path(tmp_path, total):
    """A Python file in a very deep directory still starts pdb and stops,
    marked, on its breakpoint (the path was cut short before)."""
    _need("python3")
    name = _deep_name(tmp_path, "p.py", total, PDB_CACHE_SLACK)
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


# A step over a line that takes longer than the editor's wait for a debugger
# answer (five seconds): the line sleeps eight.
PY_LONG_LINE_PROG = (
    "import time\n"
    "a = 0\n"
    "time.sleep(8)\n"
    "y = 2\n"
)
PY_LONG_LINE = 3
PY_LONG_LINE_RUN_S = 20


# @scenario SC-debug-05
def test_pdb_f8_over_a_line_that_runs_long_marks_the_next_line(tmp_path):
    """F8 over time.sleep(8) marks the line after it once the sleep is done:
    a step is not abandoned because the line takes a while."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_LONG_LINE_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, PY_LONG_LINE)
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == PY_LONG_LINE + 1,
                         PY_LONG_LINE_RUN_S), \
            "F8 over the long line should mark line %d (mark on %s):\n%s" \
            % (PY_LONG_LINE + 1, _mark_line(w), _screen(w))


def _says_the_program_is_running(w):
    """Ctrl-G K while the program runs is refused with a line on the status
    row and opens no Stack window: the key belongs to the editor."""
    w.key(CTRL_G, "k", delay=0.8)
    assert RUNNING_REFUSAL in _screen(w), \
        "Ctrl-G K should be refused with a line on the status row:\n%s" \
        % _screen(w)
    assert " Stack " not in _screen(w), \
        "no Stack window should open:\n%s" % _screen(w)


# A function that takes longer than the editor used to wait for pdb.
PY_SLOW_CALL_PROG = (
    "import time\n"
    "def slow(v):\n"
    "    time.sleep(8)\n"
    "    return v * v\n"
    "y = slow(3)\n"
    "print(y)\n"
)
PY_SLOW_BODY_LINE = 3         # time.sleep(8)
PY_SLOW_CALLER_LINE = 5       # y = slow(3)
PY_SLOW_AFTER_LINE = 6


# @scenario SC-debug-34
def test_pdb_finish_over_a_long_call_marks_the_caller(tmp_path):
    """Ctrl-G F from inside a function that runs eight seconds more marks the
    caller's line when it returns, and the editor keeps its keys meanwhile:
    neither "return" nor "up" is abandoned because the call takes a while."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_SLOW_CALL_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, PY_SLOW_BODY_LINE)
        _go_to_line(w, PY_SLOW_AFTER_LINE)
        w.key(CTRL_G, "f", delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_SLOW_CALLER_LINE,
                         PY_LONG_LINE_RUN_S), \
            "Ctrl-G F should mark line %d (mark on %s):\n%s" \
            % (PY_SLOW_CALLER_LINE, _mark_line(w), _screen(w))


# A line that takes eight seconds, between the breakpoint and the cursor.
PY_GOTO_LONG_PROG = (
    "import time\n"
    "a = 0\n"
    "time.sleep(8)\n"
    "y = 2\n"
    "z = 3\n"
)
PY_GOTO_LONG_BREAK_LINE = 2
PY_GOTO_LONG_CURSOR_LINE = 5


# @scenario SC-debug-06
def test_pdb_go_to_cursor_over_a_long_line_keeps_the_editors_keys(tmp_path):
    """F4 to a line past an eight second line: the editor refuses debugger
    keys with the running line meanwhile, then the cursor's line is marked
    and Go to cursor's own breakpoint is gone."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_GOTO_LONG_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, PY_GOTO_LONG_BREAK_LINE)
        _go_to_line(w, PY_GOTO_LONG_CURSOR_LINE)
        assert _wait_for(w, lambda: "F4 Go to cursor" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "the bar should offer F4 Go to cursor:\n%s" % _screen(w)
        w.key(F4, delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_GOTO_LONG_CURSOR_LINE,
                         PY_LONG_LINE_RUN_S), \
            "F4 should mark line %d (mark on %s):\n%s" \
            % (PY_GOTO_LONG_CURSOR_LINE, _mark_line(w), _screen(w))
        assert PY_GOTO_LONG_CURSOR_LINE not in _breakpoint_lines(w), \
            "Go to cursor's breakpoint should be gone at the stop:\n%s" \
            % _screen(w)


# F7 into code run by exec(): pdb stops in <string>, which has no source, and
# the editor steps on to the script's next line; the exec'd code takes seven
# seconds.
PY_INTERNAL_PROG = (
    "a = 0\n"
    "exec(\"import time\\ntime.sleep(7)\\nz = 2\")\n"
    "y = 2\n"
)
PY_INTERNAL_CALL_LINE = 2
PY_INTERNAL_NEXT_LINE = 3


# @scenario SC-debug-05
def test_pdb_f7_into_code_without_source_steps_on_to_the_next_line(tmp_path):
    """F7 on a line that runs exec() stops in Python's own <string>: the
    editor steps on, with the keys free, and marks the next line of the
    script once the exec'd code is done."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_INTERNAL_PROG, filename="p.py",
                    wait=1.5) as w:
        _start_py_session_on(w, PY_INTERNAL_CALL_LINE)
        w.key(F7, delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_INTERNAL_NEXT_LINE,
                         PY_LONG_LINE_RUN_S), \
            "F7 should end on line %d (mark on %s):\n%s" \
            % (PY_INTERNAL_NEXT_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-36
def test_pdb_keys_reach_the_editor_while_the_program_runs(tmp_path):
    """While the Python program runs, Ctrl-G K says on the status row that
    the program is running and opens no Stack window, as under gdb: the key
    belongs to the editor, and pdb is asked nothing until the program
    stops."""
    _need("python3")
    spec = SLEEPING_PROGS["pdb"]
    with WpeSession(str(tmp_path), spec["prog"], filename=spec["file"],
                    wait=2.0) as w:
        _move_down(w, spec["line"] - 1)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == spec["line"],
                         spec["start_s"]), \
            "pdb should stop on line %d:\n%s" % (spec["line"], _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        w._drain(RUNNING_SETTLE_S)   # asleep for ten minutes (drain scales)
        w.key(CTRL_G, "k", delay=0.8)
        assert RUNNING_REFUSAL in _screen(w), \
            "Ctrl-G K should be refused with a line on the status row:\n%s" \
            % _screen(w)
        assert " Stack " not in _screen(w), \
            "no Stack window should open:\n%s" % _screen(w)


# pdb shares its stdin with the program, so nothing typed in the editor can
# reach the program: the status row offers only Quit.
PDB_RUNNING_LINE = "The program is running: Debug > Quit ends the session"
TYPED_WHILE_RUNNING = "QZQ"


# @scenario SC-debug-35
def test_pdb_text_typed_while_the_program_runs_goes_into_the_file(tmp_path):
    """While the Python program runs, letters typed in the source window are
    inserted in the file, and Ctrl-G K's status row offers only Debug > Quit:
    the program cannot read them, so they must not vanish into a terminal
    nobody reads."""
    _need("python3")
    spec = SLEEPING_PROGS["pdb"]
    with WpeSession(str(tmp_path), spec["prog"], filename=spec["file"],
                    wait=2.0) as w:
        _move_down(w, spec["line"] - 1)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == spec["line"],
                         spec["start_s"]), \
            "pdb should stop on line %d:\n%s" % (spec["line"], _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        w._drain(RUNNING_SETTLE_S)   # asleep for ten minutes (drain scales)
        w.key(CTRL_G, "k", delay=0.8)
        assert PDB_RUNNING_LINE in _screen(w), \
            "Ctrl-G K should say %r under pdb:\n%s" \
            % (PDB_RUNNING_LINE, _screen(w))
        w.key(TYPED_WHILE_RUNNING, delay=0.8)
        w.save()
        assert TYPED_WHILE_RUNNING in w.text(), \
            "the letters typed while pdb's program runs should be in the " \
            "file:\n%s" % w.text()


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


LATE_READER_RETURN_LINE = 17      # return 0;  after the count is printed
LATE_READER_COUNT_RE = re.compile(r"COUNT:(\d+)")
PASTE_TIMEOUT_S = 90              # the editor forwards a paste key by key


# @scenario SC-debug-35
def test_text_pasted_before_the_program_reads_reaches_it_whole(tmp_path):
    """A paste larger than the pty holds, typed while the program under
    gdb is not yet reading: every byte reaches the program once it reads.
    With the pty master non-blocking the editor's write met a full pty and
    the rest of the paste was lost."""
    _need("gcc", "gdb")
    go = tmp_path / "go"
    with WpeSession(str(tmp_path), late_reader_c_program(str(go)),
                    filename="p.c", wait=1.5) as w:
        _move_down(w, LATE_READER_RETURN_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=0.2)
        assert _wait_for(w, lambda: "READY" in _screen(w), START_TIMEOUT_S), \
            "the program should be running and print READY:\n%s" % _screen(w)
        w.paste(paste_text(), go.touch)
        assert end_paste_until(
            w, lambda: LATE_READER_COUNT_RE.search(_screen(w)),
            PASTE_TIMEOUT_S), \
            "the program should count what was pasted:\n%s" % _screen(w)
        got = int(LATE_READER_COUNT_RE.search(_screen(w)).group(1))
        assert got == PASTE_LINES * PASTE_BYTES_PER_LINE, \
            "the program read %d of the %d bytes pasted for it" \
            % (got, PASTE_LINES * PASTE_BYTES_PER_LINE)


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
    "gdb-dap": {"file": "main.rs", "line": 2, "child": "gdb",
                "env": {"XWPE_DAP_ADAPTER": "gdb"},
                "need": ("rustc", "gdb"), "start_s": 60,
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


# A busy loop that runs until nine seconds of a68g's own clock have passed
# since t0, longer than the five the editor used to wait for a68g, which has
# no call to sleep.  It lasts the same on a fast or a slow machine.
A68_LONG_LOOP = "WHILE seconds - t0 < 9.0 DO SKIP OD;\n"
A68_LONG_RUN_S = 40
A68_LONG_PROG = (
    "INT y := 0;\n"
    "REAL t0 := seconds;\n"
    + A68_LONG_LOOP +
    "y := 2;\n"
    "print ((y, new line))\n"
)
A68_LONG_FIRST_LINE = 2      # REAL t0 := seconds;  the first stop
A68_LONG_AFTER_LINE = 4      # y := 2;  past the loop


def _start_a68_session_on(w, first_line, later_line):
    """Breakpoints on @first_line and @later_line, Ctrl-G R, and wait for
    the marked stop on the first."""
    _go_to_line(w, first_line)
    w.key(CTRL_G, "b", delay=0.4)
    _go_to_line(w, later_line)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == first_line, START_TIMEOUT_S), \
        "a68g should stop on line %d; mark is on %s:\n%s" \
        % (first_line, _mark_line(w), _screen(w))


# @scenario SC-debug-05
def test_a68g_continue_to_a_breakpoint_far_away_marks_it(tmp_path):
    """Ctrl-G R to a breakpoint nine seconds of run time away marks it when
    it is reached: the editor does not give up on a68g after five seconds."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_LONG_PROG, filename="p.a68",
                    wait=1.5) as w:
        _start_a68_session_on(w, A68_LONG_FIRST_LINE, A68_LONG_AFTER_LINE)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == A68_LONG_AFTER_LINE,
                         A68_LONG_RUN_S), \
            "Ctrl-G R should mark line %d (mark on %s):\n%s" \
            % (A68_LONG_AFTER_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-36
def test_a68g_keys_reach_the_editor_while_the_program_runs(tmp_path):
    """While the Algol 68 program runs, Ctrl-G K says on the status row that
    the program is running and opens no Stack window: the key belongs to the
    editor, and a68g is told nothing until the program stops."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_LONG_PROG, filename="p.a68",
                    wait=1.5) as w:
        _start_a68_session_on(w, A68_LONG_FIRST_LINE, A68_LONG_AFTER_LINE)
        w.key(CTRL_G, "r", delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == A68_LONG_AFTER_LINE,
                         A68_LONG_RUN_S), \
            "the stop should be marked once the program gets there " \
            "(mark on %s):\n%s" % (_mark_line(w), _screen(w))


# A procedure whose call takes about nine seconds.
A68_SLOW_CALL_PROG = (
    "PROC busy = (INT k) INT:\n"
    "BEGIN\n"
    "   REAL t0 := seconds;\n"
    "   " + A68_LONG_LOOP +
    "   k\n"
    "END;\n"
    "INT a := 1;\n"
    "INT y := busy (a);\n"
    "INT z := 2;\n"
    "print ((y, z, new line))\n"
)
A68_SLOW_BODY_LINE = 3       # REAL t0 := seconds;  in busy, before its loop
A68_SLOW_CALL_LINE = 8       # INT y := busy (a);
A68_SLOW_AFTER_LINE = 9      # INT z := 2;


# @scenario SC-debug-05
def test_a68g_f8_over_a_call_that_runs_long_marks_the_next_line(tmp_path):
    """F8 on a call that takes nine seconds marks the line after it once the
    call returns, and the editor refuses debugger keys meanwhile."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_SLOW_CALL_PROG, filename="p.a68",
                    wait=1.5) as w:
        _go_to_line(w, A68_SLOW_CALL_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == A68_SLOW_CALL_LINE,
                         START_TIMEOUT_S), \
            "a68g should stop on line %d; mark is on %s:\n%s" \
            % (A68_SLOW_CALL_LINE, _mark_line(w), _screen(w))
        w.key(F8, delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == A68_SLOW_AFTER_LINE,
                         A68_LONG_RUN_S), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (A68_SLOW_AFTER_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-34
def test_a68g_finish_over_a_long_call_marks_the_caller(tmp_path):
    """Ctrl-G F from inside a procedure that runs nine seconds more marks the
    line after the call when it returns, and the editor refuses debugger
    keys meanwhile."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_SLOW_CALL_PROG, filename="p.a68",
                    wait=1.5) as w:
        _go_to_line(w, A68_SLOW_BODY_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == A68_SLOW_BODY_LINE,
                         START_TIMEOUT_S), \
            "a68g should stop on line %d; mark is on %s:\n%s" \
            % (A68_SLOW_BODY_LINE, _mark_line(w), _screen(w))
        _go_to_line(w, A68_SLOW_AFTER_LINE)
        w.key(CTRL_G, "f", delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == A68_SLOW_AFTER_LINE,
                         A68_LONG_RUN_S), \
            "Ctrl-G F should mark line %d (mark on %s):\n%s" \
            % (A68_SLOW_AFTER_LINE, _mark_line(w), _screen(w))


# The program prints while the user has closed Messages and opened the File
# Manager; it runs on to a far breakpoint.  Times are a68g's own clock.
A68_PRINT_PROG = (
    "INT y := 0;\n"
    "REAL t0 := seconds;\n"
    "WHILE seconds - t0 < 5.0 DO SKIP OD;\n"
    "print ((\"late\", new line));\n"
    "WHILE seconds - t0 < 10.0 DO SKIP OD;\n"
    "y := 2;\n"
    "print ((y, new line))\n"
)
A68_PRINT_FIRST_LINE = 2
A68_PRINT_STOP_LINE = 6
A68_PRINT_WAIT_S = 6         # past the print, with the File Manager open


# @scenario SC-debug-36
def test_a68g_output_while_a_box_is_open_opens_no_window_over_it(tmp_path):
    """Messages closed, the File Manager open, the program prints: no new
    Messages window opens over the File Manager, Esc closes it, and after
    the stop Ctrl-G K opens the Stack window."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_PRINT_PROG, filename="p.a68",
                    wait=1.5) as w:
        _start_a68_session_on(w, A68_PRINT_FIRST_LINE, A68_PRINT_STOP_LINE)
        w.key(CTRL_G, "r", delay=0.5)
        w.key(F6, delay=0.5)             # Messages on top
        w.key(CTRL_W, delay=0.5)
        assert " Messages " not in _screen(w), \
            "Ctrl-W should have closed Messages:\n%s" % _screen(w)
        w.key(F3, delay=0.8)
        w._drain(A68_PRINT_WAIT_S)       # the program prints meanwhile
        assert " Messages " not in _screen(w), \
            "no Messages window should open over the File Manager:\n%s" \
            % _screen(w)
        w.key(ESC, delay=0.8)
        assert _wait_for(w, lambda: _mark_line(w) == A68_PRINT_STOP_LINE,
                         A68_LONG_RUN_S), \
            "the stop should be marked on line %d (mark on %s):\n%s" \
            % (A68_PRINT_STOP_LINE, _mark_line(w), _screen(w))
        w.key(CTRL_G, "k", delay=0.8)
        assert _wait_for(w, lambda: len(_stack_frames(w)) >= 1,
                         STEP_TIMEOUT_S), \
            "Ctrl-G K should open the Stack window:\n%s" % _screen(w)


# A program whose own text is a68g's prompt, with no line end, as a68g
# writes it at a stop; then it runs on to a far stop.
A68_FAKE_TEXT = "print ((\"(a68g) \"));\n"
# The same, after the line a68g writes just before its prompt.
A68_FAKE_THREAD_TEXT = ("print ((\"This is the main thread\", new line, "
                        "\"(a68g) \"));\n")
A68_FAKE_STOP_LINE = 5
A68_FAKE_SETTLE_S = 2        # well inside the nine-second loop


def _a68g_fake_prog(fake_text):
    """A program that prints @fake_text, then runs nine seconds to line
    A68_FAKE_STOP_LINE."""
    return ("INT y := 0;\n" + fake_text +
            "REAL t0 := seconds;\n"
            "WHILE seconds - t0 < 9.0 DO SKIP OD;\n"
            "y := 2;\n"
            "print ((y, new line))\n")


def _assert_the_fake_prompt_does_not_end_the_run(tmp_path, fake_text):
    """Run to the far breakpoint past @fake_text: debugger keys are still
    refused after it, and the real stop is the one marked."""
    with WpeSession(str(tmp_path), _a68g_fake_prog(fake_text),
                    filename="p.a68", wait=1.5) as w:
        _go_to_line(w, A68_FAKE_STOP_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _children(w, "a68g"), START_TIMEOUT_S), \
            "a68g should be running:\n%s" % _screen(w)
        w._drain(A68_FAKE_SETTLE_S)      # past the start and the fake prompt
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == A68_FAKE_STOP_LINE,
                         A68_LONG_RUN_S), \
            "the real stop, line %d, should be marked (mark on %s):\n%s" \
            % (A68_FAKE_STOP_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-05
def test_a68g_program_text_with_the_prompt_does_not_end_the_run(tmp_path):
    """A program that prints "(a68g) " and no line end, exactly what a68g
    prints when it stops, does not stop the run there: debugger keys are
    still refused while the program runs on, and the real stop, on the far
    breakpoint, is the one that is marked."""
    _need("a68g")
    _assert_the_fake_prompt_does_not_end_the_run(tmp_path, A68_FAKE_TEXT)


# @scenario SC-debug-05
def test_a68g_program_text_with_the_thread_line_and_prompt_runs_on(tmp_path):
    """A program that prints a68g's thread line and then its prompt does not
    stop the run either: a real stop also has the numbered source line and
    the column marker before them."""
    _need("a68g")
    _assert_the_fake_prompt_does_not_end_the_run(tmp_path,
                                                  A68_FAKE_THREAD_TEXT)


# @scenario SC-debug-18
@pytest.mark.parametrize("debugger", [
    "jdb",
    "pdb",
    "dlv",
    "lldb-dap",
    "gdb-dap",
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


# ---- the debug adapters on the run controller -----------------------------
#
# One program per language, two breakpoints in it: the line that prints
# "ready" and the line that prints "done".  Between them it sleeps, or prints
# a tick every tenth of a second, and it writes its own PID to a file first.

DAP_ADAPTERS = ["dlv", "lldb-dap", "gdb-dap"]
DAP_CASES = {
    "dlv": {"file": "main.go", "lang": "go", "env": {},
            "need": ("dlv", "go"), "child": "dlv", "start_s": 40},
    "lldb-dap": {"file": "main.rs", "lang": "rust",
                 "env": {"XWPE_DAP_ADAPTER": "lldb"},
                 "need": ("rustc", "lldb-dap"), "child": "lldb-dap",
                 "start_s": 60},
    "gdb-dap": {"file": "main.rs", "lang": "rust",
                "env": {"XWPE_DAP_ADAPTER": "gdb"},
                "need": ("rustc", "gdb"), "child": "gdb", "start_s": 60},
}
DAP_PIDFILE = "program.pid"
DAP_LONG_SLEEP_S = 600        # the program is running for as long as a test needs
DAP_FAR_SLEEP_S = 8           # long enough to press keys while it runs
DAP_TICKS = 60                # six seconds of one line every tenth of a second
DAP_FAR_RUN_S = 40
DAP_TICK_LAST_LINE = "TICK 59"

GO_DAP_HEAD = (
    "package main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n\t\"strconv\"\n"
    "\t\"time\"\n)\n\nfunc main() {\n"
    "\tos.WriteFile(%(pidfile)s, []byte(strconv.Itoa(os.Getpid())), 0644)\n"
    "\tfmt.Println(\"ready\")\n")
GO_DAP_SLEEP = "\ttime.Sleep(%d * time.Second)\n"
GO_DAP_TICKS = (
    "\tfor i := 0; i < %d; i++ {\n\t\tfmt.Println(\"TICK\", i)\n"
    "\t\ttime.Sleep(100 * time.Millisecond)\n\t}\n")
GO_DAP_TAIL = "\tfmt.Println(\"done\")\n}\n"
RUST_DAP_HEAD = (
    "fn main() {\n"
    "    std::fs::write(%(pidfile)s, std::process::id().to_string()).unwrap();\n"
    "    println!(\"ready\");\n")
RUST_DAP_SLEEP = "    std::thread::sleep(std::time::Duration::from_secs(%d));\n"
RUST_DAP_TICKS = (
    "    for i in 0..%d {\n        println!(\"TICK {}\", i);\n"
    "        std::thread::sleep(std::time::Duration::from_millis(100));\n    }\n")
RUST_DAP_TAIL = "    println!(\"done\");\n}\n"
DAP_PROGRAM_PARTS = {"go": (GO_DAP_HEAD, GO_DAP_SLEEP, GO_DAP_TICKS,
                            GO_DAP_TAIL),
                     "rust": (RUST_DAP_HEAD, RUST_DAP_SLEEP, RUST_DAP_TICKS,
                              RUST_DAP_TAIL)}


def _dap_program(case, pidfile, sleep_s=None, ticks=None):
    """The source text, with the file name of the PID file in it, and the
    1-based lines of its "ready" and "done" prints."""
    head, sleep, tick, tail = DAP_PROGRAM_PARTS[case["lang"]]
    quoted = '"%s"' % pidfile
    middle = (sleep % sleep_s) if sleep_s is not None else (tick % ticks)
    text = (head % {"pidfile": quoted}) + middle + tail
    lines = text.splitlines()
    ready = [i + 1 for i, l in enumerate(lines) if '"ready"' in l][0]
    done = [i + 1 for i, l in enumerate(lines) if '"done"' in l][0]
    return text, ready, done


class DapRun:
    """A session on one adapter's program: its editor, its two lines and the
    file the program writes its PID to."""

    def __init__(self, tmp_path, adapter, sleep_s=None, ticks=None):
        self.case = DAP_CASES[adapter]
        _need(*self.case["need"])
        if self.case["lang"] == "go":
            (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
        self.pidfile = tmp_path / DAP_PIDFILE
        self.text, self.ready, self.done = _dap_program(
            self.case, str(self.pidfile), sleep_s, ticks)
        self.session = WpeSession(
            str(tmp_path), self.text, filename=self.case["file"], wait=2.0,
            env_extra=self.case["env"] or None)

    def pid(self):
        """The program's own PID, from the file it wrote."""
        return int(self.pidfile.read_text())

    def stop_on_ready(self, w, also_done=False):
        """A breakpoint on the "ready" line (and the "done" line), Ctrl-G R,
        and wait for the mark on the first."""
        _go_to_line(w, self.ready)
        w.key(CTRL_G, "b", delay=0.5)
        if also_done:
            _go_to_line(w, self.done)
            w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == self.ready,
                         self.case["start_s"]), \
            "the adapter should stop on line %d:\n%s" \
            % (self.ready, _screen(w))


def _run_on_towards_done(run, w):
    """Both breakpoints, stop on the first, then Ctrl-G R: the program runs
    on towards the second."""
    run.stop_on_ready(w, also_done=True)
    w.key(CTRL_G, "r", delay=0.5)


def _kill_program_if_left(run):
    """After a failed row: end the program the adapter left behind, by the
    PID it wrote itself.  The adapter may end the program between the look
    and the kill, as it does once the editor has closed; a program already
    gone is the state this wants."""
    try:
        pid = run.pid()
    except (OSError, ValueError):
        return
    if _process_gone(pid):
        return
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


# @scenario SC-debug-47
@pytest.mark.parametrize("adapter", DAP_ADAPTERS)
def test_quit_while_running_leaves_no_program_under_every_adapter(
        tmp_path, adapter):
    """Debug > Quit while the program sleeps ends the adapter and the
    program itself: its PID is gone, not left running under init."""
    run = DapRun(tmp_path, adapter, sleep_s=DAP_LONG_SLEEP_S)
    try:
        with run.session as w:
            run.stop_on_ready(w)
            w.key(CTRL_G, "r", delay=0.5)
            w._drain(RUNNING_SETTLE_S)
            pid = run.pid()
            assert not _process_gone(pid), "the program should be running"
            w.menu(ALT.DEBUG, "q", delay=0.5)
            assert _wait_for(w, lambda: not _children(w, run.case["child"]),
                             STEP_TIMEOUT_S), \
                "Debug > Quit should end the adapter:\n%s" % _screen(w)
            assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
                "the program (pid %d) should be gone after Quit:\n%s" \
                % (pid, _screen(w))
    finally:
        _kill_program_if_left(run)


# @scenario SC-debug-38
@pytest.mark.parametrize("adapter", DAP_ADAPTERS)
def test_continue_to_a_breakpoint_far_away_is_marked_under_every_adapter(
        tmp_path, adapter):
    """Ctrl-G R to a breakpoint eight seconds of run time away: the editor
    takes keys meanwhile, and the stop is marked when it is reached.  The
    editor's keys are what this row guards; the late mark already worked
    with the blocking wait, so it is kept here as a guard."""
    run = DapRun(tmp_path, adapter, sleep_s=DAP_FAR_SLEEP_S)
    with run.session as w:
        _run_on_towards_done(run, w)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == run.done, DAP_FAR_RUN_S), \
            "the stop on line %d should be marked (mark on %s):\n%s" \
            % (run.done, _mark_line(w), _screen(w))
        w.key(CTRL_G, "r", delay=0.5)    # the session goes on: to the end
        assert _wait_for(w, lambda: not _children(w, run.case["child"]),
                         STEP_TIMEOUT_S + DAP_FAR_SLEEP_S), \
            "Ctrl-G R after the late stop should run the program out:\n%s" \
            % _screen(w)


# @scenario SC-debug-36
@pytest.mark.parametrize("adapter", DAP_ADAPTERS)
def test_keys_reach_the_editor_while_the_program_runs_under_every_adapter(
        tmp_path, adapter):
    """While the program sleeps, Ctrl-G K is refused on the status row with
    the line that offers only Debug > Quit, no Stack window opens, and the
    letters the user types go into the file."""
    run = DapRun(tmp_path, adapter, sleep_s=DAP_LONG_SLEEP_S)
    try:
        with run.session as w:
            run.stop_on_ready(w)
            w.key(CTRL_G, "r", delay=0.5)
            w._drain(RUNNING_SETTLE_S)
            w.key(CTRL_G, "k", delay=0.8)
            assert PDB_RUNNING_LINE in _screen(w), \
                "Ctrl-G K should be refused with the Quit-only line:\n%s" \
                % _screen(w)
            assert " Stack " not in _screen(w), \
                "no Stack window should open:\n%s" % _screen(w)
            w.key(*TYPED_WHILE_RUNNING, delay=0.15)
            w.save()
            assert TYPED_WHILE_RUNNING in w.text(), \
                "typing while the program runs should edit the source"
    finally:
        _kill_program_if_left(run)


# @scenario SC-debug-18
@pytest.mark.parametrize("adapter", DAP_ADAPTERS)
def test_a_stop_while_the_file_manager_is_open_waits_for_it_under_an_adapter(
        tmp_path, adapter):
    """The File Manager is opened while the program runs on towards its
    second breakpoint: the stop arrives under it, is held, and is shown
    with the mark once Esc closes the File Manager."""
    run = DapRun(tmp_path, adapter, sleep_s=DAP_FAR_SLEEP_S)
    with run.session as w:
        _run_on_towards_done(run, w)
        _open_the_file_manager(w)
        w._drain(DAP_FAR_SLEEP_S + 2)
        assert FILE_MANAGER_FIELD in _screen(w), \
            "the File Manager should still be drawn after the stop " \
            "(mark on %s):\n%s" % (_mark_line(w), _screen(w))
        w.key(ESC, delay=0.6)
        assert FILE_MANAGER_FIELD not in _screen(w), \
            "Esc should close the File Manager:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == run.done, STEP_TIMEOUT_S), \
            "Esc should show the stop, mark on line %d (mark on %s):\n%s" \
            % (run.done, _mark_line(w), _screen(w))


# @scenario SC-debug-36
@pytest.mark.parametrize("adapter", DAP_ADAPTERS)
def test_output_with_messages_closed_leaves_the_editor_idle_under_an_adapter(
        tmp_path, adapter):
    """Messages is closed while the program prints a line every tenth of a
    second on its way to the second breakpoint: no window opens, the editor
    stays idle, the stop is marked, and everything the program printed is
    on the User Screen (Alt-F5)."""
    run = DapRun(tmp_path, adapter, ticks=DAP_TICKS)
    with run.session as w:
        _run_on_towards_done(run, w)
        w.key(F6, delay=0.3)             # Messages on top, when it is open
        if " Messages " in _screen(w):
            w.key(CTRL_W, delay=0.3)
        assert " Messages " not in _screen(w), \
            "Messages should be closed:\n%s" % _screen(w)
        _assert_idle(w, CPU_SAMPLE_S, "while the program prints")
        assert " Messages " not in _screen(w), \
            "no Messages window should open while the program runs:\n%s" \
            % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == run.done, DAP_FAR_RUN_S), \
            "the stop on line %d should be marked (mark on %s):\n%s" \
            % (run.done, _mark_line(w), _screen(w))
        w.key(ALT_F5, delay=1.0)
        assert DAP_TICK_LAST_LINE in _screen(w), \
            "Alt-F5 should show what the program printed:\n%s" % _screen(w)
        w.key("\r", delay=0.8)


# @scenario SC-debug-05
def test_go_to_cursor_as_the_first_command_stops_at_the_cursor_under_dlv(
        tmp_path):
    """Go to cursor before any session starts dlv and runs on to the
    cursor's line, with no breakpoint set.  It stopped at the start of main,
    the adapter's own entry stop."""
    run = DapRun(tmp_path, "dlv", sleep_s=2)
    with run.session as w:
        _go_to_line(w, run.done)
        w.key(CTRL_G, "u", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == run.done,
                         run.case["start_s"]), \
            "Go to cursor should stop on line %d (mark on %s):\n%s" \
            % (run.done, _mark_line(w), _screen(w))
        assert _children(w, "dlv"), "dlv should still run the session"


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


# A program that prints a line every tenth of a second for six seconds on
# its way to the next breakpoint.
TICKING_C_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int i = 0;\n"
    "  for (i = 0; i < 60; i++)\n"
    "  {\n"
    "    printf(\"TICK %d\\n\", i);\n"
    "    fflush(stdout);\n"
    "    usleep(100000);\n"
    "  }\n"
    "  i = 99;\n"
    "  return 0;\n"
    "}\n"
)
TICKING_FIRST_LINE = 5    # int i = 0;
TICKING_AFTER_LINE = 12   # i = 99;  after the loop
TICKING_RUN_S = 6
TICKING_LAST_LINE = "TICK 59"
CPU_SAMPLE_S = 2.0


def _cpu_used_over(w, seconds):
    """CPU ticks the editor used while it sat @seconds reading the screen,
    or None when its process is gone."""
    before = _cpu_ticks(w.proc.pid)
    w._drain(seconds)
    after = _cpu_ticks(w.proc.pid)
    if before is None or after is None:
        return None
    return after - before


def _assert_idle(w, seconds, what):
    """The editor used no more CPU over @seconds than an idle one does."""
    used = _cpu_used_over(w, seconds)
    if used is None:
        return
    assert used < IDLE_CPU_TICKS * seconds * WAIT_SCALE, \
        "the editor should be idle %s, used %d ticks in %.1fs" \
        % (what, used, seconds)


# @scenario SC-debug-36
def test_gdb_output_with_messages_closed_leaves_the_editor_idle(tmp_path):
    """Messages is closed while gdb's program prints on its way to the next
    breakpoint: the editor stays idle meanwhile, the stop is marked, and
    everything the program printed is on the User Screen (Alt-F5).  The
    output nobody read kept the pty readable, so the editor woke without
    end and used a whole core until the program stopped."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), TICKING_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _move_down(w, TICKING_FIRST_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        _move_down(w, TICKING_AFTER_LINE - TICKING_FIRST_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == TICKING_FIRST_LINE,
                         START_TIMEOUT_S), \
            "gdb should stop on line %d:\n%s" % (TICKING_FIRST_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=0.3)
        w.key(F6, delay=0.3)             # Messages on top
        w.key(CTRL_W, delay=0.3)
        assert " Messages " not in _screen(w), \
            "Ctrl-W should have closed Messages:\n%s" % _screen(w)
        _assert_idle(w, CPU_SAMPLE_S, "while the program prints")
        assert _wait_for(w, lambda: _mark_line(w) == TICKING_AFTER_LINE,
                         STEP_TIMEOUT_S + TICKING_RUN_S), \
            "the stop on line %d should be marked (mark on %s):\n%s" \
            % (TICKING_AFTER_LINE, _mark_line(w), _screen(w))
        w.key(ALT_F5, delay=1.0)
        assert TICKING_LAST_LINE in _screen(w), \
            "Alt-F5 should show what the program printed:\n%s" % _screen(w)
        w.key("\r", delay=0.8)


# @scenario SC-debug-13
def test_the_editor_is_idle_after_a_gdb_session_ended(tmp_path):
    """Once the program ran to its end and gdb is gone, the editor sleeps
    until a key: no descriptor of the ended session wakes it."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_G, "r", delay=0.3)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "gdb should be gone after the program ended:\n%s" % _screen(w)
        _assert_idle(w, CPU_SAMPLE_S, "after the session ended")
        w.key("Q", delay=0.3)
        w.save()
        assert "Q  int x = 3;" in w.text(), \
            "typing after the session should edit the source"


# A java launcher that says a lot before it starts the real VM, as a
# wrapper script that reports its settings would: it waits three seconds,
# prints a line every tenth of a second for six, then runs the VM.  It
# prints while jdb starts, before the VM connects.
FAKE_JAVA_TALKS = ("#!/bin/sh\nsleep 3\ni=0\nwhile [ $i -lt 60 ]; do\n"
                   "  echo \"VMTICK $i\"\n  i=$((i + 1))\n  sleep 0.1\ndone\n"
                   "exec '%s' \"$@\"\n")
JDB_TALK_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 3;\n"
    "        System.out.println(x);\n"
    "    }\n"
    "}\n"
)
JDB_TALK_BREAK_LINE = 3       # int x = 3;
JDB_TALK_QUIET_S = 4          # past the launcher's three-second wait
JDB_TALK_START_S = 30         # javac, the launcher and the VM


# @scenario SC-debug-17
def test_jdb_start_with_messages_closed_leaves_the_editor_idle(tmp_path):
    """Messages is closed while jdb starts and the java launcher prints:
    the editor stays idle, and the start goes on to the stop.  What the
    launcher printed was left unread with Messages closed, so the pty woke
    the editor without end until the VM connected."""
    _need("jdb", "javac")
    env = _fake_jdk_env(tmp_path, FAKE_JAVA_TALKS % _jdk_java())
    with WpeSession(str(tmp_path), JDB_TALK_PROG, filename="P.java",
                    wait=2.0, env_extra=env) as w:
        _move_down(w, JDB_TALK_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: WAITING_FOR_JDB in _screen(w),
                         JDB_TALK_START_S), \
            "the start should wait for the VM:\n%s" % _screen(w)
        w.key(CTRL_W, delay=0.5)         # Messages is on top while it starts
        assert " Messages " not in _screen(w), \
            "Ctrl-W should have closed Messages:\n%s" % _screen(w)
        w._drain(JDB_TALK_QUIET_S)       # the launcher prints from here on
        _assert_idle(w, CPU_SAMPLE_S, "while the java launcher prints")
        assert _wait_for(w, lambda: _mark_line(w) == JDB_TALK_BREAK_LINE,
                         JDB_TALK_START_S), \
            "the start should go on to the stop on line %d (mark on %s):" \
            "\n%s" % (JDB_TALK_BREAK_LINE, _mark_line(w), _screen(w))


SLEEPY_NEXT_LINE = 8     # printf(...);  one F8 after the second stop
FILE_MANAGER_FIELD = "Name"
ALT_O = "\033o"           # the Options menu
OPTIONS_MENU_ROWS = ("Adjust Colors", "Save Options", "Compiler",
                     "Debugger")
ALT_0 = "\0330"           # the Window list
WINDOW_LIST_TITLE = "Windows"
OPENED_FILE = "q.c"       # a second file, opened from the File Manager
OPENED_FILE_TEXT = "/* opened from the file manager */\nint q;\n"


def _run_on_towards_the_second_stop(w):
    """Stop gdb on SLEEPY_FIRST_LINE, then continue: the program sleeps
    SLEEPY_RUN_S seconds before it stops on SLEEPY_AFTER_LINE, and the
    editor takes keys meanwhile."""
    _move_down(w, SLEEPY_FIRST_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    _move_down(w, SLEEPY_AFTER_LINE - SLEEPY_FIRST_LINE)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == SLEEPY_FIRST_LINE,
                     START_TIMEOUT_S), \
        "gdb should stop on line %d:\n%s" % (SLEEPY_FIRST_LINE, _screen(w))
    w.key(CTRL_G, "r", delay=0.5)


def _let_the_program_stop(w):
    """Wait past the program's sleep, so its stop has arrived."""
    w._drain(SLEEPY_RUN_S + 2)


def _assert_the_stop_is_shown_and_f8_steps(w, how):
    """The source is on top marked on the stop, and F8 steps from it."""
    assert _wait_for(w, lambda: _mark_line(w) == SLEEPY_AFTER_LINE,
                     STEP_TIMEOUT_S), \
        "%s should show the stop, mark on line %d (mark on %s):\n%s" \
        % (how, SLEEPY_AFTER_LINE, _mark_line(w), _screen(w))
    assert _step(w, F8, SLEEPY_NEXT_LINE), \
        "F8 should mark line %d (mark on %s):\n%s" \
        % (SLEEPY_NEXT_LINE, _mark_line(w), _screen(w))


def _open_the_file_manager(w):
    w.key(F3, delay=0.5)
    assert FILE_MANAGER_FIELD in _screen(w), \
        "the File Manager should be open:\n%s" % _screen(w)


# @scenario SC-debug-18
def test_a_stop_while_the_file_manager_is_open_waits_for_it(tmp_path):
    """The File Manager is opened while gdb's program sleeps on its way to
    the next breakpoint.  The stop arrives under it: it must not draw the
    source over the File Manager or leave it open.  After Esc the source
    is on top with the mark on the stop, and F8 steps instead of typing
    into the File Manager's Name field."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_on_towards_the_second_stop(w)
        _open_the_file_manager(w)
        _let_the_program_stop(w)
        w.key(ESC, delay=0.6)
        assert FILE_MANAGER_FIELD not in _screen(w), \
            "Esc should close the File Manager the stop arrived under:\n%s" \
            % _screen(w)
        _assert_the_stop_is_shown_and_f8_steps(w, "Esc")


# @scenario SC-debug-18
def test_a_stop_while_the_file_manager_is_open_shows_in_a_file_opened(
        tmp_path):
    """The File Manager is opened while gdb's program sleeps on its way to
    the next breakpoint, and the user opens another file from it, its
    commonest way out.  The File Manager stays drawn whole after the stop
    arrives (the stop used to paint the source over it, the name then
    typed into the hidden box).  Once the file is open the stop is shown
    and F8 steps, instead of 'The program is running' for ever."""
    _need("gcc", "gdb")
    (tmp_path / OPENED_FILE).write_text(OPENED_FILE_TEXT)
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_on_towards_the_second_stop(w)
        _open_the_file_manager(w)
        _let_the_program_stop(w)
        assert FILE_MANAGER_FIELD in _screen(w), \
            "the File Manager should still be drawn after the stop " \
            "(mark on %s):\n%s" % (_mark_line(w), _screen(w))
        w.key(OPENED_FILE, delay=0.6)
        w.key("\r", delay=1.2)
        _assert_the_stop_is_shown_and_f8_steps(w, "Opening %s" % OPENED_FILE)
        w.key(ALT_0, delay=0.8)
        assert OPENED_FILE in _screen(w), \
            "%s should be open, listed in the Window list:\n%s" \
            % (OPENED_FILE, _screen(w))


# @scenario SC-debug-18
def test_a_stop_while_the_file_manager_is_open_shows_when_switching_away(
        tmp_path):
    """The File Manager is opened while gdb's program sleeps on its way to
    the next breakpoint, and left with F6 instead of Esc.  The stop that
    arrived under it is shown then: the source is marked on the stop and
    F8 steps, instead of 'The program is running' for ever.  This row
    guards the hold itself: without the hold the stop shows at once and
    the row passes too."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_on_towards_the_second_stop(w)
        _open_the_file_manager(w)
        _let_the_program_stop(w)
        w.key(F6, delay=1.0)
        _assert_the_stop_is_shown_and_f8_steps(w, "F6")


# @scenario SC-debug-18
def test_a_stop_while_a_menu_is_open_waits_for_it(tmp_path):
    """The Options menu is opened while gdb's program sleeps on its way to
    the next breakpoint.  The stop arrives under the menu: the menu box
    stays drawn whole until Esc (the stop used to switch windows and
    paint the source over it), then the source is on top marked on the
    stop, and F8 steps."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_on_towards_the_second_stop(w)
        w.key(ALT_O, delay=0.5)
        _let_the_program_stop(w)
        shown = _screen(w)
        for row in OPTIONS_MENU_ROWS:
            assert row in shown, \
                "the Options menu should still be drawn whole after the " \
                "stop time (missing %r):\n%s" % (row, shown)
        w.key(ESC, delay=0.6)
        _assert_the_stop_is_shown_and_f8_steps(w, "Esc")


# @scenario SC-debug-18
def test_a_stop_while_the_window_list_is_open_waits_for_it(tmp_path):
    """The Window list (Alt-0) is opened while gdb's program sleeps on its
    way to the next breakpoint.  The list has its own key loop: the stop
    used to paint the source over it, and the next keys went to the hidden
    list.  The list stays drawn whole until Esc, then the source is on top
    marked on the stop, and F8 steps."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_on_towards_the_second_stop(w)
        w.key(ALT_0, delay=0.8)
        assert WINDOW_LIST_TITLE in _screen(w), \
            "the Window list should be open:\n%s" % _screen(w)
        _let_the_program_stop(w)
        assert WINDOW_LIST_TITLE in _screen(w), \
            "the Window list should still be drawn after the stop " \
            "(mark on %s):\n%s" % (_mark_line(w), _screen(w))
        w.key(ESC, delay=1.0)
        _assert_the_stop_is_shown_and_f8_steps(w, "Esc")


ALT_P = "\033p"           # the Project menu
PROJECT_FILE = "demo.prj"
PROJECT_TEXT = "CMP=\tgcc\nEXENAME=\tdemo\nFILES=\tp.c\n"
PROJECT_WINDOW_TITLE = "Project: " + PROJECT_FILE


def _open_the_project_window(w):
    """Project > Open Project, then Enter twice: the picker's Files list,
    then the only project in it."""
    w.key(ALT_P, delay=0.6)
    w.key("p", delay=0.8)
    w.key("\r", delay=0.6)
    w.key("\r", delay=1.0)
    assert PROJECT_WINDOW_TITLE in _above_the_status_row(w), \
        "the Project window should be open:\n%s" % _screen(w)


def _above_the_status_row(w):
    """The screen without its status row, which names the open project
    whichever window is on top."""
    return "\n".join(w.display(drain=0)[:-1])


# @scenario SC-debug-18
def test_a_stop_while_the_project_window_is_open_waits_for_it(tmp_path):
    """The Project window is opened while gdb's program sleeps on its way
    to the next breakpoint.  It has its own key loop, like the Window
    list: the stop used to paint the source over it, and F8 then went to
    the hidden window.  It stays drawn until Esc, then the source is on
    top marked on the stop, and F8 steps."""
    _need("gcc", "gdb")
    (tmp_path / PROJECT_FILE).write_text(PROJECT_TEXT)
    with WpeSession(str(tmp_path), SLEEPY_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_on_towards_the_second_stop(w)
        _open_the_project_window(w)
        _let_the_program_stop(w)
        assert PROJECT_WINDOW_TITLE in _above_the_status_row(w), \
            "the Project window should still be drawn after the stop " \
            "(mark on %s):\n%s" % (_mark_line(w), _screen(w))
        w.key(ESC, delay=1.0)
        _assert_the_stop_is_shown_and_f8_steps(w, "Esc")


C_CURSOR_FIRST_LINE = 11  # x = y + 1;  the cursor for Go to cursor
C_CURSOR_NEXT_LINE = 12   # return 0;  the line after the cursor's


def _go_to_cursor_as_the_first_command(w):
    """No breakpoint, cursor on line 11, Ctrl-G U before any session."""
    _move_down(w, C_CURSOR_FIRST_LINE - 1)
    w.key(CTRL_G, "u", delay=1.0)


# @scenario SC-debug-05
def test_go_to_cursor_as_the_first_command_starts_a_session_that_works(
        tmp_path):
    """Go to cursor before any session starts gdb and runs on to the
    cursor's line, and the session goes on: F8 marks the next line.  The
    session used to wait for ever: the start breakpoint's removal was sent
    while the program ran, and its answer swallowed the stop.  It then
    stopped at main and never reached the cursor."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _go_to_cursor_as_the_first_command(w)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_FIRST_LINE,
                         START_TIMEOUT_S), \
            "Go to cursor should start the session and stop on line %d " \
            "(mark on %s):\n%s" \
            % (C_CURSOR_FIRST_LINE, _mark_line(w), _screen(w))
        assert _children(w, "gdb"), "gdb should still run the session"
        assert _step(w, F8, C_CURSOR_NEXT_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (C_CURSOR_NEXT_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-05
def test_go_to_cursor_as_the_first_command_stops_at_the_cursor(tmp_path):
    """Borland's F4 before any session runs to the cursor's line."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _go_to_cursor_as_the_first_command(w)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_FIRST_LINE,
                         START_TIMEOUT_S), \
            "Go to cursor should stop on line %d (mark on %s):\n%s" \
            % (C_CURSOR_FIRST_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-05
def test_go_to_cursor_as_the_first_command_into_a_called_function(tmp_path):
    """The cursor is inside sq, which main calls: Go to cursor before any
    session runs on and stops on that line, the session alive.  It ran the
    program to its end, because gdb's until stops only in the frame that
    runs or an outer one."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _move_down(w, C_CALLEE_LINE - 1)
        w.key(CTRL_G, "u", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == C_CALLEE_LINE,
                         START_TIMEOUT_S), \
            "Go to cursor should stop on line %d in sq (mark on %s):\n%s" \
            % (C_CALLEE_LINE, _mark_line(w), _screen(w))
        assert _children(w, "gdb"), "gdb should still run the session"


# @scenario SC-debug-05
def test_go_to_cursor_at_a_stop_into_a_called_function(tmp_path):
    """Stopped in main before the call, the cursor moved up into sq: Go to
    cursor stops on that line.  until ran past it, to the end."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        for _ in range(C_BREAK_LINE - C_CALLEE_LINE):
            w.key(UP, delay=0.1)
        w.key(CTRL_G, "u", delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == C_CALLEE_LINE,
                         STEP_TIMEOUT_S), \
            "Go to cursor should stop on line %d in sq (mark on %s):\n%s" \
            % (C_CALLEE_LINE, _mark_line(w), _screen(w))
        assert _children(w, "gdb"), "gdb should still run the session"


# A program in two source files: main in p.c calls bb, which p.c takes in
# from b.c.  Line 5 of b.c is inside bb; line 5 of p.c is main's first line,
# where the session stops, so a line number read in the wrong file runs on
# to the end of the program.
TWO_FILE_MAIN_C = (
    "#include <stdio.h>\n"
    "#include \"b.c\"\n"
    "int main(void)\n"
    "{\n"
    "  int x = 3;\n"
    "  int y = bb(x);\n"
    "  printf(\"OUTMARK %d\\n\", y);\n"
    "  return 0;\n"
    "}\n"
)
TWO_FILE_SECOND_C = (
    "int bb(int v)\n"
    "{\n"
    "  int r = v;\n"
    "  r = r + 1;\n"
    "  r = r * 2;\n"
    "  return r;\n"
    "}\n"
)
TWO_FILE_BREAK_LINE = 5    # int x = 3;  in p.c
TWO_FILE_TARGET_LINE = 5   # r = r * 2;  in b.c
TWO_FILE_TARGET_TEXT = "r = r * 2;"


def _marked_text(w):
    """The text of the screen row carrying the current-line mark, or None.

    A window opened over the first one sits a row lower, so its marked
    line number cannot be read from the screen row; its text can."""
    rows = w.rows_with_bg(CURRENT_LINE_BG)
    if not rows:
        return None
    return w.display(drain=0)[rows[0]]


# @scenario SC-debug-05
def test_go_to_cursor_in_another_source_file_stops_there(tmp_path):
    """Stopped in p.c, the user opens b.c and puts the cursor inside bb:
    Go to cursor stops on that line of b.c.  The bare line number was read
    in p.c, the file the program was stopped in."""
    _need("gcc", "gdb")
    (tmp_path / "b.c").write_text(TWO_FILE_SECOND_C)
    with WpeSession(str(tmp_path), TWO_FILE_MAIN_C, filename="p.c",
                    wait=1.5) as w:
        _move_down(w, TWO_FILE_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == TWO_FILE_BREAK_LINE,
                         START_TIMEOUT_S), \
            "Ctrl-G R should stop on line %d of p.c:\n%s" \
            % (TWO_FILE_BREAK_LINE, _screen(w))
        w.key(F3, delay=1.2)
        w.key("b.c", delay=0.6)
        w.key("\r", delay=1.2)
        _move_down(w, TWO_FILE_TARGET_LINE - 1)
        w.key(CTRL_G, "u", delay=0.3)
        assert _wait_for(w, lambda: TWO_FILE_TARGET_TEXT
                         in (_marked_text(w) or ""), STEP_TIMEOUT_S), \
            "Go to cursor should stop on line %d of b.c (mark on %r):\n%s" \
            % (TWO_FILE_TARGET_LINE, _marked_text(w), _screen(w))
        assert _children(w, "gdb"), "gdb should still run the session"


# A function that sleeps for ten minutes before it returns, so Finish and
# Go to cursor leave the program running, not stopped.
LONG_LEAF_C_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "static int leaf(int v)\n"
    "{\n"
    "  int r = v * 2;\n"
    "  sleep(600);\n"
    "  return r;\n"
    "}\n"
    "int main(void)\n"
    "{\n"
    "  int x = leaf(2);\n"
    "  printf(\"%d\\n\", x);\n"
    "  return 0;\n"
    "}\n"
)
LONG_LEAF_FIRST_LINE = 5   # int r = v * 2;  inside leaf
LONG_LEAF_AFTER_LINE = 7   # return r;  past the sleep


# @scenario SC-debug-18
def test_debug_quit_during_a_long_finish_under_gdb(tmp_path):
    """Stopped inside a function that sleeps, Finish (Ctrl-G F) lets the
    program run; Debug > Quit from the menu then ends gdb and the editor
    stays.  Finish used to wait for the stop in a loop that read no key."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), LONG_LEAF_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on(w, LONG_LEAF_FIRST_LINE)
        w.key(CTRL_G, "f", delay=0.5)
        w._drain(RUNNING_SETTLE_S)   # asleep for ten minutes (drain scales)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Debug > Quit should end gdb during Finish:\n%s" % _screen(w)
        assert w.alive(), "the editor should still run"


# @scenario SC-debug-18
def test_keys_are_refused_during_a_long_finish_under_gdb(tmp_path):
    """While Finish runs the program, F8 is refused with the line that says
    the program is running, and gdb keeps the session."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), LONG_LEAF_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on(w, LONG_LEAF_FIRST_LINE)
        w.key(CTRL_G, "f", delay=0.5)
        w._drain(RUNNING_SETTLE_S)
        w.key(F8, delay=0.5)
        assert "The program is running" in _screen(w), \
            "F8 should say the program is running:\n%s" % _screen(w)
        assert _children(w, "gdb"), "gdb should still run the session"


# @scenario SC-debug-05
def test_debug_quit_during_a_long_go_to_cursor_under_gdb(tmp_path):
    """Go to cursor over a function that sleeps lets the program run;
    Debug > Quit from the menu ends gdb and the editor stays.  The run
    used to wait for the stop in a loop that read no key."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), LONG_LEAF_C_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on(w, LONG_LEAF_FIRST_LINE)
        _move_down(w, LONG_LEAF_AFTER_LINE - LONG_LEAF_FIRST_LINE)
        w.key(CTRL_G, "u", delay=0.5)
        w._drain(RUNNING_SETTLE_S)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "gdb"), STEP_TIMEOUT_S), \
            "Debug > Quit should end gdb during Go to cursor:\n%s" \
            % _screen(w)
        assert w.alive(), "the editor should still run"


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
    """jdb names no exit code: the plain line.  The program's output is in
    Messages already, so the line names no other place to look."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), GOTO_JAVA, filename="P.java", wait=1.5) as w:
        _ends_with_one_line(w, JAVA_SQ_LINE, "Program exited.",
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


# A Java program that sleeps eight seconds between two statements, longer
# than the five seconds jdb's step commands used to wait for a stop.
# Line numbers are 1-based source lines of JDB_SLOW_PROG.
JDB_SLOW_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int x = 1;\n"
    "        Thread.sleep(8000);\n"
    "        int y = x + 1;\n"
    "        System.out.println(y);\n"
    "    }\n"
    "}\n"
)
JDB_SLOW_FIRST_LINE = 3     # int x = 1;
JDB_SLOW_SLEEP_LINE = 4     # Thread.sleep(8000);
JDB_SLOW_AFTER_LINE = 5     # int y = x + 1;  after the sleep
JDB_SLOW_WAIT_S = 25        # the sleep, the stop and the slow machine


# @scenario SC-debug-05
def test_jdb_continue_to_a_breakpoint_far_away_marks_it(tmp_path):
    """Stopped at the first breakpoint, Ctrl-G R runs the program through
    an eight-second sleep to the second breakpoint, and the stop there is
    marked."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_SLOW_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_SLOW_FIRST_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        _move_down(w, JDB_SLOW_AFTER_LINE - JDB_SLOW_FIRST_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_SLOW_FIRST_LINE,
                         JDB_START_TIMEOUT_S), \
            "jdb should stop on line %d:\n%s" \
            % (JDB_SLOW_FIRST_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_SLOW_AFTER_LINE,
                         JDB_SLOW_WAIT_S), \
            "the run should stop on line %d (mark on %s):\n%s" \
            % (JDB_SLOW_AFTER_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-05
def test_jdb_f8_over_a_line_that_runs_long_marks_the_next_line(tmp_path):
    """F8 on a line that sleeps eight seconds marks the line after it once
    the sleep ends."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_SLOW_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_SLOW_SLEEP_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_SLOW_SLEEP_LINE,
                         JDB_START_TIMEOUT_S), \
            "jdb should stop on line %d:\n%s" \
            % (JDB_SLOW_SLEEP_LINE, _screen(w))
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_SLOW_AFTER_LINE,
                         JDB_SLOW_WAIT_S), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (JDB_SLOW_AFTER_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-36
def test_jdb_keys_reach_the_editor_while_the_program_runs(tmp_path):
    """Ctrl-G K while the Java program sleeps says so on the status row and
    opens no Stack window; jdb is asked nothing until the program stops."""
    _need("jdb", "javac")
    spec = SLEEPING_PROGS["jdb"]
    with WpeSession(str(tmp_path), spec["prog"], filename=spec["file"],
                    wait=1.5) as w:
        _move_down(w, spec["line"] - 1)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == spec["line"],
                         spec["start_s"]), \
            "jdb should stop on line %d:\n%s" % (spec["line"], _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        w._drain(RUNNING_SETTLE_S)   # asleep for ten minutes (drain scales)
        w.key(CTRL_G, "k", delay=0.8)
        assert RUNNING_REFUSAL in _screen(w), \
            "Ctrl-G K should be refused with a line on the status row:\n%s" \
            % _screen(w)
        assert " Stack " not in _screen(w), \
            "no Stack window should open:\n%s" % _screen(w)


def _start_jdb_session_on(w, line):
    """A breakpoint on @line, Ctrl-G R, and wait for jdb's first stop
    there."""
    _move_down(w, line - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == line,
                     JDB_START_TIMEOUT_S), \
        "jdb should stop on line %d (mark on %s):\n%s" \
        % (line, _mark_line(w), _screen(w))


# A method that takes eight seconds, called from main.
JDB_SLOW_CALL_PROG = (
    "public class P {\n"
    "    static int slow(int v) throws Exception {\n"
    "        Thread.sleep(8000);\n"
    "        return v * v;\n"
    "    }\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int y = slow(3);\n"
    "        System.out.println(y);\n"
    "    }\n"
    "}\n"
)
JDB_SLOW_BODY_LINE = 3        # Thread.sleep(8000);
JDB_SLOW_CALLER_LINE = 7      # int y = slow(3);


# @scenario SC-debug-34
def test_jdb_finish_over_a_long_call_marks_the_caller(tmp_path):
    """Ctrl-G F from inside a method that runs eight seconds more marks the
    caller's line when it returns, and the editor keeps its keys meanwhile:
    "step up" is not abandoned because the call takes a while."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_SLOW_CALL_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_session_on(w, JDB_SLOW_BODY_LINE)
        w.key(CTRL_G, "f", delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_SLOW_CALLER_LINE,
                         JDB_SLOW_WAIT_S), \
            "Ctrl-G F should mark line %d (mark on %s):\n%s" \
            % (JDB_SLOW_CALLER_LINE, _mark_line(w), _screen(w))


JDB_GOTO_CURSOR_LINE = 6      # System.out.println(y);  after the sleep


# @scenario SC-debug-06
def test_jdb_go_to_cursor_over_a_long_line_keeps_the_editors_keys(tmp_path):
    """F4 to a line past an eight second line: the editor refuses debugger
    keys with the running line meanwhile, then the cursor's line is marked
    and Go to cursor's own breakpoint is gone."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_SLOW_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_session_on(w, JDB_SLOW_FIRST_LINE)
        _go_to_line(w, JDB_GOTO_CURSOR_LINE)
        assert _wait_for(w, lambda: "F4 Go to cursor" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "the bar should offer F4 Go to cursor:\n%s" % _screen(w)
        w.key(F4, delay=0.5)
        _says_the_program_is_running(w)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_GOTO_CURSOR_LINE,
                         JDB_SLOW_WAIT_S), \
            "F4 should mark line %d (mark on %s):\n%s" \
            % (JDB_GOTO_CURSOR_LINE, _mark_line(w), _screen(w))
        assert JDB_GOTO_CURSOR_LINE not in _breakpoint_lines(w), \
            "Go to cursor's breakpoint should be gone at the stop:\n%s" \
            % _screen(w)


# A class whose static initialiser takes twelve seconds, longer than the ten
# the editor used to give the VM to reach its first breakpoint.
JDB_SLOW_START_PROG = (
    "public class P {\n"
    "    static {\n"
    "        try { Thread.sleep(12000); } catch (InterruptedException e) { }\n"
    "    }\n"
    "    public static void main(String[] a) {\n"
    "        int x = 3;\n"
    "        System.out.println(x);\n"
    "    }\n"
    "}\n"
)
JDB_SLOW_START_LINE = 6       # int x = 3;
JDB_SLOW_START_WAIT_S = 45    # javac, twelve seconds, the VM


# @scenario SC-debug-05
def test_jdb_first_run_reaches_a_breakpoint_after_a_slow_start(tmp_path):
    """The first Run of a program whose VM takes twelve seconds to reach
    main still stops on the breakpoint there: the start has no deadline,
    and jdb is still the session's debugger."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_SLOW_START_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_SLOW_START_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_SLOW_START_LINE,
                         JDB_SLOW_START_WAIT_S), \
            "the run should stop on line %d (mark on %s):\n%s" \
            % (JDB_SLOW_START_LINE, _mark_line(w), _screen(w))
        assert _children(w, "jdb"), "jdb should still run the session"


JDB_CURSOR_LINE = 8           # System.out.println(...) of JAVA_PROG


# @scenario SC-debug-05
def test_jdb_go_to_cursor_as_the_first_command_stops_at_the_cursor(tmp_path):
    """Borland's F4 before any session starts jdb and runs on to the
    cursor's line, as it does under gdb, and the session goes on."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_CURSOR_LINE - 1)
        w.key(CTRL_G, "u", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_CURSOR_LINE,
                         JDB_START_TIMEOUT_S), \
            "Go to cursor should start the session and stop on line %d " \
            "(mark on %s):\n%s" \
            % (JDB_CURSOR_LINE, _mark_line(w), _screen(w))
        assert _children(w, "jdb"), "jdb should still run the session"
        assert not _breakpoint_lines(w), \
            "no breakpoint should be left in the margin:\n%s" % _screen(w)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: not _children(w, "jdb"), STEP_TIMEOUT_S), \
            "a continue should run to the program's end, not stop again " \
            "at main or the cursor:\n%s" % _screen(w)


# The Java program has a terminal of its own, so the status row offers
# typing its input while it runs, as under gdb.
JDB_RUNNING_LINE = "type its input in Messages"
# A program that reads one line from its input and prints it back.
JDB_READ_PROG = (
    "import java.io.*;\n"
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        System.out.println(\"READY\");\n"
    "        BufferedReader in = new BufferedReader(\n"
    "            new InputStreamReader(System.in));\n"
    "        System.out.println(\"GOT:\" + in.readLine());\n"
    "        int x = 3;\n"
    "    }\n"
    "}\n"
)
JDB_READ_BREAK_LINE = 8       # int x = 3;  after the line is read
JDB_TYPED_LINE = "hi there"


def _messages_hold(w, text):
    """True when a line of the Messages window on screen holds @text."""
    return any(text in line for line in _messages_lines(w))


def _start_jdb_reading_program(w):
    """A breakpoint after the read, then Ctrl-G R: return once the Java
    program printed READY in Messages and waits on its input."""
    _move_down(w, JDB_READ_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _messages_hold(w, "READY"),
                     JDB_START_TIMEOUT_S), \
        "the program should run and print READY in Messages:\n%s" \
        % _screen(w)


# @scenario SC-debug-35
def test_jdb_text_typed_while_the_program_runs_reaches_it(tmp_path):
    """While the Java program waits for a line, Ctrl-G K's status row
    offers typing its input, the line typed and Enter reach the program,
    which prints it back in Messages and stops on the next breakpoint."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_READ_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_reading_program(w)
        w.key(CTRL_G, "k", delay=0.8)
        assert JDB_RUNNING_LINE in _screen(w), \
            "Ctrl-G K should say %r under jdb:\n%s" \
            % (JDB_RUNNING_LINE, _screen(w))
        w.key(JDB_TYPED_LINE, delay=0.4)
        w.key("\r", delay=0.3)
        assert _wait_for(w, lambda: _messages_hold(w, "GOT:" + JDB_TYPED_LINE),
                         STEP_TIMEOUT_S), \
            "the program should read what was typed:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_READ_BREAK_LINE,
                         STEP_TIMEOUT_S), \
            "the run should stop on line %d (mark on %s):\n%s" \
            % (JDB_READ_BREAK_LINE, _mark_line(w), _screen(w))
        w.save()
        assert JDB_TYPED_LINE not in w.text(), \
            "the typed line belongs to the program, not the file:\n%s" \
            % w.text()


# @scenario SC-debug-18
@pytest.mark.parametrize("stage", ["running", "stopped"])
def test_debug_quit_under_jdb_leaves_no_java_process_behind(tmp_path, stage):
    """Debug > Quit ends the whole tree jdb started, the VM it launched
    included, whether the program runs or waits at a stop: no Java process
    outlives the session."""
    _need("jdb", "javac")
    spec = SLEEPING_PROGS["jdb"]
    with WpeSession(str(tmp_path), spec["prog"], filename=spec["file"],
                    wait=1.5) as w:
        _start_jdb_session_on(w, spec["line"])
        if stage == "running":
            w.key(CTRL_G, "r", delay=0.5)
            w._drain(RUNNING_SETTLE_S)
        tree = procinfo.descendants(w.proc.pid)
        assert len(tree) >= 2, \
            "jdb and the VM it launched should be running: %s" % tree
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: procinfo.all_ended(tree),
                         STEP_TIMEOUT_S), \
            "no process of the session should be left (alive: %s):\n%s" \
            % ([p for p in tree if not procinfo.all_ended([p])], _screen(w))


# A program that prints without end, so the VM is busy writing to jdb's pipe
# when Quit comes.
JDB_FLOOD_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int i = 0;\n"
    "        while (true) {\n"
    "            System.out.println(\"flood \" + i++);\n"
    "        }\n"
    "    }\n"
    "}\n"
)
JDB_FLOOD_LINE = 3            # int i = 0;
JDB_FLOOD_SETTLE_S = 2        # let the flood get going


# @scenario SC-debug-18
def test_debug_quit_while_the_java_program_floods_leaves_no_process(tmp_path):
    """Debug > Quit while the Java program prints without end ends the whole
    tree jdb started, the VM included: a flood must not keep the VM alive
    after jdb is gone."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_FLOOD_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_session_on(w, JDB_FLOOD_LINE)
        w.key(CTRL_G, "r", delay=0.5)
        w._drain(JDB_FLOOD_SETTLE_S)
        tree = procinfo.descendants(w.proc.pid)
        assert len(tree) >= 2, \
            "jdb and the VM it launched should be running: %s" % tree
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: procinfo.all_ended(tree),
                         STEP_TIMEOUT_S), \
            "no process of the session should be left (alive: %s):\n%s" \
            % ([p for p in tree if not procinfo.all_ended([p])], _screen(w))


# An uncaught exception in a method main calls: jdb stops on the line that
# threw, as it does at a breakpoint.
JDB_THROW_PROG = (
    "public class P {\n"
    "    static void boom() {\n"
    "        throw new IllegalStateException(\"boom\");\n"
    "    }\n"
    "    public static void main(String[] a) {\n"
    "        int x = 1;\n"
    "        boom();\n"
    "        System.out.println(x);\n"
    "    }\n"
    "}\n"
)
JDB_THROW_LINE = 3            # throw new IllegalStateException(...)
JDB_THROW_BEFORE_LINE = 6     # int x = 1;
JDB_THROW_CALL_LINE = 7       # boom();
JDB_THROW_NEVER_LINE = 8      # a breakpoint the exception comes before
JDB_THROW_SAYS = "Uncaught exception: java.lang.IllegalStateException"


# @scenario SC-debug-05
@pytest.mark.parametrize("how", ["first_run", "continue", "step"])
def test_jdb_uncaught_exception_marks_the_line_that_threw(tmp_path, how):
    """An uncaught exception is a stop: the line that threw is marked, from
    the first Run, from a continue and from F8 over the call, and the
    editor does not stay "running"."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_THROW_PROG, filename="P.java",
                    wait=1.5) as w:
        if how == "first_run":
            _move_down(w, JDB_THROW_NEVER_LINE - 1)
            w.key(CTRL_G, "b", delay=0.4)
            w.key(CTRL_G, "r", delay=1.0)
        elif how == "continue":
            _start_jdb_session_on(w, JDB_THROW_BEFORE_LINE)
            w.key(CTRL_G, "r", delay=0.5)
        else:
            _start_jdb_session_on(w, JDB_THROW_CALL_LINE)
            w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_THROW_LINE,
                         JDB_START_TIMEOUT_S), \
            "the uncaught exception should mark line %d (mark on %s):\n%s" \
            % (JDB_THROW_LINE, _mark_line(w), _screen(w))
        assert _status_row_says(w, JDB_THROW_SAYS, STEP_TIMEOUT_S), \
            "the status row should name the exception:\n%s" % _screen(w)
        w.key(CTRL_G, "k", delay=0.8)
        assert RUNNING_REFUSAL not in _screen(w), \
            "the session should not be 'running' after the stop:\n%s" \
            % _screen(w)


# The program prints jdb's launch-failure words between two breakpoints.
JDB_UNABLE_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 1;\n"
    "        System.out.println(\"Unable to open config\");\n"
    "        int y = x + 1;\n"
    "    }\n"
    "}\n"
)
JDB_UNABLE_FIRST_LINE = 3
JDB_UNABLE_SECOND_LINE = 5


# @scenario SC-debug-05
def test_jdb_program_output_saying_unable_does_not_end_the_session(tmp_path):
    """A line of the program's own output that says "Unable to ..." is
    output, not a failed start: the next breakpoint is still marked."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_UNABLE_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_UNABLE_FIRST_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        _move_down(w, JDB_UNABLE_SECOND_LINE - JDB_UNABLE_FIRST_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_UNABLE_FIRST_LINE,
                         JDB_START_TIMEOUT_S), \
            "jdb should stop on line %d:\n%s" \
            % (JDB_UNABLE_FIRST_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_UNABLE_SECOND_LINE,
                         STEP_TIMEOUT_S), \
            "the second breakpoint should be marked (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        assert _children(w, "jdb"), "jdb should still run the session"


# The program prints the words of jdb's stop lines and something that looks
# like its prompt, then waits for a line on its input (which the test gives
# when it chooses, not after a fixed time) before the next breakpoint.
JDB_FAKE_STOP_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int x = 1;\n"
    "        System.out.println(\"[app] Exception occurred: handled\");\n"
    "        System.out.println(\"Breakpoint hit: not jdb's\");\n"
    "        System.out.print(\"main[1] \");\n"
    "        System.out.flush();\n"
    "        new java.io.BufferedReader(new java.io.InputStreamReader("
    "System.in)).readLine();\n"
    "        int y = x + 1;\n"
    "    }\n"
    "}\n"
)
JDB_FAKE_STOP_FIRST_LINE = 3
JDB_FAKE_STOP_SECOND_LINE = 9     # after the read
JDB_FAKE_STOP_PRINTED = "Breakpoint hit: not jdb's"
JDB_FAKE_STOP_ANSWER = "go"       # the line that lets the program on


# @scenario SC-debug-05
def test_jdb_program_output_like_a_stop_is_not_taken_for_one(tmp_path):
    """Program output with jdb's stop words and a prompt-like "main[1] " is
    output: while the program waits for its input the session is still running
    (Ctrl-G K is refused), and the next breakpoint is marked once the
    program is given its line."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_FAKE_STOP_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_FAKE_STOP_FIRST_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        _move_down(w, JDB_FAKE_STOP_SECOND_LINE - JDB_FAKE_STOP_FIRST_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_FAKE_STOP_FIRST_LINE,
                         JDB_START_TIMEOUT_S), \
            "jdb should stop on line %d:\n%s" \
            % (JDB_FAKE_STOP_FIRST_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _messages_hold(w, JDB_FAKE_STOP_PRINTED),
                         STEP_TIMEOUT_S), \
            "the program's stop-like lines should be in Messages:\n%s" \
            % _screen(w)
        w.key(CTRL_G, "k", delay=0.8)
        assert RUNNING_REFUSAL in _screen(w), \
            "the program still runs: Ctrl-G K should be refused:\n%s" \
            % _screen(w)
        w.key(JDB_FAKE_STOP_ANSWER, delay=0.4)
        w.key("\r", delay=0.3)
        assert _wait_for(w,
                         lambda: _mark_line(w) == JDB_FAKE_STOP_SECOND_LINE,
                         STEP_TIMEOUT_S), \
            "the breakpoint after the sleep should be marked (mark on %s):" \
            "\n%s" % (_mark_line(w), _screen(w))


# A line the program never reaches.
JDB_NEVER_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 3;\n"
    "        if (x > 5) {\n"
    "            x = 0;\n"
    "        }\n"
    "        System.out.println(x);\n"
    "    }\n"
    "}\n"
)
JDB_NEVER_LINE = 5            # x = 0;


# @scenario SC-debug-05
def test_jdb_go_to_cursor_as_the_first_command_onto_a_line_never_reached(
        tmp_path):
    """Go to cursor before any session onto a line the program never
    reaches runs it to its end: Messages says so, jdb is gone and nothing
    of the session is left running."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_NEVER_PROG, filename="P.java",
                    wait=1.5) as w:
        _move_down(w, JDB_NEVER_LINE - 1)
        w.key(CTRL_G, "u", delay=1.0)
        assert _wait_for(w, lambda: "Program exited" in _screen(w),
                         JDB_START_TIMEOUT_S), \
            "the session should end saying the program exited:\n%s" \
            % _screen(w)
        assert _wait_for(w, lambda: not procinfo.descendants(w.proc.pid),
                         STEP_TIMEOUT_S), \
            "nothing of the session should be left: %s\n%s" \
            % (procinfo.descendants(w.proc.pid), _screen(w))


# ---- The Java program on a terminal of its own ---------------------------
#
# jdb waits for the program's VM on a loopback port and the editor starts
# the VM itself on the session's terminal, so what the program prints never
# mixes with what jdb says: it reaches Messages as it prints, and what the
# user types while it runs reaches the program, as under gdb.

# The program prints a line, then text without a newline, then stops.
JDB_EARLY_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 1;\n"
    "        System.out.println(\"EARLY OUT \" + x);\n"
    "        System.out.print(\"NO NEWLINE\");\n"
    "        int y = x + 1;\n"
    "        System.out.println(y);\n"
    "    }\n"
    "}\n"
)
JDB_EARLY_FIRST_LINE = 3      # int x = 1;  before both prints
JDB_EARLY_STOP_LINE = 6       # int y = x + 1;  after both prints


def _jdb_breakpoints_and_run(w, lines):
    """A breakpoint on each of @lines (ascending), then Ctrl-G R."""
    at = 1
    for line in lines:
        _move_down(w, line - at)
        w.key(CTRL_G, "b", delay=0.4)
        at = line
    w.key(CTRL_G, "r", delay=1.0)


# @scenario SC-debug-11
@pytest.mark.parametrize("how", ["first_run", "continue"])
def test_jdb_output_printed_before_a_breakpoint_reaches_messages(tmp_path,
                                                                 how):
    """What the Java program printed before it stopped, the text that has no
    newline yet included, is in Messages at the stop: from the first Run and
    from a continue."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_EARLY_PROG, filename="P.java",
                    wait=1.5) as w:
        if how == "continue":
            _jdb_breakpoints_and_run(w, [JDB_EARLY_FIRST_LINE,
                                         JDB_EARLY_STOP_LINE])
            assert _wait_for(w, lambda: _mark_line(w) == JDB_EARLY_FIRST_LINE,
                             JDB_START_TIMEOUT_S), \
                "jdb should stop on line %d:\n%s" \
                % (JDB_EARLY_FIRST_LINE, _screen(w))
            w.key(CTRL_G, "r", delay=0.5)
        else:
            _jdb_breakpoints_and_run(w, [JDB_EARLY_STOP_LINE])
        assert _wait_for(w, lambda: _mark_line(w) == JDB_EARLY_STOP_LINE,
                         JDB_START_TIMEOUT_S), \
            "jdb should stop on line %d (mark on %s):\n%s" \
            % (JDB_EARLY_STOP_LINE, _mark_line(w), _screen(w))
        assert _wait_for(w, lambda: _messages_hold(w, "EARLY OUT 1") and
                         _messages_hold(w, "NO NEWLINE"), STEP_TIMEOUT_S), \
            "Messages should show what the program printed:\n%s" % _screen(w)


# One source line prints two thousand lines.
JDB_LOOP_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int n = 3;\n"
    "        for (int i = 0; i < 2000; i++) "
    "System.out.println(\"LOOP \" + i);\n"
    "        int y = n + 1;\n"
    "    }\n"
    "}\n"
)
JDB_LOOP_LINE = 4             # the printing loop
JDB_LOOP_NEXT_LINE = 5        # int y = n + 1;
JDB_LOOP_LAST = "LOOP 1999"


# @scenario SC-debug-11
def test_jdb_f8_over_a_println_loop_marks_the_next_line_and_shows_the_output(
        tmp_path):
    """F8 over a line that prints two thousand lines marks the next line,
    and Messages ends with the last line printed."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_LOOP_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_session_on(w, JDB_LOOP_LINE)
        w.key(F8, delay=0.3)
        assert _wait_for(w, lambda: _mark_line(w) == JDB_LOOP_NEXT_LINE,
                         JDB_SLOW_WAIT_S), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (JDB_LOOP_NEXT_LINE, _mark_line(w), _screen(w))
        assert _wait_for(w, lambda: _messages_hold(w, JDB_LOOP_LAST),
                         STEP_TIMEOUT_S), \
            "Messages should end with %r:\n%s" % (JDB_LOOP_LAST, _screen(w))


# A jdb that runs the real one, in a directory whose java is a stand-in:
# the editor starts the java it finds next to jdb.
FAKE_JDB = "#!/bin/sh\nexec '%s' \"$@\"\n"
FAKE_JAVA_SLEEPS = "#!/bin/sh\nexec sleep 600\n"
FAKE_JAVA_FAILS = "#!/bin/sh\necho 'stand-in VM refuses to start' >&2\nexit 3\n"
FAKE_JAVA_FAILS_SAYS = "stand-in VM refuses to start"
FAKE_JAVA_GONE_LINE = "java ended before jdb connected (exit status 3)"
WAITING_FOR_JDB = "jdb starting the Java VM"


def _fake_jdk_env(tmp_path, java_script):
    """The environment of a session whose jdb finds @java_script as its
    java, with a start deadline no test reaches."""
    bindir = tmp_path / "fakejdk"
    bindir.mkdir()
    _write_script(bindir / "jdb",
                  FAKE_JDB % shutil.which("jdb"))
    _write_script(bindir / "java", java_script)
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_DEBUG_START_TIMEOUT": str(LONG_START_TIMEOUT_S)}


def _quit_while_the_vm_starts(w, key):
    """The start waits for a VM that never connects: end it with @key."""
    assert _wait_for(w, lambda: WAITING_FOR_JDB in _screen(w) and
                     _children(w, "sleep"), JDB_START_TIMEOUT_S), \
        "the start should wait for the VM:\n%s" % _screen(w)
    tree = procinfo.descendants(w.proc.pid)
    if key == "esc":
        w.key(ESC, delay=0.5)
    else:
        w.menu(ALT.DEBUG, "q", delay=0.5)
    return tree


def _quit_while_the_program_reads(w):
    """The program waits on its input: end the session with Debug > Quit."""
    _start_jdb_reading_program(w)
    tree = procinfo.descendants(w.proc.pid)
    w.menu(ALT.DEBUG, "q", delay=0.5)
    return tree


# @scenario SC-debug-18
@pytest.mark.parametrize("how", ["starting", "starting_esc",
                                 "blocked_on_stdin"])
def test_debug_quit_under_jdb_leaves_nothing(tmp_path, how):
    """Debug > Quit, or Esc, while jdb waits for the VM, and Debug > Quit
    while the program waits for its input, end jdb and the VM: no process
    of the session is left."""
    _need("jdb", "javac")
    env = None
    prog = JDB_READ_PROG
    if how != "blocked_on_stdin":
        env = _fake_jdk_env(tmp_path, FAKE_JAVA_SLEEPS)
        prog = JAVA_PROG
    with WpeSession(str(tmp_path), prog, filename="P.java", wait=1.5,
                    env_extra=env) as w:
        if how == "blocked_on_stdin":
            tree = _quit_while_the_program_reads(w)
        elif how == "starting_esc":
            _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
            tree = _quit_while_the_vm_starts(w, "esc")
        else:
            _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
            tree = _quit_while_the_vm_starts(w, "quit")
        assert len(tree) >= 2, "jdb and the VM should be running: %s" % tree
        assert _wait_for(w, lambda: procinfo.all_ended(tree),
                         STEP_TIMEOUT_S), \
            "no process of the session should be left (alive: %s):\n%s" \
            % ([p for p in tree if not procinfo.all_ended([p])], _screen(w))
        assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)


# @scenario SC-debug-17
def test_jdb_vm_that_ends_before_connecting_says_why(tmp_path):
    """A VM that ends before it reaches jdb ends the start: Messages shows
    what it said and its exit status, and jdb is not left listening."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5,
                    env_extra=_fake_jdk_env(tmp_path, FAKE_JAVA_FAILS)) as w:
        _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
        assert _wait_for(w, lambda: _messages_hold(w, FAKE_JAVA_GONE_LINE),
                         JDB_START_TIMEOUT_S), \
            "Messages should say the VM ended:\n%s" % _screen(w)
        assert _messages_hold(w, FAKE_JAVA_FAILS_SAYS), \
            "Messages should show what the VM said:\n%s" % _screen(w)
        assert _wait_for(w, lambda: not procinfo.descendants(w.proc.pid),
                         STEP_TIMEOUT_S), \
            "nothing of the session should be left: %s\n%s" \
            % (procinfo.descendants(w.proc.pid), _screen(w))


# A jdb that is a script: what it prints first, then it waits.
STAND_IN_JDB_LISTEN = "Listening at address: localhost:%s"
STAND_IN_JDB_NO_PORT = STAND_IN_JDB_LISTEN % "99999"
STAND_IN_JDB_NO_PORT_NAMED = "jdb named no port to listen on"
WAITING_ROW = "(Esc cancels)"
STAND_IN_JDB_DIED_LINE = "jdb ended unexpectedly (exit status 5)"
VM_REFUSED_CONNECT = "java ended before jdb connected"
VM_CANNOT_ENTER = "cannot enter"
VM_PERMISSION_DENIED = "Permission denied"
WIDE_COLS = 220           # a whole temporary path fits on one Messages line


def _stand_in_jdb_env(tmp_path, body):
    """The environment of a session whose jdb is the shell script @body."""
    bindir = tmp_path / "standinjdb"
    bindir.mkdir()
    _write_script(bindir / "jdb", "#!/bin/sh\n" + body)
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", "")),
            "XWPE_DEBUG_START_TIMEOUT": str(LONG_START_TIMEOUT_S)}


def _unused_port():
    """A loopback port nothing listens on."""
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _message_index(w, text):
    """The index in Messages of the first line holding @text, or -1."""
    for i, line in enumerate(_messages_lines(w)):
        if text in line:
            return i
    return -1


# @scenario SC-debug-17
def test_jdb_listen_line_with_no_port_keeps_jdbs_line_and_drops_the_wait(
        tmp_path):
    """jdb's listen line names port 99999: Messages shows jdb's line and the
    line that says no port was named, and the waiting row is gone."""
    _need("javac")
    env = _stand_in_jdb_env(tmp_path,
                            "echo '%s'\nsleep 600\n" % STAND_IN_JDB_NO_PORT)
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5,
                    env_extra=env) as w:
        _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
        assert _wait_for(w, lambda: _messages_hold(w, STAND_IN_JDB_NO_PORT_NAMED),
                         JDB_START_TIMEOUT_S), \
            "Messages should say no port was named:\n%s" % _screen(w)
        assert _messages_hold(w, STAND_IN_JDB_NO_PORT), \
            "jdb's own line should stay in Messages:\n%s" % _screen(w)
        assert not _messages_hold(w, WAITING_ROW), \
            "the waiting row should be gone:\n%s" % _screen(w)
        assert _message_index(w, STAND_IN_JDB_NO_PORT) < \
            _message_index(w, STAND_IN_JDB_NO_PORT_NAMED), \
            "jdb's line comes before the reason:\n%s" % _screen(w)


# @scenario SC-debug-17
def test_jdb_listen_line_ending_in_cr_lf_still_names_its_port(tmp_path):
    """A listen line that ends "\\r\\n" has a valid port: the VM is started
    for it (here it finds nothing listening and says so), not refused as a
    line that names no port."""
    _need("javac")
    line = STAND_IN_JDB_LISTEN % _unused_port()
    env = _stand_in_jdb_env(tmp_path,
                            "printf '%s\\r\\n'\nsleep 600\n" % line)
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5,
                    env_extra=env) as w:
        _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
        assert _wait_for(w, lambda: _messages_hold(w, VM_REFUSED_CONNECT),
                         JDB_START_TIMEOUT_S), \
            "the VM should have been started for the port:\n%s" % _screen(w)
        assert not _messages_hold(w, STAND_IN_JDB_NO_PORT_NAMED), \
            "the port was valid:\n%s" % _screen(w)


# @scenario SC-debug-17
def test_jdb_that_dies_while_it_starts_says_its_exit_status(tmp_path):
    """jdb ends with status 5 before the VM reaches it: Messages says so, as
    it does for a jdb that dies while the program runs."""
    _need("javac")
    env = _stand_in_jdb_env(tmp_path, "echo 'something odd'\nexit 5\n")
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5,
                    env_extra=env) as w:
        _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
        assert _wait_for(w, lambda: _messages_hold(w, STAND_IN_JDB_DIED_LINE),
                         JDB_START_TIMEOUT_S), \
            "Messages should say how jdb ended:\n%s" % _screen(w)


# @scenario SC-debug-17
def test_jdb_vm_that_cannot_enter_its_directory_says_why(tmp_path):
    """A VM that cannot change to the build's directory says why in
    Messages, in the same place as a java that cannot be run."""
    _need("javac")
    body = ("D='%s'\n"
            "trap 'chmod 755 \"$D\"; exit 0' TERM\n"
            "chmod 000 \"$D\"\n"
            "echo '%s'\n"
            "sleep 600 &\nwait\n"
            % (tmp_path, STAND_IN_JDB_LISTEN % _unused_port()))
    env = _stand_in_jdb_env(tmp_path, body)
    try:
        with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java",
                        wait=1.5, cols=WIDE_COLS, env_extra=env) as w:
            _jdb_breakpoints_and_run(w, [JAVA_BREAK_LINE])
            assert _wait_for(w, lambda: _messages_hold(w, VM_CANNOT_ENTER),
                             JDB_START_TIMEOUT_S), \
                "Messages should say the VM cannot enter its directory:\n%s" \
                % _screen(w)
            assert _messages_hold(w, VM_PERMISSION_DENIED), \
                "and why:\n%s" % _screen(w)
    finally:
        tmp_path.chmod(0o755)


# The program ends with a failure code.
JDB_EXIT3_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 1;\n"
    "        System.exit(3);\n"
    "    }\n"
    "}\n"
)
JDB_EXIT3_LINE = 3
JDB_EXIT3_END = "Program exited with code 3"
JDB_KILLED_END = "Program terminated with signal SIGKILL"
JDB_PLAIN_END = "Program exited."


# @scenario SC-debug-13
def test_jdb_end_names_the_exit_code_of_a_failing_program(tmp_path):
    """System.exit(3): the end line says the program exited with code 3."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_EXIT3_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_session_on(w, JDB_EXIT3_LINE)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _messages_hold(w, JDB_EXIT3_END),
                         STEP_TIMEOUT_S), \
            "Messages should say %r:\n%s" % (JDB_EXIT3_END, _screen(w))
        assert not _messages_hold(w, JDB_PLAIN_END), \
            "the plain line would hide the code:\n%s" % _screen(w)


# @scenario SC-debug-13
def test_jdb_end_names_the_signal_that_killed_the_program(tmp_path):
    """The VM killed by SIGKILL (its exact PID, a child of the editor): the
    end line names the signal."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_READ_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_reading_program(w)
        vms = _children(w, "java")
        assert len(vms) == 1, "the editor's one java child: %s" % vms
        os.kill(vms[0], signal.SIGKILL)
        assert _wait_for(w, lambda: _messages_hold(w, JDB_KILLED_END),
                         STEP_TIMEOUT_S), \
            "Messages should say %r:\n%s" % (JDB_KILLED_END, _screen(w))
        assert not _messages_hold(w, JDB_PLAIN_END), \
            "the plain line would hide the signal:\n%s" % _screen(w)


# A java that runs the real VM and then lingers, as a wrapper script that
# cleans up after the VM does: the editor's child outlives the VM.
FAKE_JAVA_OUTLIVES = "#!/bin/sh\n'%s' \"$@\"\nsleep 600\n"


def _jdk_java():
    """The java next to the real jdb, the one the editor would start."""
    return os.path.join(os.path.dirname(os.path.realpath(
        shutil.which("jdb"))), "java")


# Time for an end line that should not come yet to show if it were wrong.
JDB_NO_END_YET_S = 2


def _any_end_line(w):
    """True when Messages holds an end line of the program."""
    return _messages_hold(w, JDB_PLAIN_END) or \
        _messages_hold(w, JDB_EXIT3_END)


def _run_jdb_to_the_end_of_the_vm(w):
    """Stop on JDB_EXIT3_LINE, then Ctrl-G R: return the processes of the
    session once jdb has said the program ended and gone away."""
    _start_jdb_session_on(w, JDB_EXIT3_LINE)
    tree = procinfo.descendants(w.proc.pid)
    w.key(CTRL_G, "r", delay=0.5)
    assert _wait_for(w, lambda: not _children(w, "jdb"), STEP_TIMEOUT_S), \
        "jdb should end with the program:\n%s" % _screen(w)
    return tree


# @scenario SC-debug-13
def test_jdb_keeps_the_keys_while_a_java_wrapper_outlives_the_vm(tmp_path):
    """The VM ends while the java the editor started lives on: the session
    waits for that process without holding a key.  No end line yet; the
    editor answers; Ctrl-G K and Ctrl-G R say the program runs, and start
    nothing; Debug > Quit ends the session at once and leaves no process
    behind, the wrapper's sleep included."""
    _need("jdb", "javac")
    env = _fake_jdk_env(tmp_path, FAKE_JAVA_OUTLIVES % _jdk_java())
    with WpeSession(str(tmp_path), JDB_EXIT3_PROG, filename="P.java",
                    wait=1.5, env_extra=env) as w:
        tree = _run_jdb_to_the_end_of_the_vm(w)
        w._drain(JDB_NO_END_YET_S)
        assert not _any_end_line(w), \
            "the process the editor started still runs:\n%s" % _screen(w)
        assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)
        _says_the_program_is_running(w)
        assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)
        w.key(CTRL_G, "r", delay=0.8)
        assert RUNNING_REFUSAL in _screen(w), \
            "Ctrl-G R should say the program runs:\n%s" % _screen(w)
        assert not _children(w, "jdb"), \
            "Ctrl-G R should start no new session:\n%s" % _screen(w)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: procinfo.all_ended(tree) and
                         not procinfo.descendants(w.proc.pid),
                         STEP_TIMEOUT_S), \
            "no process of the session should be left (alive: %s)" \
            % procinfo.descendants(w.proc.pid)
        assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)


# A java wrapper that lives a little after the VM and ends with its own code.
FAKE_JAVA_ENDS_LATER = "#!/bin/sh\n'%s' \"$@\"\nsleep %d\nexit 7\n"
JDB_WRAPPER_LINGER_S = 4
JDB_WRAPPER_END = "Program exited with code 7"


# @scenario SC-debug-13
def test_jdb_end_line_waits_for_a_java_wrapper_and_gives_its_status(tmp_path):
    """The wrapper ends a few seconds after the VM with code 7: the end
    line comes then, with the wrapper's code, as a shell would report the
    command it ran; the editor took keys meanwhile."""
    _need("jdb", "javac")
    env = _fake_jdk_env(tmp_path, FAKE_JAVA_ENDS_LATER
                        % (_jdk_java(), JDB_WRAPPER_LINGER_S))
    with WpeSession(str(tmp_path), JDB_EXIT3_PROG, filename="P.java",
                    wait=1.5, env_extra=env) as w:
        _run_jdb_to_the_end_of_the_vm(w)
        assert _takes_keys(w), "the editor should take keys:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _messages_hold(w, JDB_WRAPPER_END),
                         STEP_TIMEOUT_S + JDB_WRAPPER_LINGER_S), \
            "Messages should say %r:\n%s" % (JDB_WRAPPER_END, _screen(w))
        assert _wait_for(w, lambda: not procinfo.descendants(w.proc.pid),
                         STEP_TIMEOUT_S), \
            "no process of the session should be left (alive: %s)" \
            % procinfo.descendants(w.proc.pid)


# The program would write a file on the line after its stop.
JDB_AFTER_QUIT_FILE = "ran-after-quit"
JDB_AFTER_QUIT_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int x = 1;\n"
    "        new java.io.FileOutputStream(\"%s\").close();\n"
    "    }\n"
    "}\n" % JDB_AFTER_QUIT_FILE
)
JDB_AFTER_QUIT_LINE = 3
JDB_AFTER_QUIT_SETTLE_S = 3   # long enough for the program to run on


# @scenario SC-debug-18
def test_debug_quit_at_a_jdb_stop_runs_no_more_of_the_program(tmp_path):
    """Debug > Quit at a stop ends the Java program there: the line after
    the stop never runs."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_AFTER_QUIT_PROG, filename="P.java",
                    wait=1.5) as w:
        _start_jdb_session_on(w, JDB_AFTER_QUIT_LINE)
        tree = procinfo.descendants(w.proc.pid)
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: procinfo.all_ended(tree),
                         STEP_TIMEOUT_S), \
            "no process of the session should be left (alive: %s):\n%s" \
            % ([p for p in tree if not procinfo.all_ended([p])], _screen(w))
        w._drain(JDB_AFTER_QUIT_SETTLE_S)
        assert not (tmp_path / JDB_AFTER_QUIT_FILE).exists(), \
            "the program ran on after Quit and wrote %s" % JDB_AFTER_QUIT_FILE


# P.java holds no class P: javac builds Other only, and the VM cannot find
# the class it is asked to run.
JDB_NO_MAIN_PROG = (
    "class Other {\n"
    "    public static void main(String[] a) {\n"
    "        int x = 1;\n"
    "    }\n"
    "}\n"
)
JDB_NO_MAIN_LINE = 3
# The VM's last words, still on screen above the end lines.
JDB_NO_MAIN_SAYS = "ClassNotFoundException: P"


# @scenario SC-debug-13
def test_jdb_missing_class_says_why_in_messages(tmp_path):
    """A program whose class the VM cannot find says why in Messages, in
    the VM's own words, the session ends and nothing is left running."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_NO_MAIN_PROG, filename="P.java",
                    wait=1.5) as w:
        _jdb_breakpoints_and_run(w, [JDB_NO_MAIN_LINE])
        assert _wait_for(w, lambda: _messages_hold(w, JDB_NO_MAIN_SAYS),
                         JDB_START_TIMEOUT_S), \
            "Messages should say why the program did not start:\n%s" \
            % _screen(w)
        assert _wait_for(w, lambda: not procinfo.descendants(w.proc.pid),
                         STEP_TIMEOUT_S), \
            "nothing of the session should be left: %s\n%s" \
            % (procinfo.descendants(w.proc.pid), _screen(w))


# A method below a thousand lines of comment: jdb writes its line number
# with a thousands separator, line=1,005.
JDB_FAR_PAD = 1000
JDB_FAR_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        int y = sq(3);\n"
    "    }\n"
    + "    //\n" * JDB_FAR_PAD +
    "    static int sq(int v) {\n"
    "        return v * v;\n"
    "    }\n"
    "}\n"
)
JDB_FAR_LINE = 4 + JDB_FAR_PAD + 2    # return v * v;
JDB_FAR_TEXT = "return v * v;"


# @scenario SC-debug-20
def test_jdb_breakpoint_beyond_line_999_is_marked(tmp_path):
    """A stop on line 1006 marks line 1006, not line 1."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_FAR_PROG, filename="P.java",
                    wait=1.5) as w:
        _go_to_line(w, JDB_FAR_LINE)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: JDB_FAR_TEXT in _marked_text(w),
                         JDB_START_TIMEOUT_S), \
            "the stop should mark %r:\n%s" % (JDB_FAR_TEXT, _screen(w))
        assert _status_field_line(w) == JDB_FAR_LINE, \
            "the stop should be on line %d, the field says %s:\n%s" \
            % (JDB_FAR_LINE, _status_field_line(w), _screen(w))


GERMAN_LOCALE = "de_DE.UTF-8"


def _locale_installed(name):
    """True when `locale -a` lists @name (spelled utf8 or UTF-8)."""
    try:
        out = subprocess.run(["locale", "-a"], capture_output=True,
                             text=True).stdout
    except OSError:
        return False
    wanted = name.lower().replace("-", "")
    return any(line.lower().replace("-", "") == wanted
               for line in out.split())


# @scenario SC-debug-20
def test_jdb_works_in_a_german_locale(tmp_path):
    """Under a German locale jdb is still understood: the breakpoint and the
    next F8 are marked, and the end says the program exited."""
    _need("jdb", "javac")
    if not _locale_installed(GERMAN_LOCALE):
        pytest.skip("no %s locale" % GERMAN_LOCALE)
    with WpeSession(str(tmp_path), JAVA_PROG, filename="P.java", wait=1.5,
                    env_extra={"LC_ALL": GERMAN_LOCALE,
                               "LANG": GERMAN_LOCALE}) as w:
        _start_jdb_session_on(w, JAVA_BREAK_LINE)
        assert _step(w, F8, JAVA_NEXT_LINE), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (JAVA_NEXT_LINE, _mark_line(w), _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _messages_hold(w, "Program exited"),
                         JDB_START_TIMEOUT_S), \
            "Messages should say the program exited:\n%s" % _screen(w)


# ---- Pause: Ctrl-Break, Ctrl-G I, Debug > Interrupt, the bar's entry -------
#
# The program prints "ready" and then sleeps (or loops, where the debugger has
# no sleep call).  The breakpoint is on the print; after the stop there,
# Ctrl-G R leaves the program running.  Pause then marks the line the program
# is on: for a sleeping program that is the line of the sleep call, in the
# user's code, not the library function underneath it.

CTRL_G_I = (CTRL_G, "i")          # Debug > Interrupt (pause)
PAUSED_LINE = "Paused"
PAUSE_TIMEOUT_S = 10

GDB_SLEEP_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  sleep(600);\n"
    "  return 0;\n"
    "}\n"
)
GDB_SLEEP_BREAK_LINE = 5          # printf("READY\n")
GDB_SLEEP_LINE = 7                # sleep(600)

PY_SLEEP_PROG = SLEEPING_PROGS["pdb"]["prog"]
PY_SLEEP_BREAK_LINE = 2           # print('ready')
PY_SLEEP_LINE = 3                 # time.sleep(600)

A68_PAUSE_PROG = (
    "print ((\"rea\" + \"dy\", new line));\nINT n := 0;\n"
    "WHILE n < 2000000000 DO n +:= 1 OD;\nINT x := 3;\n"
    "print ((x, new line))\n"
)
A68_PAUSE_BREAK_LINE = 2          # INT n := 0;
A68_PAUSE_LINE = 3                # WHILE n < ... DO n +:= 1 OD;


PAUSE_SETTLE_S = 3            # the program has printed and gone to sleep


def _stop_then_run_on(w, break_line, start_s, in_messages=True):
    """Breakpoint on @break_line, Ctrl-G R to the stop, Ctrl-G R to run on;
    return once the program runs and has printed "ready".  Under gdb and
    a68g the word reaches Messages while it runs (the source holds it too,
    so only Messages is looked at); pdb shows its program's text at the
    next stop, so there the program is given time to print and sleep."""
    _go_to_line(w, break_line)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == break_line, start_s), \
        "the session should stop on line %d (mark on %s):\n%s" \
        % (break_line, _mark_line(w), _screen(w))
    w.key(CTRL_G, "r", delay=0.5)
    if not in_messages:
        assert _wait_for(w, lambda: "^Brk Pause" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "the program should run:\n%s" % _screen(w)
        w._drain(PAUSE_SETTLE_S)
        return
    assert _wait_for(w, lambda: _messages_hold(w, "ready")
                     or _messages_hold(w, "READY"), start_s), \
        "the program should run and print ready in Messages:\n%s" \
        % _screen(w)


def _bottom_row(w):
    return w.display(drain=0)[-1]


def _assert_paused_at(w, line, what):
    """The line is marked and the status row says Paused."""
    assert _wait_for(w, lambda: _mark_line(w) == line, PAUSE_TIMEOUT_S), \
        "%s should mark line %d (mark on %s):\n%s" \
        % (what, line, _mark_line(w), _screen(w))
    assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                     PAUSE_TIMEOUT_S), \
        "%s should say %r on the status row:\n%s" \
        % (what, PAUSED_LINE, _screen(w))


def _assert_debugger_takes_commands(w):
    """The pause is a real stop: Ctrl-G K opens the Stack window instead of
    being refused because the program is running."""
    w.key(CTRL_G, "k", delay=0.8)
    assert _wait_for(w, lambda: " Stack " in _screen(w), STEP_TIMEOUT_S), \
        "Stack should open at the pause:\n%s" % _screen(w)
    assert RUNNING_REFUSAL not in _screen(w), \
        "the program is stopped, not running:\n%s" % _screen(w)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_gdb(tmp_path):
    """Ctrl-G I while the program sleeps stops it and marks the sleep call in
    main, not the library function gdb stopped in, and shows no error box
    for the interrupt."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_SLEEP_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_then_run_on(w, GDB_SLEEP_BREAK_LINE, START_TIMEOUT_S)
        w.key(*CTRL_G_I, delay=0.5)
        _assert_paused_at(w, GDB_SLEEP_LINE, "Ctrl-G I")
        assert "SIGINT" not in _screen(w), \
            "the interrupt is not an error to show:\n%s" % _screen(w)
        _assert_debugger_takes_commands(w)


GDB_LOCAL_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int counter = 41;\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  counter++;\n"
    "  sleep(600);\n"
    "  return counter;\n"
    "}\n"
)
GDB_LOCAL_BREAK_LINE = 6          # printf("READY\n")
GDB_LOCAL_SLEEP_LINE = 9          # sleep(600)


# @scenario SC-debug-45
def test_pause_under_gdb_leaves_watches_on_locals_working(tmp_path):
    """A watch on a local of main shows its value after Pause, as at a
    breakpoint: gdb stopped in the library function, so the frame of the
    marked line has to be the one the watch is evaluated in."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_LOCAL_PROG, filename="p.c",
                    wait=1.5) as w:
        _go_to_line(w, GDB_LOCAL_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == GDB_LOCAL_BREAK_LINE,
                         START_TIMEOUT_S), _screen(w)
        _add_watch(w, "counter")
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _messages_hold(w, "READY"),
                         START_TIMEOUT_S), _screen(w)
        w._drain(PAUSE_SETTLE_S)
        w.key(*CTRL_G_I, delay=0.5)
        _assert_paused_at(w, GDB_LOCAL_SLEEP_LINE, "Ctrl-G I")
        assert _wait_for(w, lambda: "42" in (_watch_row(w, "counter") or ""),
                         STEP_TIMEOUT_S), \
            "the watch should read 42 after Pause (row %r):\n%s" \
            % (_watch_row(w, "counter"), _screen(w))


GDB_LOOP_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int i;\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  for (i = 0; i < 100; i++)\n"
    "    sleep(3);\n"
    "  return 0;\n"
    "}\n"
)
GDB_LOOP_BREAK_LINE = 6           # printf("READY\n")
GDB_LOOP_SLEEP_LINE = 9           # sleep(3)
GDB_LOOP_MID_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int i;\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  for (i = 0; i < 100; i++) {\n"
    "    i += sleep(3);\n"
    "    puts(\"tick\");\n"
    "  }\n"
    "  return 0;\n"
    "}\n"
)
# program, the line Pause marks, the line F8 must mark next
GDB_F8_AFTER_PAUSE = {
    "returns-to-a-line-start": (GDB_LOOP_PROG, GDB_LOOP_SLEEP_LINE,
                                GDB_LOOP_SLEEP_LINE - 1),
    "returns-mid-line": (GDB_LOOP_MID_PROG, 9, 10),
}


# @scenario SC-debug-45
@pytest.mark.parametrize("which", ["returns-to-a-line-start",
                                   "returns-mid-line"])
def test_f8_after_a_pause_in_a_blocking_call_marks_the_next_user_line(
        tmp_path, which):
    """F8 after Pause finishes the paused line, the rest of the blocking
    call, and stops in the user's code: the next line of the loop, not
    library code or a frame without source.  The call returns either to the
    start of a line or into the middle of the paused one."""
    _need("gcc", "gdb")
    prog, sleep_line, want = GDB_F8_AFTER_PAUSE[which]
    with WpeSession(str(tmp_path), prog, filename="p.c", wait=1.5) as w:
        _stop_then_run_on(w, GDB_LOOP_BREAK_LINE, START_TIMEOUT_S)
        w._drain(1.0)
        w.key(*CTRL_G_I, delay=0.5)
        _assert_paused_at(w, sleep_line, "Ctrl-G I")
        w.key(F8, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == want, PAUSE_TIMEOUT_S), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (want, _mark_line(w), _screen(w))


# @scenario SC-debug-45
def test_debug_interrupt_in_the_menu_pauses_the_program(tmp_path):
    """Debug > Interrupt (pause), Alt-D then I, does what the keys do: the
    menu is the pause a mouse user always has."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_SLEEP_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_then_run_on(w, GDB_SLEEP_BREAK_LINE, START_TIMEOUT_S)
        w.menu(ALT.DEBUG, "i", delay=0.5)
        _assert_paused_at(w, GDB_SLEEP_LINE, "Debug > Interrupt (pause)")


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_pdb(tmp_path):
    """Ctrl-G I while the script sleeps stops it on the sleep line."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_SLEEP_PROG, filename="p.py",
                    wait=1.5) as w:
        _stop_then_run_on(w, PY_SLEEP_BREAK_LINE, START_TIMEOUT_S,
                          in_messages=False)
        w.key(*CTRL_G_I, delay=0.5)
        _assert_paused_at(w, PY_SLEEP_LINE, "Ctrl-G I")
        _assert_debugger_takes_commands(w)


# A script that adds a byte to a file every tick, so the file's size tells
# whether the loop runs (pdb shows the script's own text only at a stop).
PY_TICK_BREAK_LINE = 2            # print('ready')
PY_TICK_PROG = (
    "import time\n"
    "print('ready', flush=True)\n"
    "while True:\n"
    "    with open(%r, 'a') as f:\n"
    "        f.write('x')\n"
    "    time.sleep(0.2)\n"
)
PY_TICK_MORE = 5                  # ticks that prove the loop runs again


def _ticks(path):
    """How many ticks the script has written so far."""
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


# @scenario SC-debug-45
def test_continue_after_pause_resumes_under_pdb(tmp_path):
    """Ctrl-G I stops the looping script; Ctrl-G R then runs it again, which
    only the loop's own file writes can show."""
    _need("python3")
    ticks = str(tmp_path / "ticks")
    with WpeSession(str(tmp_path), PY_TICK_PROG % ticks, filename="p.py",
                    wait=1.5) as w:
        _stop_then_run_on(w, PY_TICK_BREAK_LINE, START_TIMEOUT_S,
                          in_messages=False)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G I should stop the script:\n%s" % _screen(w)
        paused_at = _ticks(ticks)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _ticks(ticks) >= paused_at + PY_TICK_MORE,
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G R should run the script on (%d ticks at the pause, %d " \
            "now):\n%s" % (paused_at, _ticks(ticks), _screen(w))


PY_TICK_FIRST_LINE = 3            # while True:
PY_TICK_LAST_LINE = 6             # time.sleep(0.2)
PDB_OWN_FILES = ("pdb.py", "bdb.py")


# @scenario SC-debug-45
def test_pause_under_pdb_with_a_breakpoint_shows_the_scripts_line_not_pdbs(
        tmp_path):
    """With a breakpoint set, pdb's interrupt first stops inside its own
    files; the Pause shows a line of the loop, and the Stack window lists
    only the script's frames: no sigint_handler, no row of another file."""
    _need("python3")
    ticks = str(tmp_path / "ticks")
    with WpeSession(str(tmp_path), PY_TICK_PROG % ticks, filename="p.py",
                    wait=1.5, cols=STACK_COLS) as w:
        _stop_then_run_on(w, PY_TICK_BREAK_LINE, START_TIMEOUT_S,
                          in_messages=False)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G I should stop the script:\n%s" % _screen(w)
        mark = _mark_line(w)
        assert mark is not None and \
            PY_TICK_FIRST_LINE <= mark <= PY_TICK_LAST_LINE, \
            "the pause should mark a line of the loop (mark on %s):\n%s" \
            % (mark, _screen(w))
        _assert_debugger_takes_commands(w)
        for name in PDB_OWN_FILES:
            assert name not in _screen(w), \
                "the Stack window should show no frame of %s:\n%s" \
                % (name, _screen(w))
        frames = _stack_frames(w)
        assert frames, "the Stack window should list frames:\n%s" % _screen(w)
        assert not any(PDB_HANDLER in row for row in frames), \
            "the Stack window should not list pdb's handler:\n%s" % _screen(w)
        assert all(PY_SCRIPT_FILE in row for row in frames), \
            "every row should be a frame of the script:\n%s" % _screen(w)


# A script with a frame under the loop's, holding a local the loop's frame
# has not: a watch on it reads only in the frame the Stack window selected.
PY_NESTED_PROG = (
    "import time\n"
    "print('ready', flush=True)\n"
    "def tick(path):\n"
    "    while True:\n"
    "        with open(path, 'a') as f:\n"
    "            f.write('x')\n"
    "        time.sleep(0.2)\n"
    "def outer(path):\n"
    "    marker = 77\n"
    "    tick(path)\n"
    "outer(%r)\n"
)
PY_NESTED_MARKER = "77"
PY_NESTED_CALLER_ROW = 1          # "#1  outer", under the loop's "#0  tick"
PDB_HANDLER = "sigint_handler"
PY_SCRIPT_FILE = "p.py"


# @scenario SC-debug-45
def test_pause_under_pdb_selects_the_right_frame_in_the_stack_window(tmp_path):
    """After a Pause with a breakpoint the Stack window starts at the script's
    loop; Enter on the row below it makes that frame the one the Watches read
    (marker is 77 there, undefined in the loop's frame), and Continue runs
    the script on."""
    _need("python3")
    ticks = str(tmp_path / "ticks")
    with WpeSession(str(tmp_path), PY_NESTED_PROG % ticks, filename="p.py",
                    wait=1.5, cols=STACK_COLS) as w:
        _stop_then_run_on(w, PY_TICK_BREAK_LINE, START_TIMEOUT_S,
                          in_messages=False)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G I should stop the script:\n%s" % _screen(w)
        _add_watch(w, "marker")
        w.key(CTRL_G, "k", delay=0.8)
        assert _wait_for(w, lambda: len(_stack_frames(w)) > \
                         PY_NESTED_CALLER_ROW, STEP_TIMEOUT_S), \
            "Ctrl-G K should list the script's frames:\n%s" % _screen(w)
        frames = _stack_frames(w)
        assert "tick" in frames[0] and "outer" in frames[1], \
            "the rows should be tick over outer:\n%s" % _screen(w)
        for _ in range(PY_NESTED_CALLER_ROW):
            w.key(DOWN, delay=0.3)
        w.key("\r", delay=0.8)
        _watches_on_top(w)
        assert _wait_for(w, lambda: PY_NESTED_MARKER in
                         (_watch_row(w, "marker") or ""), STEP_TIMEOUT_S), \
            "marker should read 77 in the selected frame (row %r):\n%s" \
            % (_watch_row(w, "marker"), _screen(w))
        paused_at = _ticks(ticks)
        _source_window_on_top(w)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _ticks(ticks) >= paused_at + PY_TICK_MORE,
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G R should run the script on (%d ticks at the pause, %d " \
            "now):\n%s" % (paused_at, _ticks(ticks), _screen(w))


# A script with a function of the same name as pdb's SIGINT handler: only
# pdb's own handler is left out of the Stack window, never the script's.
PY_OWN_HANDLER_NAME_PROG = (
    "def leaf():\n"
    "    return 1\n"
    "def sigint_handler():\n"
    "    return leaf()\n"
    "sigint_handler()\n"
)
PY_OWN_HANDLER_NAME_LINE = 2      # return 1, in leaf
PY_OWN_HANDLER_NAME_ROWS = ("leaf", PDB_HANDLER, "<module>")


# @scenario SC-debug-45
def test_a_script_function_named_like_pdbs_handler_stays_in_the_stack_window(
        tmp_path):
    """A breakpoint in a function the script's own sigint_handler calls: the
    Stack window lists leaf, sigint_handler and the module, all of p.py."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_OWN_HANDLER_NAME_PROG, filename="p.py",
                    wait=1.5, cols=STACK_COLS) as w:
        _go_to_line(w, PY_OWN_HANDLER_NAME_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == PY_OWN_HANDLER_NAME_LINE,
                         START_TIMEOUT_S), \
            "the session should stop on line %d:\n%s" \
            % (PY_OWN_HANDLER_NAME_LINE, _screen(w))
        w.key(CTRL_G, "k", delay=0.8)
        rows = len(PY_OWN_HANDLER_NAME_ROWS)
        assert _wait_for(w, lambda: len(_stack_frames(w)) >= rows,
                         STEP_TIMEOUT_S), \
            "the Stack window should list %d frames:\n%s" % (rows, _screen(w))
        frames = _stack_frames(w)
        for row, func in zip(frames, PY_OWN_HANDLER_NAME_ROWS):
            assert func in row and PY_SCRIPT_FILE in row, \
                "the rows should be %r:\n%s" \
                % (PY_OWN_HANDLER_NAME_ROWS, _screen(w))


# A script that handles SIGINT itself: the interrupt reaches its handler, not
# pdb, so the script goes on and the pause is only its next stop.  pdb puts
# its own handler in while it continues and takes it out at the stop, so the
# script installs its handler after the run has begun.
PY_OWN_HANDLER_PROG = (
    "import signal, time\n"
    "got = []\n"
    "print('ready', flush=True)\n"
    "time.sleep(1)\n"
    "signal.signal(signal.SIGINT, lambda s, f: got.append(1))\n"
    "while not got:\n"
    "    time.sleep(0.1)\n"
    "x = 1\n"
)
PY_OWN_HANDLER_FIRST_LINE = 3     # print('ready')
PY_OWN_HANDLER_NEXT_LINE = 8      # x = 1, the next breakpoint


# @scenario SC-debug-45
def test_pause_under_pdb_with_the_scripts_own_sigint_handler_waits_for_the_next_stop(
        tmp_path):
    """The script handles SIGINT itself, so the interrupt does not stop it:
    the pause becomes the next stop, the breakpoint on line 8, and the
    status row says that the interrupt did not stop the program."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_OWN_HANDLER_PROG, filename="p.py",
                    wait=1.5) as w:
        _go_to_line(w, PY_OWN_HANDLER_FIRST_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        _go_to_line(w, PY_OWN_HANDLER_NEXT_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == PY_OWN_HANDLER_FIRST_LINE,
                         START_TIMEOUT_S), \
            "pdb should stop on line %d:\n%s" \
            % (PY_OWN_HANDLER_FIRST_LINE, _screen(w))
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: "^Brk Pause" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "the program should run:\n%s" % _screen(w)
        w._drain(PAUSE_SETTLE_S)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == PY_OWN_HANDLER_NEXT_LINE,
                         PAUSE_TIMEOUT_S), \
            "the script should go on to line %d (mark on %s):\n%s" \
            % (PY_OWN_HANDLER_NEXT_LINE, _mark_line(w), _screen(w))
        assert _wait_for(w, lambda: "did not stop the program"
                         in _bottom_row(w), PAUSE_TIMEOUT_S), \
            "the status row should say the interrupt missed:\n%s" \
            % _screen(w)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_a68g(tmp_path):
    """Ctrl-G I while the Algol 68 loop runs stops it on the loop's line:
    a68g asks whether to terminate, and the editor answers no."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_PAUSE_PROG, filename="p.a68",
                    wait=2.0) as w:
        _stop_then_run_on(w, A68_PAUSE_BREAK_LINE, START_TIMEOUT_S)
        w.key(*CTRL_G_I, delay=0.5)
        _assert_paused_at(w, A68_PAUSE_LINE, "Ctrl-G I")
        assert _children(w, "a68g"), \
            "a68g should still run, paused:\n%s" % _screen(w)
        assert "Terminate" not in _screen(w), \
            "the terminate question is the editor's to answer:\n%s" \
            % _screen(w)


# A Java program whose main loop never ends within the test, with a counter
# the Watches window can follow; jdb and the adapters pause it in the loop.
JDB_PAUSE_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) {\n"
    "        long n = 0;\n"
    "        System.out.println(\"ready\");\n"
    "        while (n < 4000000000000L) {\n"
    "            n++;\n"
    "        }\n"
    "        System.out.println(n);\n"
    "    }\n"
    "}\n"
)
GO_PAUSE_PROG = (
    "package main\n"
    "\n"
    "import \"fmt\"\n"
    "\n"
    "func main() {\n"
    "\tn := 0\n"
    "\tfmt.Println(\"ready\")\n"
    "\tfor n < 2000000000000 {\n"
    "\t\tn++\n"
    "\t}\n"
    "\tfmt.Println(n)\n"
    "}\n"
)
RUST_PAUSE_PROG = (
    "fn main() {\n"
    "    let mut n: u64 = 0;\n"
    "    println!(\"ready\");\n"
    "    while std::hint::black_box(n) < 4_000_000_000_000 {\n"
    "        n += 1;\n"
    "    }\n"
    "    println!(\"{}\", n);\n"
    "}\n"
)
SCALA_PAUSE_PROG = (
    "object P:\n"
    "  def main(args: Array[String]): Unit =\n"
    "    var n = 0L\n"
    "    println(\"ready\")\n"
    "    while n < 4000000000000L do\n"
    "      n += 1\n"
    "    println(n)\n"
)
# the line that prints "ready", and the lines of the loop that follows
JDB_PAUSE_LINES = (4, (5, 6))
GO_PAUSE_LINES = (7, (8, 9))
RUST_PAUSE_LINES = (3, (4, 5))
SCALA_PAUSE_LINES = (4, (5, 6))
PAUSE_RUN_BAR_S = 8
JVM_COMM = "java"       # the kernel's command name of a JVM


def _assert_paused_in_loop(w, loop, what):
    """The mark is on a line of the loop and the status row says Paused."""
    assert _wait_for(w, lambda: _mark_line(w) in loop, PAUSE_TIMEOUT_S), \
        "%s should mark a line of the loop %s (mark on %s):\n%s" \
        % (what, loop, _mark_line(w), _screen(w))
    assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                     PAUSE_TIMEOUT_S), \
        "%s should say %r on the status row:\n%s" \
        % (what, PAUSED_LINE, _screen(w))


def _wait_for_the_pause_bar(w):
    """The source window's bottom row names Pause: the program runs."""
    _source_window_on_top(w)
    assert _wait_for(w, lambda: "^Brk Pause" in _bottom_bar(w),
                     PAUSE_RUN_BAR_S), \
        "the program should run:\n%s" % _screen(w)


def _run_on_with_a_watch(w, break_line, start_s):
    """Breakpoint on @break_line, Ctrl-G R to the stop, a watch on n, then
    Ctrl-G R to run on into the loop; return once the program runs and has
    had time to print and loop."""
    _go_to_line(w, break_line)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == break_line, start_s), \
        "the session should stop on line %d (mark on %s):\n%s" \
        % (break_line, _mark_line(w), _screen(w))
    _add_watch(w, "n")
    _source_window_on_top(w)
    w.key(CTRL_G, "r", delay=0.5)
    _wait_for_the_pause_bar(w)
    w._drain(PAUSE_SETTLE_S)


def _watches_on_top(w):
    """Bring the Watches window forward: the program's output may have
    opened Messages over it, whichever number each got."""
    for key in ("\0332", "\0333", "\0334"):
        if " Watches " in _screen(w):
            return
        w.key(key, delay=0.4)


def _pause_in_the_loop_twice(w, spec, start_s):
    """Pause marks a line of the loop and refreshes the watch; Continue and a
    second Pause mark the loop again with a newer value."""
    break_line, loop = spec
    _run_on_with_a_watch(w, break_line, start_s)
    w.key(*CTRL_G_I, delay=0.5)
    _assert_paused_in_loop(w, loop, "Ctrl-G I")
    _watches_on_top(w)
    assert _wait_for(w, lambda: _watch_value(w, "n") is not None,
                     STEP_TIMEOUT_S), \
        "the watch should show n after Pause:\n%s" % _screen(w)
    first = _watch_value(w, "n")
    _assert_debugger_takes_commands(w)
    _source_window_on_top(w)
    w.key(CTRL_G, "r", delay=0.5)
    _wait_for_the_pause_bar(w)
    w._drain(PAUSE_SETTLE_S)
    w.key(*CTRL_G_I, delay=0.5)
    _assert_paused_in_loop(w, loop, "the second Ctrl-G I")
    _watches_on_top(w)
    assert _wait_for(w, lambda: (_watch_value(w, "n") or 0) > first,
                     STEP_TIMEOUT_S), \
        "the watch should read more than %d after the second Pause (%s):\n%s" \
        % (first, _watch_value(w, "n"), _screen(w))


def _pause_in_the_loop_twice_then_quit(w, spec, start_s):
    """The two Pauses of _pause_in_the_loop_twice; Quit then leaves no
    process of the session behind."""
    _pause_in_the_loop_twice(w, spec, start_s)
    tree = procinfo.descendants(w.proc.pid)
    assert tree, "the debugger and the program should be running"
    w.menu(ALT.DEBUG, "q", delay=0.5)
    assert _wait_for(w, lambda: procinfo.all_ended(tree), STEP_TIMEOUT_S), \
        "no process of the session should be left after Quit (alive: %s):\n%s" \
        % ([p for p in tree if not procinfo.all_ended([p])], _screen(w))


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_jdb(tmp_path):
    """Ctrl-G I while the Java loop runs suspends the VM and marks the loop
    in P.java, with the watch refreshed; Continue resumes it and a second
    Pause marks the loop again."""
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_PAUSE_PROG, filename="P.java",
                    wait=1.5) as w:
        _pause_in_the_loop_twice_then_quit(w, JDB_PAUSE_LINES,
                                           JDB_START_TIMEOUT_S)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_dlv(tmp_path):
    """The same under dlv, which may stop inside Go's runtime: the mark is
    the first line of main.go, and the row says Paused, not the stop's
    reason."""
    _need("dlv", "go")
    (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), GO_PAUSE_PROG, filename="main.go",
                    wait=2.0) as w:
        _pause_in_the_loop_twice_then_quit(w, GO_PAUSE_LINES, GOTO_TIMEOUT_S)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_lldb_dap(tmp_path):
    """The same under lldb-dap, which reports the pause as a SIGSTOP
    exception: the row says Paused all the same."""
    _need("rustc", "lldb-dap")
    with WpeSession(str(tmp_path), RUST_PAUSE_PROG, filename="main.rs",
                    wait=2.0, env_extra={"XWPE_DAP_ADAPTER": "lldb"}) as w:
        _pause_in_the_loop_twice_then_quit(w, RUST_PAUSE_LINES,
                                           RUST_START_TIMEOUT_S)
        assert "SIGSTOP" not in _screen(w), \
            "the adapter's word for the stop is not shown:\n%s" % _screen(w)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_gdb_dap(tmp_path):
    """The same under gdb's adapter."""
    _need("rustc", "gdb")
    with WpeSession(str(tmp_path), RUST_PAUSE_PROG, filename="main.rs",
                    wait=2.0, env_extra={"XWPE_DAP_ADAPTER": "gdb"}) as w:
        _pause_in_the_loop_twice_then_quit(w, RUST_PAUSE_LINES,
                                           RUST_START_TIMEOUT_S)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_scala(tmp_path):
    """The same under the Scala adapter."""
    _need("scala-cli")
    workdir = str(tmp_path)
    try:
        with WpeSession(workdir, SCALA_PAUSE_PROG, filename="p.scala",
                        wait=2.0) as w:
            _pause_in_the_loop_twice(w, SCALA_PAUSE_LINES,
                                     SCALA_START_TIMEOUT_S)
            _quit_ends_the_scala_program(w, workdir)
    finally:
        for pid in procinfo.processes_in(workdir, JVM_COMM):
            os.kill(pid, signal.SIGKILL)


def _quit_ends_the_scala_program(w, workdir):
    """Quit ends the program the build server runs for this session.  The
    program is Bloop's child, not the editor's, and Bloop itself is shared:
    it may be the editor's descendant (this session's launcher started it)
    and rightly outlives Quit, so the program is counted, not the editor's
    tree."""
    programs = procinfo.processes_in(workdir, JVM_COMM)
    assert len(programs) == 1, \
        "one program JVM should run in %s (found %s)" % (workdir, programs)
    server = procinfo.parent_of(programs[0])
    w.menu(ALT.DEBUG, "q", delay=0.5)
    assert _wait_for(
        w, lambda: not procinfo.children_in(server, workdir, JVM_COMM),
        STEP_TIMEOUT_S), \
        "the program (pid %d) should end on Quit:\n%s" \
        % (programs[0], _screen(w))


GDB_SEGV_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  int *p = 0;\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  *p = 1;\n"
    "  return 0;\n"
    "}\n"
)
GDB_SEGV_BREAK_LINE = 5           # printf("READY\n")


# @scenario SC-debug-46
def test_a_signal_box_leaves_the_bottom_row_as_it_was(tmp_path):
    """A program that dies of a signal shows its box; while the box is up the
    bottom row is not redrawn with the stepping keys, as it was not before
    the bar followed the run (the row changes once the box is gone)."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_SEGV_PROG, filename="p.c",
                    wait=1.5) as w:
        _go_to_line(w, GDB_SEGV_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == GDB_SEGV_BREAK_LINE,
                         START_TIMEOUT_S), _screen(w)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: "SIGSEGV" in _screen(w),
                         START_TIMEOUT_S), \
            "the signal's box should appear:\n%s" % _screen(w)
        assert "F8 Step" not in _bottom_bar(w), \
            "under the box the row must not name the stepping keys:\n%r" \
            % _bottom_bar(w)
        w.key("\r", delay=1.0)
        assert _wait_for(w, lambda: "F8 Step" in _bottom_bar(w),
                         STEP_TIMEOUT_S), \
            "once the box is gone the stepping bar is back:\n%s" % _screen(w)


# @scenario SC-debug-46
def test_the_running_bar_names_pause_and_its_click_pauses(tmp_path):
    """While the program runs the source window's bottom row names Pause and
    not the stepping keys; a click on "^Brk Pause" pauses the program (a
    terminal that never sends Ctrl-Break can still pause), and at the stop the
    stepping bar is back."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_SLEEP_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_then_run_on(w, GDB_SLEEP_BREAK_LINE, START_TIMEOUT_S)
        # The program's output made Messages the window the user types in,
        # so the bar of the window on top is the one that must name Pause.
        bar = _bottom_bar(w)
        assert "^Brk Pause" in bar, \
            "while it runs the bar should name Pause:\n%r\n%s" \
            % (bar, _screen(w))
        for refused in ("F8 Step", "F4 Go to cursor", "^F8 Breakpoint",
                        "^F7 Watch"):
            assert refused not in bar, \
                "%r is refused while it runs, so the bar must not offer " \
                "it:\n%r" % (refused, bar)
        _click_bottom_bar(w, "^Brk Pause")
        _assert_paused_at(w, GDB_SLEEP_LINE, "a click on ^Brk Pause")
        _source_window_on_top(w)
        assert "F4 Go to cursor" in _bottom_bar(w), \
            "at the stop the stepping bar should be back:\n%s" % _screen(w)


# @scenario SC-debug-45
def test_pause_at_a_stop_or_without_a_session_only_says_so(tmp_path):
    """With no session, and at a stop, Pause names the reason on the status
    row and sends the debugger nothing: the mark stays, and F8 steps on."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: "no debugger session" in _bottom_row(w),
                         STEP_TIMEOUT_S), \
            "with no session Pause should say so:\n%s" % _screen(w)
        assert not _children(w, "gdb"), "Pause must not start a debugger"
        _start_c_session(w)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: "stopped" in _bottom_row(w),
                         STEP_TIMEOUT_S), \
            "at a stop Pause should say so:\n%s" % _screen(w)
        assert _mark_line(w) == C_BREAK_LINE, \
            "the mark must stay on line %d:\n%s" % (C_BREAK_LINE, _screen(w))
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 should still step on from the stop:\n%s" % _screen(w)


# Prints which of SIGINT and SIGPIPE its signal mask says are ignored, then
# sleeps.  A program started from a shell ignores neither.
SIGMASK_C_PROG = (
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <string.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  char row[256];\n"
    "  unsigned long ign = 0;\n"
    "  FILE *f = fopen(\"/proc/self/status\", \"r\");\n"
    "  while (f && fgets(row, sizeof row, f))\n"
    "    if (!strncmp(row, \"SigIgn:\", 7))\n"
    "      ign = strtoul(row + 7, NULL, 16);\n"
    "  printf(\"SIGINT_IGNORED=%d\\n\", (int)((ign >> 1) & 1));\n"
    "  printf(\"SIGPIPE_IGNORED=%d\\n\", (int)((ign >> 12) & 1));\n"
    "  fflush(stdout);\n"
    "  sleep(600);\n"
    "  return 0;\n"
    "}\n"
)


# @scenario SC-debug-45
def test_the_program_under_gdb_does_not_inherit_ignored_signals(tmp_path):
    """The editor ignores SIGINT and SIGPIPE; the program under gdb must not
    inherit that, or Pause's SIGINT cannot reach it and a program relying on
    SIGPIPE behaves differently than outside the editor."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), SIGMASK_C_PROG, filename="p.c",
                    wait=1.5) as w:
        w.key(CTRL_G, "r", delay=1.0)          # to the start of main
        assert _wait_for(w, lambda: _mark_line(w) is not None,
                         START_TIMEOUT_S), \
            "the session should stop at main:\n%s" % _screen(w)
        w.key(CTRL_G, "r", delay=0.5)          # run on
        assert _wait_for(w, lambda: _messages_hold(w, "SIGPIPE_IGNORED="),
                         START_TIMEOUT_S), \
            "the program should print its signal mask:\n%s" % _screen(w)
        assert _messages_hold(w, "SIGINT_IGNORED=0"), \
            "the program must not inherit an ignored SIGINT:\n%s" \
            % _screen(w)
        assert _messages_hold(w, "SIGPIPE_IGNORED=0"), \
            "the program must not inherit an ignored SIGPIPE:\n%s" \
            % _screen(w)


# The script runs a shell and prints the shell's ignored-signal mask: Python
# gives its children SIGPIPE back, but a SIGINT the script was started with as
# ignored stays ignored in them.
PY_SIGMASK_PROG = (
    "import subprocess\n"
    "out = subprocess.run(['sh', '-c', 'grep SigIgn /proc/self/status'], "
    "capture_output=True, text=True).stdout\n"
    "mask = int(out.split()[1], 16)\n"
    "open('sigmask.txt', 'w').write('SIGINT_IGNORED=%d' % ((mask >> 1) & 1))\n"
    "x = 1\n"
)
PY_SIGMASK_RUN_LINE = 2           # the line that runs the shell
PY_SIGMASK_PRINT_LINE = 4         # the line that writes the mask


# @scenario SC-debug-45
def test_the_script_under_pdb_does_not_inherit_ignored_sigint(tmp_path):
    """The script's children start with SIGINT as the shell would give it:
    not ignored."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_SIGMASK_PROG, filename="p.py",
                    wait=1.5) as w:
        _go_to_line(w, PY_SIGMASK_RUN_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == PY_SIGMASK_RUN_LINE,
                         START_TIMEOUT_S), \
            "pdb should stop on line %d:\n%s" \
            % (PY_SIGMASK_RUN_LINE, _screen(w))
        # Stepped, not continued: pdb takes SIGINT itself while it continues.
        assert _step(w, F8, PY_SIGMASK_PRINT_LINE - 1), \
            "F8 over the shell should mark line %d:\n%s" \
            % (PY_SIGMASK_PRINT_LINE - 1, _screen(w))
        assert _step(w, F8, PY_SIGMASK_PRINT_LINE), \
            "F8 should mark line %d:\n%s" \
            % (PY_SIGMASK_PRINT_LINE, _screen(w))
        w.key(F8, delay=0.5)
        mask_file = tmp_path / "sigmask.txt"
        assert _wait_for(w, mask_file.exists, STEP_TIMEOUT_S), \
            "the script should write its children's mask:\n%s" % _screen(w)
        assert mask_file.read_text() == "SIGINT_IGNORED=0", \
            "the script must not inherit an ignored SIGINT (%s):\n%s" \
            % (mask_file.read_text(), _screen(w))


# The programs under a debug adapter write which of SIGINT and SIGPIPE their
# signal mask says are ignored, to a file, and then wait a while.  Go and Rust
# leave SIGINT alone unless it was ignored when they started; Rust's runtime
# ignores SIGPIPE itself, so that signal is only asked of the Go program.
GO_SIGMASK_PROG = (
    "package main\n\n"
    "import (\"os\"; \"strconv\"; \"strings\"; \"time\")\n\n"
    "func main() {\n"
    "\td, _ := os.ReadFile(\"/proc/self/status\")\n"
    "\tm := uint64(0)\n"
    "\tfor _, r := range strings.Split(string(d), \"\\n\") {\n"
    "\t\tif strings.HasPrefix(r, \"SigIgn:\") {\n"
    "\t\t\tm, _ = strconv.ParseUint(strings.TrimSpace(r[7:]), 16, 64)\n"
    "\t\t}\n"
    "\t}\n"
    "\ttext := \"SIGINT_IGNORED=\" + strconv.Itoa(int(m>>1&1)) +\n"
    "\t\t\" SIGPIPE_IGNORED=\" + strconv.Itoa(int(m>>12&1))\n"
    "\tos.WriteFile(%(path)s, []byte(text), 0644)\n"
    "\ttime.Sleep(20 * time.Second)\n"
    "}\n"
)
RUST_SIGMASK_PROG = (
    "fn main() {\n"
    "    let status = std::fs::read_to_string(\"/proc/self/status\").unwrap();\n"
    "    let row = status.lines().find(|l| l.starts_with(\"SigIgn:\")).unwrap();\n"
    "    let mask = u64::from_str_radix(row[7..].trim(), 16).unwrap();\n"
    "    let text = format!(\"SIGINT_IGNORED={}\", (mask >> 1) & 1);\n"
    "    std::fs::write(%(path)s, text).unwrap();\n"
    "    std::thread::sleep(std::time::Duration::from_secs(20));\n"
    "}\n"
)
SIGMASK_ADAPTER_PROGS = {"go": GO_SIGMASK_PROG, "rust": RUST_SIGMASK_PROG}


# @scenario SC-debug-45
@pytest.mark.parametrize("adapter", DAP_ADAPTERS)
def test_the_program_under_an_adapter_does_not_inherit_ignored_signals(
        tmp_path, adapter):
    """The adapter and the program it starts get the signals a shell gives
    them: SIGINT (and SIGPIPE, for the Go program) is not ignored, or Pause's
    interrupt could not reach the program."""
    case = DAP_CASES[adapter]
    _need(*case["need"])
    if case["lang"] == "go":
        (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    mask_file = tmp_path / "sigmask.txt"
    text = SIGMASK_ADAPTER_PROGS[case["lang"]] % {"path": '"%s"' % mask_file}
    sleep_line = [i + 1 for i, row in enumerate(text.splitlines())
                  if "Sleep" in row or "sleep(" in row][0]
    with WpeSession(str(tmp_path), text, filename=case["file"], wait=2.0,
                    env_extra=case["env"] or None) as w:
        _go_to_line(w, sleep_line)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == sleep_line,
                         case["start_s"]), \
            "the adapter should stop on line %d:\n%s" % (sleep_line, _screen(w))
        got = mask_file.read_text()
        assert "SIGINT_IGNORED=0" in got, \
            "the program must not inherit an ignored SIGINT (%s)" % got
        if case["lang"] == "go":
            assert "SIGPIPE_IGNORED=0" in got, \
                "the program must not inherit an ignored SIGPIPE (%s)" % got


# ---- Ctrl-C typed in the terminal editor during a debug session ------------
#
# The terminal sends SIGINT for Ctrl-C to the processes of its foreground
# group.  The editor ignores it; the debugger and its program no longer do
# (they get the signals a shell gives them, so Pause can reach them), so they
# must not be in that group: a Ctrl-C typed by habit must leave the session
# as it was.

CTRL_C = "\x03"                   # the terminal's interrupt character
CTRL_C_SETTLE_S = 1.5             # the signal, had it been sent, has landed
STDIO_ADAPTERS = ["lldb-dap", "gdb-dap"]
PY_PRINT_LINE = 6                 # print('OUTMARK', y), after PY_NEXT_LINE


# @scenario SC-debug-45
def test_ctrl_c_at_a_gdb_stop_leaves_the_session_working(tmp_path):
    """Ctrl-C at a breakpoint under gdb does nothing: F8 then marks the
    next line, the program has not ended."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        w.key(CTRL_C, delay=0.3)
        w._drain(CTRL_C_SETTLE_S)
        assert _step(w, F8, C_NEXT_LINE), \
            "F8 after Ctrl-C should mark line %d (mark on %s):\n%s" \
            % (C_NEXT_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-45
def test_ctrl_c_at_a_pdb_stop_leaves_the_session_working(tmp_path):
    """Ctrl-C at a breakpoint under pdb does nothing: F8 then marks the
    next line instead of leaving the session hung."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_PROG, filename="p.py", wait=1.5) as w:
        _start_py_session(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_BREAK_LINE,
                         STEP_TIMEOUT_S), \
            "pdb should stop on line %d:\n%s" % (PY_BREAK_LINE, _screen(w))
        w.key(CTRL_C, delay=0.3)
        w._drain(CTRL_C_SETTLE_S)
        assert _step(w, F8, PY_NEXT_LINE), \
            "F8 after Ctrl-C should mark line %d (mark on %s):\n%s" \
            % (PY_NEXT_LINE, _mark_line(w), _screen(w))
        assert _step(w, F8, PY_PRINT_LINE), \
            "the next F8 should mark line %d (mark on %s):\n%s" \
            % (PY_PRINT_LINE, _mark_line(w), _screen(w))


# @scenario SC-debug-45
@pytest.mark.parametrize("adapter", STDIO_ADAPTERS)
def test_ctrl_c_while_a_stdio_adapter_runs_leaves_the_session_working(
        tmp_path, adapter):
    """Ctrl-C while the program runs under an adapter on the editor's pipes
    does nothing: the adapter is still there and the breakpoint ahead is
    marked when the program reaches it."""
    run = DapRun(tmp_path, adapter, sleep_s=DAP_FAR_SLEEP_S)
    try:
        with run.session as w:
            _run_on_towards_done(run, w)
            _says_the_program_is_running(w)
            w.key(CTRL_C, delay=0.3)
            w._drain(CTRL_C_SETTLE_S)
            assert _children(w, run.case["child"]), \
                "Ctrl-C should leave the adapter running:\n%s" % _screen(w)
            assert _wait_for(w, lambda: _mark_line(w) == run.done,
                             DAP_FAR_RUN_S), \
                "the stop on line %d should be marked (mark on %s):\n%s" \
                % (run.done, _mark_line(w), _screen(w))
    finally:
        _kill_program_if_left(run)


# @scenario SC-debug-17
@pytest.mark.parametrize("transport", sorted(SILENT_ADAPTERS))
def test_ctrl_c_while_an_adapter_starts_leaves_it_running(tmp_path,
                                                          transport):
    """Ctrl-C while the editor waits for an adapter that has not answered
    yet leaves the adapter running and the waiting line in place; lldb-dap
    and gdb catch the signal themselves, an adapter that does not would
    otherwise die of it."""
    adapter = SILENT_ADAPTERS[transport]["adapter"]
    with _dap_session(tmp_path, transport, LONG_START_TIMEOUT_S) as w:
        pid = _start_silent_adapter(w, transport)
        w.key(CTRL_C, delay=0.3)
        w._drain(CTRL_C_SETTLE_S)
        assert not _process_gone(pid), \
            "Ctrl-C should leave the adapter (pid %d) running:\n%s" \
            % (pid, _screen(w))
        assert "starting %s" % adapter in _screen(w), \
            "the start should still be waiting:\n%s" % _screen(w)
        w.key(ESC, delay=0.5)
        assert _wait_for(w, lambda: _process_gone(pid), STEP_TIMEOUT_S), \
            "Esc should end the silent adapter (pid %d)" % pid


# ---- breakpoints set while the program runs, and what the debugger said ----
#
# A breakpoint the user sets or removes while the program runs is kept by the
# editor and told to the debugger at the next stop.  Until the debugger has
# confirmed it, the line is drawn as the outline of the red bar: the bar's red
# as the text colour, on the editor's own background.

BRK_STATE_SETTLE_S = 6            # the stop's sync and repaint have run
HOLLOW_WAIT_S = 6

# One case per backend: the program (it keeps running until Pause), the line
# of its first breakpoint, and a line past the loop the user marks while it
# runs.  "need" lists the tools; "env" the adapter choice.
BRK_RUN_CASES = {
    "gdb": {"file": "p.c", "prog": GDB_SLEEP_PROG, "break": 5, "tail": 8,
            "need": ("gcc", "gdb"), "start_s": START_TIMEOUT_S, "env": {},
            "child": "gdb", "wait": 1.5, "gomod": False},
    "pdb": {"file": "p.py", "prog": SLEEPING_PROGS["pdb"]["prog"],
            "break": 2, "tail": 4, "need": ("python3",),
            "start_s": START_TIMEOUT_S, "env": {}, "child": "python3",
            "wait": 1.5, "gomod": False},
    "a68g": {"file": "p.a68", "prog": A68_PAUSE_PROG, "break": 2, "tail": 4,
             "need": ("a68g",), "start_s": START_TIMEOUT_S, "env": {},
             "child": "a68g", "wait": 2.0, "gomod": False},
    "jdb": {"file": "P.java", "prog": SLEEPING_PROGS["jdb"]["prog"],
            "break": 3, "tail": 5, "need": ("jdb", "javac"),
            "start_s": JDB_START_TIMEOUT_S, "env": {}, "child": "jdb",
            "wait": 1.5, "gomod": False},
    "dlv": {"file": "main.go", "prog": SLEEPING_PROGS["dlv"]["prog"],
            "break": 9, "tail": 11, "need": ("dlv", "go"),
            "start_s": GOTO_TIMEOUT_S, "env": {}, "child": "dlv",
            "wait": 2.0, "gomod": True},
    "lldb-dap": {"file": "main.rs", "prog": SLEEPING_PROGS["lldb-dap"]["prog"],
                 "break": 2, "tail": 4, "need": ("rustc", "lldb-dap"),
                 "start_s": RUST_START_TIMEOUT_S,
                 "env": {"XWPE_DAP_ADAPTER": "lldb"}, "child": "lldb-dap",
                 "wait": 2.0, "gomod": False},
    "gdb-dap": {"file": "main.rs", "prog": SLEEPING_PROGS["gdb-dap"]["prog"],
                "break": 2, "tail": 4, "need": ("rustc", "gdb"),
                "start_s": RUST_START_TIMEOUT_S,
                "env": {"XWPE_DAP_ADAPTER": "gdb"}, "child": "gdb",
                "wait": 2.0, "gomod": False},
}


def _run_into_the_program(w, break_line, start_s):
    """A breakpoint on @break_line, Ctrl-G R to the stop, Ctrl-G R to run on;
    return once the program runs (the bar names Pause) and has settled."""
    _go_to_line(w, break_line)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == break_line, start_s), \
        "the session should stop on line %d (mark on %s):\n%s" \
        % (break_line, _mark_line(w), _screen(w))
    w.key(CTRL_G, "r", delay=0.5)
    _wait_for_the_pause_bar(w)
    w._drain(PAUSE_SETTLE_S)


def _unconfirmed_lines(w, fg=UNCONFIRMED_BREAKPOINT_FG):
    return w.unconfirmed_breakpoint_lines(fg=fg)


def _assert_hollow(w, line, what, fg=UNCONFIRMED_BREAKPOINT_FG):
    """@line is drawn as an unconfirmed breakpoint, not as the solid bar."""
    assert _wait_for(w, lambda: line in _unconfirmed_lines(w, fg),
                     HOLLOW_WAIT_S), \
        "%s: line %d should be drawn hollow (hollow %s, solid %s):\n%s" \
        % (what, line, _unconfirmed_lines(w, fg), _breakpoint_lines(w),
           _screen(w))
    assert line not in _breakpoint_lines(w), \
        "%s: line %d must not be the solid bar too:\n%s" \
        % (what, line, _screen(w))


def _assert_solid(w, line, what):
    """@line is drawn as the solid breakpoint bar, not hollow."""
    assert _wait_for(w, lambda: line in _breakpoint_lines(w), HOLLOW_WAIT_S), \
        "%s: line %d should be drawn solid (hollow %s, solid %s):\n%s" \
        % (what, line, _unconfirmed_lines(w), _breakpoint_lines(w),
           _screen(w))
    assert line not in _unconfirmed_lines(w), \
        "%s: line %d must not be hollow too:\n%s" % (what, line, _screen(w))


def _quit_and_see_the_session_gone(w):
    """Debug > Quit leaves none of the session's processes behind."""
    tree = procinfo.descendants(w.proc.pid)
    w.menu(ALT.DEBUG, "q", delay=0.5)
    assert _wait_for(w, lambda: procinfo.all_ended(tree), STEP_TIMEOUT_S), \
        "no process of the session should be left after Quit:\n%s" \
        % _screen(w)


# @scenario SC-debug-07
@pytest.mark.parametrize("debugger", sorted(BRK_RUN_CASES))
def test_a_breakpoint_set_while_running_turns_solid_while_it_runs(
        tmp_path, debugger):
    """A breakpoint set while the program runs is not refused: the debugger
    is told at once and the line turns into the solid bar while the program
    still runs; after a stop (Pause here) it is still the solid bar."""
    case = BRK_RUN_CASES[debugger]
    _need(*case["need"])
    if case["gomod"]:
        (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    with WpeSession(str(tmp_path), case["prog"], filename=case["file"],
                    wait=case["wait"], env_extra=case["env"] or None) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _assert_solid(w, case["break"], "the breakpoint set at the start")
        _go_to_line(w, case["tail"])
        w.key(CTRL_G, "b", delay=0.6)
        assert RUNNING_REFUSAL not in _screen(w), \
            "a breakpoint is no debugger command to refuse:\n%s" % _screen(w)
        _assert_solid(w, case["tail"], "while the program runs")
        assert "^Brk Pause" in _bottom_bar(w), \
            "the program should still run:\n%s" % _screen(w)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G I should stop the program:\n%s" % _screen(w)
        _assert_solid(w, case["tail"], "after the stop")
        _quit_and_see_the_session_gone(w)


# A loop that passes its breakpoint line once every LOOP_SLEEP_S seconds, so
# a line unmarked just after a stop there is reached again only if the
# debugger still has the breakpoint.
LOOP_SLEEP_S = 5
GDB_HOT_LOOP_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int n = 0;\n"
    "  for (;;) {\n"
    "    sleep(%d);\n"
    "    n++;\n"
    "    printf(\"N %%d\\n\", n);\n"
    "    fflush(stdout);\n"
    "  }\n"
    "}\n" % LOOP_SLEEP_S
)
GDB_HOT_LOOP_BREAK_LINE = 8       # n++, right after the sleep
GDB_LOOP_PASSES = 2               # loop passes watched for a wrong stop


def _never_marked_at(w, line, seconds):
    """True when the current-line mark stays off @line for @seconds."""
    deadline = time.time() + seconds * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if _mark_line(w) == line:
            return False
    return True


# @scenario SC-debug-07
def test_a_breakpoint_removed_while_running_is_gone_at_the_next_stop(tmp_path):
    """A line unmarked while the program runs goes from the margin at once
    and from the debugger at the next stop: the loop then passes the line
    again and again without stopping there."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_HOT_LOOP_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on_a_breakpoint_at(w, GDB_HOT_LOOP_BREAK_LINE, START_TIMEOUT_S)
        w.key(CTRL_G, "r", delay=0.5)    # into the sleep; the cursor stays
        w.key(CTRL_G, "b", delay=0.6)    # on the breakpoint's line
        assert RUNNING_REFUSAL not in _screen(w), \
            "removing a breakpoint is no debugger command to refuse:\n%s" \
            % _screen(w)
        assert GDB_HOT_LOOP_BREAK_LINE not in \
            _breakpoint_lines(w) + _unconfirmed_lines(w), \
            "the line should be unmarked at once:\n%s" % _screen(w)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                         PAUSE_TIMEOUT_S), \
            "Ctrl-G I should stop the program:\n%s" % _screen(w)
        w.key(CTRL_G, "r", delay=0.5)    # run on from the pause
        assert _never_marked_at(w, GDB_HOT_LOOP_BREAK_LINE,
                                GDB_LOOP_PASSES * LOOP_SLEEP_S), \
            "the program must not stop on the removed line %d:\n%s" \
            % (GDB_HOT_LOOP_BREAK_LINE, _screen(w))
        _quit_and_see_the_session_gone(w)


NOT_IN_THE_PROGRAM_C = "/* a file the program does not contain */\nint q;\n"
NOT_IN_THE_PROGRAM_JAVA = "public class Q {\n    int q = 1;\n}\n"
# A line with only a comment has no unit: a68g sets no breakpoint on it.  (A
# blank line has none either, but draws nothing a hollow mark could show.)
A68_NO_UNIT_TEXT = "# only a comment #"
A68_BLANK_PROG = ("print ((\"hi\", new line));\n" + A68_NO_UNIT_TEXT +
                  "\nINT x := 3;\nprint ((x, new line))\n")
A68_NO_UNIT_LINE = 2
A68_BLANK_STOP_LINE = 3


def _open_a_second_file(w, name):
    """F3, the file's name, Enter: the file is open on top."""
    w.key(F3, delay=0.5)
    w.key(name, delay=0.6)
    w.key("\r", delay=1.2)


def _stop_on_a_breakpoint_at(w, line, start_s):
    """A breakpoint on @line, Ctrl-G R, and wait for the stop there."""
    _go_to_line(w, line)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == line, start_s), \
        "the session should stop on line %d (mark on %s):\n%s" \
        % (line, _mark_line(w), _screen(w))


def _hollow_texts(w):
    """The text of each source row drawn as an unconfirmed breakpoint.  A
    second file opens in a window of its own, so the rows, not the line
    numbers of the first window, say which line it is."""
    rows = w.display(drain=0)
    return [rows[line + 1] for line in _unconfirmed_lines(w)]


def _assert_says_why_it_is_hollow(w, text, words):
    """The row holding @text is hollow and the status row holds the
    debugger's @words."""
    assert _wait_for(w, lambda: any(text in t for t in _hollow_texts(w)),
                     HOLLOW_WAIT_S), \
        "the row with %r should be drawn hollow (hollow rows %s):\n%s" \
        % (text, _hollow_texts(w), _screen(w))
    assert _wait_for(w, lambda: words in _bottom_row(w), HOLLOW_WAIT_S), \
        "the status row should say %r:\n%s" % (words, _screen(w))


# @scenario SC-debug-07
def test_a_refused_breakpoint_stays_hollow_and_says_why(tmp_path):
    """gdb knows no source file q.c: the breakpoint the user sets there is
    refused, the line stays drawn hollow, and the status row gives gdb's
    words, not a box."""
    _need("gcc", "gdb")
    (tmp_path / "q.c").write_text(NOT_IN_THE_PROGRAM_C)
    with WpeSession(str(tmp_path), GDB_SLEEP_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on_a_breakpoint_at(w, GDB_SLEEP_BREAK_LINE, START_TIMEOUT_S)
        _open_a_second_file(w, "q.c")
        _go_to_line(w, 2)
        w.key(CTRL_G, "b", delay=1.0)
        _assert_says_why_it_is_hollow(w, "int q;", "No source file named q.c")
        assert "not set" in _bottom_row(w), \
            "the row should say the breakpoint is not set:\n%s" % _screen(w)


# @scenario SC-debug-07
def test_a_refused_breakpoint_stays_hollow_and_says_why_under_a68g(tmp_path):
    """a68g sets no breakpoint on a line with no unit (a blank line): the
    line stays hollow and the status row gives a68g's own words."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_BLANK_PROG, filename="p.a68",
                    wait=2.0) as w:
        _stop_on_a_breakpoint_at(w, A68_BLANK_STOP_LINE, START_TIMEOUT_S)
        _go_to_line(w, A68_NO_UNIT_LINE)
        w.key(CTRL_G, "b", delay=1.0)
        _assert_says_why_it_is_hollow(w, A68_NO_UNIT_TEXT,
                                      "cannot set breakpoint in that line")


PDB_COMMENT_TEXT = "# only a comment"
PDB_COMMENT_PROG = "x = 1\n" + PDB_COMMENT_TEXT + "\ny = 2\nprint(x + y)\n"
PDB_COMMENT_LINE = 2
PDB_COMMENT_STOP_LINE = 3


# @scenario SC-debug-07
def test_a_refused_breakpoint_stays_hollow_and_says_why_under_pdb(tmp_path):
    """pdb sets no breakpoint on a comment line: the line stays hollow and
    the status row gives pdb's own words."""
    _need("python3")
    with WpeSession(str(tmp_path), PDB_COMMENT_PROG, filename="p.py",
                    wait=1.5) as w:
        _stop_on_a_breakpoint_at(w, PDB_COMMENT_STOP_LINE, START_TIMEOUT_S)
        _go_to_line(w, PDB_COMMENT_LINE)
        w.key(CTRL_G, "b", delay=1.0)
        _assert_says_why_it_is_hollow(w, PDB_COMMENT_TEXT, "Blank or comment")


A68_RUNNING_REFUSED_PROG = (
    "print ((\"ready\", new line));\n" + A68_NO_UNIT_TEXT +
    "\nINT n := 0;\nWHILE n < 2000000000 DO n +:= 1 OD;\n"
    "print ((n, new line))\n"
)
A68_RUNNING_REFUSED_STOP_LINE = 3


def _set_the_refused_breakpoint_while_running(w, debugger):
    """With the program running, put on the line the debugger will refuse
    a breakpoint; return that line's text and the debugger's words.  The
    text is empty when the stop raises the program's window over the one
    holding the line, so there is no hollow row to look at."""
    if debugger == "gdb":
        _open_a_second_file(w, "q.c")
        _go_to_line(w, 2)
        w.key(CTRL_G, "b", delay=0.6)
        return "", "No source file named q.c"
    _go_to_line(w, A68_NO_UNIT_LINE)
    w.key(CTRL_G, "b", delay=0.6)
    return A68_NO_UNIT_TEXT, "cannot set breakpoint in that line"


# @scenario SC-debug-07
@pytest.mark.parametrize("debugger", ["gdb", "a68g"])
def test_a_breakpoint_refused_while_running_says_why_at_once(
        tmp_path, debugger):
    """A breakpoint set while the program runs is sent at once and the
    debugger refuses it: the status row holds the debugger's words while
    the program goes on running, without a Pause."""
    if debugger == "gdb":
        _need("gcc", "gdb")
        (tmp_path / "q.c").write_text(NOT_IN_THE_PROGRAM_C)
        prog, name = GDB_SLEEP_PROG, "p.c"
        stop_line, start_s = GDB_SLEEP_BREAK_LINE, START_TIMEOUT_S
    else:
        _need("a68g")
        prog, name = A68_RUNNING_REFUSED_PROG, "p.a68"
        stop_line, start_s = A68_RUNNING_REFUSED_STOP_LINE, START_TIMEOUT_S
    with WpeSession(str(tmp_path), prog, filename=name, wait=2.0) as w:
        _stop_then_run_on(w, stop_line, start_s)
        text, words = _set_the_refused_breakpoint_while_running(w, debugger)
        if text:
            _assert_says_why_it_is_hollow(w, text, words)
        assert _wait_for(w, lambda: words in _bottom_row(w), HOLLOW_WAIT_S), \
            "the status row should say %r:\n%s" % (words, _screen(w))
        assert "not set" in _bottom_row(w), \
            "the row should say the breakpoint is not set:\n%s" % _screen(w)
        assert _mark_line(w) is None, \
            "the program should still run, unmarked:\n%s" % _screen(w)


# @scenario SC-debug-07
def test_a_deferred_breakpoint_stays_hollow_and_says_why_under_jdb(tmp_path):
    """jdb keeps a breakpoint of a class that is not loaded for when it
    loads: the line stays hollow, with jdb's word for it on the status
    row."""
    _need("jdb", "javac")
    (tmp_path / "Q.java").write_text(NOT_IN_THE_PROGRAM_JAVA)
    case = BRK_RUN_CASES["jdb"]
    with WpeSession(str(tmp_path), case["prog"], filename="P.java",
                    wait=1.5) as w:
        _stop_on_a_breakpoint_at(w, case["break"], JDB_START_TIMEOUT_S)
        _open_a_second_file(w, "Q.java")
        _go_to_line(w, 2)
        w.key(CTRL_G, "b", delay=1.0)
        _assert_says_why_it_is_hollow(w, "int q = 1;", "Deferring")


# @scenario SC-debug-07
def test_no_hollow_breakpoint_without_a_session(tmp_path):
    """With no debugger session every breakpoint is the plain red bar, as it
    always was; during one the confirmed breakpoint is the bar too, and when
    the session ends the marks are the bar again."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5) as w:
        _go_to_line(w, C_NEXT_LINE)
        w.key(CTRL_G, "b", delay=0.5)
        _assert_solid(w, C_NEXT_LINE, "no session")
        assert _unconfirmed_lines(w) == [], \
            "no line is hollow without a session:\n%s" % _screen(w)
        _go_to_line(w, C_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.5)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == C_BREAK_LINE,
                         START_TIMEOUT_S), \
            "the session should stop on line %d:\n%s" \
            % (C_BREAK_LINE, _screen(w))
        _assert_solid(w, C_NEXT_LINE, "in a session, at a stop")
        w.menu(ALT.DEBUG, "q", delay=0.8)
        _assert_solid(w, C_NEXT_LINE, "after the session")
        assert _unconfirmed_lines(w) == [], \
            "no line is hollow after the session:\n%s" % _screen(w)


# ---- breakpoints applied while the program runs ----
#
# gdb, pdb and a68g read no command while the program runs: the editor stops
# the program with an interrupt, sets or clears the breakpoints and lets it run
# on, showing nothing of that stop.  jdb and the debug adapters take the change
# while the program runs.  Either way the program stops on a line marked while
# it runs, without a Pause first.

LIVE_STOP_TIMEOUT_S = 10          # the interrupt, the change, the next pass

GDB_LIVE_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  long n = 0;\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  for (;;) {\n"
    "    sleep(1);\n"
    "    n++;\n"
    "  }\n"
    "}\n"
)
PY_LIVE_PROG = (
    "import time\n"
    "print('ready', flush=True)\n"
    "n = 0\n"
    "while True:\n"
    "    time.sleep(1)\n"
    "    n += 1\n"
)

# One case per backend: the program (it loops without end), the line of the
# breakpoint it first stops on, and the line of the loop's body the user marks
# while it runs.
BRK_LIVE_CASES = {
    "gdb": {"file": "p.c", "prog": GDB_LIVE_PROG, "break": 6, "body": 10,
            "need": ("gcc", "gdb"), "start_s": START_TIMEOUT_S, "env": {},
            "wait": 1.5, "gomod": False},
    "pdb": {"file": "p.py", "prog": PY_LIVE_PROG, "break": 2, "body": 6,
            "need": ("python3",), "start_s": START_TIMEOUT_S, "env": {},
            "wait": 1.5, "gomod": False},
    "a68g": {"file": "p.a68", "prog": A68_PAUSE_PROG, "break": 2, "body": 3,
             "need": ("a68g",), "start_s": START_TIMEOUT_S, "env": {},
             "wait": 2.0, "gomod": False},
    "jdb": {"file": "P.java", "prog": JDB_PAUSE_PROG, "break": 4, "body": 6,
            "need": ("jdb", "javac"), "start_s": JDB_START_TIMEOUT_S,
            "env": {}, "wait": 1.5, "gomod": False},
    "dlv": {"file": "main.go", "prog": GO_PAUSE_PROG, "break": 7, "body": 9,
            "need": ("dlv", "go"), "start_s": GOTO_TIMEOUT_S, "env": {},
            "wait": 2.0, "gomod": True},
    "lldb-dap": {"file": "main.rs", "prog": RUST_PAUSE_PROG, "break": 3,
                 "body": 5, "need": ("rustc", "lldb-dap"),
                 "start_s": RUST_START_TIMEOUT_S,
                 "env": {"XWPE_DAP_ADAPTER": "lldb"}, "wait": 2.0,
                 "gomod": False},
    "gdb-dap": {"file": "main.rs", "prog": RUST_PAUSE_PROG, "break": 3,
                "body": 5, "need": ("rustc", "gdb"),
                "start_s": RUST_START_TIMEOUT_S,
                "env": {"XWPE_DAP_ADAPTER": "gdb"}, "wait": 2.0,
                "gomod": False},
}


def _live_session(tmp_path, case):
    """The editor on @case's program, with what its debugger needs."""
    _need(*case["need"])
    if case["gomod"]:
        (tmp_path / "go.mod").write_text("module dapdemo\n\ngo 1.21\n")
    return WpeSession(str(tmp_path), case["prog"], filename=case["file"],
                      wait=case["wait"], env_extra=case["env"] or None)


def _assert_stops_on(w, line, what):
    """The program stops on @line, and the stop is no Pause."""
    assert _wait_for(w, lambda: _mark_line(w) == line, LIVE_STOP_TIMEOUT_S), \
        "%s: the program should stop on line %d (mark on %s):\n%s" \
        % (what, line, _mark_line(w), _screen(w))
    assert PAUSED_LINE not in _bottom_row(w), \
        "%s: the stop is the breakpoint's, not a pause:\n%s" \
        % (what, _screen(w))


# @scenario SC-debug-07
@pytest.mark.parametrize("debugger", sorted(BRK_LIVE_CASES))
def test_a_breakpoint_set_while_running_stops_the_program_there(tmp_path,
                                                                debugger):
    """A breakpoint set on the body of a loop the program is running stops
    the program there on the next pass: the user does not have to pause
    first, and the stop the editor made to apply it is never shown."""
    case = BRK_LIVE_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _go_to_line(w, case["body"])
        w.key(CTRL_G, "b", delay=0.6)
        assert RUNNING_REFUSAL not in _screen(w), \
            "a breakpoint is no debugger command to refuse:\n%s" % _screen(w)
        _assert_stops_on(w, case["body"], "a breakpoint set while running")
        _quit_and_see_the_session_gone(w)


# Loops that pass their body line once every LIVE_SLOW_S seconds: a line
# unmarked while the program sleeps is reached again only if the debugger
# still has the breakpoint.
LIVE_SLOW_S = 2
LIVE_SLOW_PASSES = 3              # passes watched for a wrong stop
GDB_SLOW_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  int n = 0;\n"
    "  for (;;) {\n"
    "    sleep(%d);\n"
    "    n++;\n"
    "    printf(\"N %%d\\n\", n);\n"
    "    fflush(stdout);\n"
    "  }\n"
    "}\n" % LIVE_SLOW_S
)
PY_SLOW_PROG = (
    "import time\n"
    "n = 0\n"
    "while True:\n"
    "    time.sleep(%d)\n"
    "    n += 1\n"
    "    print('N', n, flush=True)\n" % LIVE_SLOW_S
)
JDB_LIVE_SLOW_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        int n = 0;\n"
    "        while (true) {\n"
    "            Thread.sleep(%d);\n"
    "            n++;\n"
    "            System.out.println(n);\n"
    "        }\n"
    "    }\n"
    "}\n" % (LIVE_SLOW_S * 1000)
)
GO_SLOW_PROG = (
    "package main\n"
    "\n"
    "import (\n"
    "\t\"fmt\"\n"
    "\t\"time\"\n"
    ")\n"
    "\n"
    "func main() {\n"
    "\tn := 0\n"
    "\tfor {\n"
    "\t\ttime.Sleep(%d * time.Second)\n"
    "\t\tn++\n"
    "\t\tfmt.Println(n)\n"
    "\t}\n"
    "}\n" % LIVE_SLOW_S
)
RUST_SLOW_PROG = (
    "fn main() {\n"
    "    let mut n: u64 = 0;\n"
    "    loop {\n"
    "        std::thread::sleep(std::time::Duration::from_secs(%d));\n"
    "        n += 1;\n"
    "        println!(\"{}\", n);\n"
    "    }\n"
    "}\n" % LIVE_SLOW_S
)
# the program and the body line that follows its sleep, per backend
BRK_SLOW_CASES = {
    "gdb": dict(BRK_LIVE_CASES["gdb"], prog=GDB_SLOW_PROG, body=8),
    "pdb": dict(BRK_LIVE_CASES["pdb"], prog=PY_SLOW_PROG, body=5),
    "jdb": dict(BRK_LIVE_CASES["jdb"], prog=JDB_LIVE_SLOW_PROG, body=6),
    "dlv": dict(BRK_LIVE_CASES["dlv"], prog=GO_SLOW_PROG, body=12),
    "lldb-dap": dict(BRK_LIVE_CASES["lldb-dap"], prog=RUST_SLOW_PROG, body=5),
    "gdb-dap": dict(BRK_LIVE_CASES["gdb-dap"], prog=RUST_SLOW_PROG, body=5),
}


# @scenario SC-debug-07
@pytest.mark.parametrize("debugger", sorted(BRK_SLOW_CASES))
def test_a_breakpoint_removed_while_running_is_not_stopped_at(tmp_path,
                                                             debugger):
    """A breakpoint taken off the body of a loop while the program sleeps
    in it is gone from the debugger before the next pass: the program runs
    past the line pass after pass, with no stop there and no Pause first."""
    case = BRK_SLOW_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _stop_on_a_breakpoint_at(w, case["body"], case["start_s"])
        w.key(CTRL_G, "r", delay=0.3)    # into the sleep; the cursor stays
        w.key(CTRL_G, "b", delay=0.6)    # on the breakpoint's line
        assert case["body"] not in \
            _breakpoint_lines(w) + _unconfirmed_lines(w), \
            "the line should be unmarked at once:\n%s" % _screen(w)
        assert _never_marked_at(w, case["body"],
                                LIVE_SLOW_PASSES * LIVE_SLOW_S), \
            "the program must not stop on the removed line %d:\n%s" \
            % (case["body"], _screen(w))
        _quit_and_see_the_session_gone(w)


QUIET_SYNC_WINDOW_S = 2.5
# A breakpoint toggled with no session writes 55 bytes (its own redraw).
# The toggle and its sync together measured 110 to 156; the bound is four
# times the toggle, far below the 4500 to 9300 a full repaint writes.
QUIET_SYNC_MAX_BYTES = 4 * 55
QUIET_SYNC_GDB_PROG = (
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  long n = 0;\n"
    "  for (;;) {\n"
    "    sleep(30);\n"
    "    n++;\n"
    "  }\n"
    "}\n"
)
QUIET_SYNC_PY_PROG = (
    "import time\n"
    "n = 0\n"
    "while True:\n"
    "    time.sleep(30)\n"
    "    n += 1\n"
)
# the breakpoint the program first stops on, the line toggled while it runs
# (never reached in the window), and whether the sync is seen at once
QUIET_SYNC_CASES = {
    "gdb": dict(BRK_LIVE_CASES["gdb"], prog=QUIET_SYNC_GDB_PROG, break_at=6,
                toggle=7, solid=True),
    "pdb": dict(BRK_LIVE_CASES["pdb"], prog=QUIET_SYNC_PY_PROG, break_at=4,
                toggle=5, solid=False),
    "a68g": dict(BRK_LIVE_CASES["a68g"], break_at=2, toggle=4, solid=True),
    "jdb": dict(BRK_LIVE_CASES["jdb"], break_at=4, toggle=8, solid=True),
    "lldb-dap": dict(BRK_LIVE_CASES["lldb-dap"], break_at=3, toggle=7,
                     solid=True),
}


# @scenario SC-debug-07
@pytest.mark.parametrize("debugger", sorted(QUIET_SYNC_CASES))
def test_a_breakpoint_applied_while_running_repaints_only_its_margin(
        tmp_path, debugger):
    """The change made while the program runs is told to the debugger
    without the screen flashing: what the editor writes in the seconds after
    the toggle stays near the toggle's own redraw, not a whole-screen one."""
    case = QUIET_SYNC_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _stop_on_a_breakpoint_at(w, case["break_at"], case["start_s"])
        w.key(CTRL_G, "r", delay=0.5)
        _go_to_line(w, case["toggle"])
        w._drain(0.3)
        start = len(w.raw)
        w.key(CTRL_G, "b", delay=0.0)
        w._drain(QUIET_SYNC_WINDOW_S)
        written = len(w.raw) - start
        if case["solid"]:
            assert case["toggle"] in _breakpoint_lines(w), \
                "the sync should have happened in the window:\n%s" \
                % _screen(w)
        assert written <= QUIET_SYNC_MAX_BYTES, \
            "%d bytes written for one toggle, at most %d:\n%s" \
            % (written, QUIET_SYNC_MAX_BYTES, _screen(w))
        _quit_and_see_the_session_gone(w)


GDB_LOG_NAME = "gdb-output.log"
GDB_LOG_INIT = (
    "set logging file %s\n"
    "set logging overwrite on\n"
    "set logging enabled on\n"
)
GDB_SIGINT_REPORT = "Program received signal SIGINT"
GDB_TWO_TOGGLE_LINES = (9, 10)    # sleep(1); and n++; of GDB_LIVE_PROG


def _gdb_sigint_reports(tmp_path):
    """How many times gdb has said the program stopped on SIGINT."""
    log = tmp_path / GDB_LOG_NAME
    if not log.exists():
        return 0
    return log.read_text(errors="replace").count(GDB_SIGINT_REPORT)


# @scenario SC-debug-07
def test_two_toggles_while_running_interrupt_once(tmp_path):
    """Two breakpoints set in one breath while the program runs share one
    interrupt: gdb reports a single SIGINT stop, both lines are set, and the
    program stops on the loop."""
    _need("gcc", "gdb")
    (tmp_path / ".gdbinit").write_text(
        GDB_LOG_INIT % (tmp_path / GDB_LOG_NAME))
    first, second = GDB_TWO_TOGGLE_LINES
    with WpeSession(str(tmp_path), GDB_LIVE_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_into_the_program(w, BRK_LIVE_CASES["gdb"]["break"],
                              START_TIMEOUT_S)
        _go_to_line(w, first)
        w.key(CTRL_G + "b" + DOWN + CTRL_G + "b", delay=0.6)
        assert _wait_for(w, lambda: _mark_line(w) in GDB_TWO_TOGGLE_LINES,
                         LIVE_STOP_TIMEOUT_S), \
            "the program should stop on the loop (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        w._drain(PAUSE_SETTLE_S)
        assert _gdb_sigint_reports(tmp_path) == 1, \
            "two toggles should cost one interrupt, gdb reported %d:\n%s" \
            % (_gdb_sigint_reports(tmp_path), _screen(w))
        stop = _mark_line(w)
        other = second if stop == first else first
        _assert_solid(w, other, "the other toggled line")
        _quit_and_see_the_session_gone(w)


# A loop whose body holds an armed breakpoint, a few hundredths of a second
# of spinning apart: every Continue races the interrupt a toggle sends
# against the breakpoint's own stop.
GDB_RACE_PROG = (
    "#include <stdio.h>\n"
    "int main(void)\n"
    "{\n"
    "  long n = 0;\n"
    "  for (;;) {\n"
    "    n++;\n"
    "    for (volatile long k = 0; k < 20000000; k++)\n"
    "      ;\n"
    "  }\n"
    "}\n"
)
GDB_RACE_BODY_LINE = 6            # n++;
# the time between Continue and the toggle, round after round: the toggle
# lands before, near and after the breakpoint's next stop
GDB_RACE_DELAYS_S = (0.01, 0.02, 0.03, 0.05, 0.08, 0.01, 0.02, 0.04)
GDB_RACE_STILL_S = 1.5            # a stopped program does not move meanwhile
GDB_RACE_RUNS_S = 3               # no stop by then: the program runs on


def _watch_number(w, name):
    """The number the Watches window shows for @name, or None."""
    row = _watch_row(w, name) or ""
    m = re.search(r"-?\d+", row)
    return int(m.group(0)) if m else None


def _assert_stays_stopped(w, rnd):
    """The stop is the user's: nothing runs the program on behind his back,
    and the interrupt is never shown."""
    n = _watch_number(w, "n")
    w._drain(GDB_RACE_STILL_S)
    assert "^Brk Pause" not in _bottom_bar(w), \
        "round %d: nothing may run the program on:\n%s" % (rnd, _screen(w))
    assert _watch_number(w, "n") == n, \
        "round %d: the program moved on behind the user's back:\n%s" \
        % (rnd, _screen(w))
    assert "SIGINT" not in _screen(w), \
        "round %d: the interrupt is not the user's to see:\n%s" \
        % (rnd, _screen(w))


def _race_round(w, rnd, delay):
    """Continue, and @delay later take the loop's breakpoint off: the
    interrupt the toggle sends races the breakpoint's next stop.  Return
    with the program stopped on the loop's line and its breakpoint set."""
    line = GDB_RACE_BODY_LINE
    w.key(CTRL_G + "r", delay=delay)
    w.key(CTRL_G + "b", delay=0.3)
    if _wait_for(w, lambda: _mark_line(w) == line, GDB_RACE_RUNS_S):
        _assert_stays_stopped(w, rnd)          # the breakpoint won
        assert line not in _breakpoint_lines(w) + _unconfirmed_lines(w), \
            "round %d: the line was unmarked:\n%s" % (rnd, _screen(w))
        w.key(CTRL_G, "b", delay=0.6)          # set it again, stopped; the
        return "stop"                          # next round stops on it
    assert "^Brk Pause" in _bottom_bar(w), \
        "round %d: the change won, the program runs:\n%s" % (rnd, _screen(w))
    w.key(CTRL_G, "b", delay=0.3)              # set it again, running
    assert _wait_for(w, lambda: _mark_line(w) == line, LIVE_STOP_TIMEOUT_S), \
        "round %d: the breakpoint set again should stop the program:\n%s" \
        % (rnd, _screen(w))
    _assert_stays_stopped(w, rnd)
    return "run"


GDB_RACE_FROZEN_S = 0.5           # the program reaches its breakpoint
GDB_RACE_FROZEN_ROUNDS = 3


def _race_round_frozen(w, rnd):
    """Continue, then hold the editor still while the program reaches its
    breakpoint, so gdb's stop and the toggle wait unread together.  The
    stop is the user's whichever is read first: it is shown, the removal
    is applied, the program advanced one pass and stays there."""
    w.key(CTRL_G + "r", delay=0.1)
    os.kill(w.proc.pid, signal.SIGSTOP)
    try:
        time.sleep(GDB_RACE_FROZEN_S)
        os.write(w.master_fd, (CTRL_G + "b").encode())
    finally:
        os.kill(w.proc.pid, signal.SIGCONT)
    line = GDB_RACE_BODY_LINE
    assert _wait_for(w, lambda: _mark_line(w) == line, LIVE_STOP_TIMEOUT_S), \
        "frozen round %d: the breakpoint's stop should be shown:\n%s" \
        % (rnd, _screen(w))
    _assert_stays_stopped(w, rnd)
    assert line not in _breakpoint_lines(w) + _unconfirmed_lines(w), \
        "frozen round %d: the line was unmarked:\n%s" % (rnd, _screen(w))
    w.key(CTRL_G, "b", delay=0.6)              # set it again, stopped


# @scenario SC-debug-07
def test_a_toggle_racing_a_breakpoint_stop_leaves_the_stop_to_the_user(
        tmp_path):
    """The loop's own breakpoint is taken off just after Continue, so the
    interrupt the toggle sends races the breakpoint's next stop.  When the
    stop wins it is the user's: it is shown, the removal is applied there
    and nothing runs on.  When the interrupt wins the program runs on
    without the breakpoint.  Round after round gdb's answers stay in step:
    the watch reads a number and the Stack window answers."""
    _need("gcc", "gdb")
    (tmp_path / ".gdbinit").write_text(
        GDB_LOG_INIT % (tmp_path / GDB_LOG_NAME))
    with WpeSession(str(tmp_path), GDB_RACE_PROG, filename="p.c",
                    wait=1.5) as w:
        _stop_on_a_breakpoint_at(w, GDB_RACE_BODY_LINE, START_TIMEOUT_S)
        _add_watch(w, "n")
        _source_window_on_top(w)
        assert _wait_for(w, lambda: _watch_number(w, "n") is not None,
                         STEP_TIMEOUT_S), _screen(w)
        for rnd, delay in enumerate(GDB_RACE_DELAYS_S):
            _race_round(w, rnd, delay)
            assert _watch_number(w, "n") is not None, \
                "round %d: the watch should read a number:\n%s" \
                % (rnd, _screen(w))
        for rnd in range(GDB_RACE_FROZEN_ROUNDS):
            n = _watch_number(w, "n")
            _race_round_frozen(w, rnd)
            assert _wait_for(w, lambda: _watch_number(w, "n") == n + 1,
                             STEP_TIMEOUT_S), \
                "frozen round %d: one pass, the watch should read %d:\n%s" \
                % (rnd, n + 1, _screen(w))
        w.key(CTRL_G, "k", delay=0.8)
        assert _wait_for(w, lambda: " Stack " in _screen(w), STEP_TIMEOUT_S), \
            "gdb should still answer the Stack window:\n%s" % _screen(w)
        assert "main" in _screen(w), \
            "the Stack window should show main:\n%s" % _screen(w)


# ---- an interrupt that lost the race, forced ----
#
# Whether the interrupt a toggle sends reaches gdb before or after the
# program's own stop is a matter of microseconds, and the editor always reads
# a stop that is waiting before it reads a key, so no timing of keys reaches
# the two outcomes where the stop won.  A stand-in gdb forces them: it runs
# the real gdb behind pipes, holds back what gdb writes after one Continue
# until the editor's SIGINT arrives, and then answers as gdb does when the
# signal came after the stop (measured on gdb 18): either "Quit" on its error
# stream and one more prompt, or the SIGINT kept for the program and reported
# at its next resume, before it moved.
GDB_STAND_IN_SOURCE = r'''
import os
import re
import select
import signal
import subprocess
import sys
import time

REAL = os.environ["XWPE_REAL_GDB"]
MODE = os.environ["XWPE_STAND_IN_MODE"]
LOG = os.environ["XWPE_STAND_IN_LOG"]
PROMPT = b"(gdb) "
QUIT = "\u274c\ufe0f Quit\n".encode()
RESUME = re.compile(r"^(c|n|s|finish|advance .*)$")
CONTINUE = "c"
SEPARATE_S = 0.2           # the extra prompt comes in a read of its own

wake_r, wake_w = os.pipe()


def note(text):
    with open(LOG, "a") as fh:
        fh.write(text + "\n")


def on_interrupt(signum, frame):
    os.write(wake_w, b"i")


def on_end(signum, frame):
    os.write(wake_w, b"t")


signal.signal(signal.SIGINT, on_interrupt)
signal.signal(signal.SIGTERM, on_end)
signal.signal(signal.SIGHUP, on_end)
gdb = subprocess.Popen([REAL] + sys.argv[1:], stdin=subprocess.PIPE,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
g_in, g_out, g_err = (gdb.stdin.fileno(), gdb.stdout.fileno(),
                      gdb.stderr.fileno())
state = {"resumes": 0, "holding": False, "held": b"", "late": False}


def answer_of(cmd):
    """gdb's whole answer to a command the stand-in sends itself."""
    os.write(g_in, cmd)
    got = b""
    while not got.endswith(PROMPT):
        b = os.read(g_out, 65536)
        if not b:
            break
        got += b
    return got


def sigint_at_resume(cmd):
    """gdb's report of a SIGINT kept for the program, at this resume."""
    lead = b""
    if cmd == CONTINUE:
        lead = b"Continuing.\n"
    frame = re.sub(rb"^#0  ", b"", answer_of(b"frame\n"))
    return lead + b"\nProgram received signal SIGINT, Interrupt.\n" + frame


def took_sigint():
    note("SIGINT")
    held = state["held"]
    if not state["holding"] or not held.endswith(PROMPT):
        os.kill(gdb.pid, signal.SIGINT)
        state["holding"] = False
        os.write(1, held)
        state["held"] = b""
        return
    state["holding"] = False
    state["held"] = b""
    if MODE == "quit-same":
        os.write(2, QUIT)
        os.write(1, held + PROMPT)
    elif MODE == "quit":
        os.write(1, held)
        time.sleep(SEPARATE_S)
        os.write(2, QUIT)
        os.write(1, PROMPT)
    else:
        os.write(1, held)
        state["late"] = True
    note("stop won: " + MODE)


def from_editor(line):
    cmd = line.decode("utf-8", "replace").rstrip("\n")
    note("CMD " + cmd)
    if RESUME.match(cmd) and state["late"]:
        state["late"] = False
        note("SIGINT reported at the resume")
        os.write(1, sigint_at_resume(cmd))
        return
    if RESUME.match(cmd):
        state["resumes"] += 1
        state["holding"] = state["resumes"] == 1
    os.write(g_in, line)


def from_gdb(data):
    if state["holding"] and not state["held"] and b"Continuing.\n" in data:
        head, tail = data.split(b"Continuing.\n", 1)
        os.write(1, head + b"Continuing.\n")
        state["held"] = tail
    elif state["holding"]:
        state["held"] += data
    else:
        os.write(1, data)


pending = b""
fds = [0, g_out, g_err, wake_r]
while True:
    try:
        ready = select.select(fds, [], [])[0]
    except InterruptedError:
        continue
    if wake_r in ready:
        for c in os.read(wake_r, 64):
            if c == ord("i"):
                took_sigint()
            else:
                gdb.kill()
                sys.exit(0)
    if 0 in ready:
        data = os.read(0, 65536)
        if not data:
            gdb.stdin.close()
            fds.remove(0)
        pending += data
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            from_editor(line + b"\n")
    if g_err in ready:
        data = os.read(g_err, 65536)
        if data:
            os.write(2, data)
        else:
            fds.remove(g_err)
    if g_out in ready:
        data = os.read(g_out, 65536)
        if not data:
            gdb.wait()
            sys.exit(0)
        from_gdb(data)
'''.lstrip()

# The loop's own breakpoint is on its first line; the program raises SIGINT
# by itself once, on its sixth pass.
GDB_HELD_RACE_PROG = (
    "#include <signal.h>\n"
    "int main(void)\n"
    "{\n"
    "  long n = 0;\n"
    "  for (;;) {\n"
    "    n++;\n"
    "    n += 2;\n"
    "    n += 3;\n"
    "    if (n == 36) raise(SIGINT);\n"
    "  }\n"
    "}\n"
)
HELD_LOOP_LINE = 6               # n++;  the loop's breakpoint
HELD_NEXT_LINE = 7               # n += 2;
HELD_SET_LINE = 8                # n += 3;  set while the stop is held
HELD_PASS = 6                    # what one pass adds to n
HELD_STILL_S = 1.5               # a stopped program does not move meanwhile
GDB_SIGNAL_REPORT = "received signal SIGINT"


def _gdb_stand_in(tmp_path, mode):
    """The environment that puts the stand-in gdb, in @mode, first on PATH;
    it logs the commands it is given in stand-in.log."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    stub = bindir / "gdb"
    stub.write_text("#!%s\n%s" % (sys.executable, GDB_STAND_IN_SOURCE))
    stub.chmod(0o755)
    return {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
            "XWPE_REAL_GDB": shutil.which("gdb"),
            "XWPE_STAND_IN_MODE": mode,
            "XWPE_STAND_IN_LOG": str(tmp_path / "stand-in.log")}


def _stand_in_log(tmp_path):
    log = tmp_path / "stand-in.log"
    return log.read_text() if log.exists() else ""


def _assert_held_still(w, n, what):
    """The program is stopped, the stop shown as a breakpoint's, and it
    does not move: nothing was run on behind the user's back."""
    w._drain(HELD_STILL_S)
    assert "^Brk Pause" not in _bottom_bar(w), \
        "%s: nothing may run the program on:\n%s" % (what, _screen(w))
    assert _watch_number(w, "n") == n, \
        "%s: n should read %d:\n%s" % (what, n, _screen(w))
    assert GDB_SIGNAL_REPORT not in _screen(w), \
        "%s: the interrupt is not the user's to see:\n%s" % (what, _screen(w))
    assert PAUSED_LINE not in _bottom_row(w), \
        "%s: the stop is no Pause:\n%s" % (what, _screen(w))


def _the_stop_wins_the_race(w, tmp_path):
    """Stop on the loop with a watch on n, Continue (gdb's next stop is
    held), and set a breakpoint: its interrupt finds gdb stopped.  Return
    n at the stop the user is shown."""
    _stop_on_a_breakpoint_at(w, HELD_LOOP_LINE, START_TIMEOUT_S)
    _add_watch(w, "n")
    _source_window_on_top(w)
    assert _wait_for(w, lambda: _watch_number(w, "n") == 0,
                     STEP_TIMEOUT_S), _screen(w)
    w.key(CTRL_G, "r", delay=0.8)
    _go_to_line(w, HELD_SET_LINE)
    w.key(CTRL_G, "b", delay=0.6)
    assert _wait_for(w, lambda: _mark_line(w) == HELD_LOOP_LINE,
                     STEP_TIMEOUT_S), \
        "the breakpoint's stop should be shown:\n%s\n%s" \
        % (_screen(w), _stand_in_log(tmp_path))
    assert "stop won" in _stand_in_log(tmp_path), _stand_in_log(tmp_path)
    _assert_held_still(w, HELD_PASS, "the stop that won")
    _assert_solid(w, HELD_SET_LINE, "set at the stop that won")
    return HELD_PASS


def _resumes_once_from(w, tmp_path, key, line, n):
    """@key from the stop: the program stops on @line with n at @n, and
    gdb was sent one command for it, or two when the first was answered by
    a SIGINT kept from the race."""
    w.key(*key, delay=0.5)
    assert _wait_for(w, lambda: _mark_line(w) == line and
                     _watch_number(w, "n") == n, STEP_TIMEOUT_S), \
        "the program should stop on line %d with n at %d:\n%s\n%s" \
        % (line, n, _screen(w), _stand_in_log(tmp_path))
    _assert_held_still(w, n, "on line %d" % line)


def _its_own_sigint_is_shown(w, tmp_path):
    """With every breakpoint off, Continue: the SIGINT the program raises
    itself is the user's stop, not a left-over interrupt to run past."""
    for line in (HELD_LOOP_LINE, HELD_SET_LINE):
        _go_to_line(w, line)
        w.key(CTRL_G, "b", delay=0.6)
    assert not _breakpoint_lines(w), _screen(w)
    before = _stand_in_log(tmp_path)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: GDB_SIGNAL_REPORT in _screen(w),
                     STEP_TIMEOUT_S), \
        "the program's own SIGINT should stop it:\n%s\n%s" \
        % (_screen(w), _stand_in_log(tmp_path))
    sent = _stand_in_log(tmp_path)[len(before):]
    assert sent.count("CMD c\n") == 1, \
        "that stop must not be run past:\n%s" % sent
    w.key("\r", delay=0.8)


# @scenario SC-debug-07
@pytest.mark.parametrize("mode", ["quit", "quit-same"])
def test_an_interrupt_answered_at_gdbs_prompt_is_taken_back(tmp_path, mode):
    """The program reaches its breakpoint just before the interrupt a
    toggle sends, and gdb answers that interrupt at its prompt with "Quit"
    and one more prompt, in a read of its own or with the stop.  The stop
    is the user's and nothing runs on; every later answer belongs to its
    own command (F8, Continue, the watch); and a SIGINT the program raises
    itself later is shown, not taken for that interrupt."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_HELD_RACE_PROG, filename="p.c",
                    wait=1.5, env_extra=_gdb_stand_in(tmp_path, mode)) as w:
        n = _the_stop_wins_the_race(w, tmp_path)
        _resumes_once_from(w, tmp_path, (F8,), HELD_NEXT_LINE, n + 1)
        _resumes_once_from(w, tmp_path, (F8,), HELD_SET_LINE, n + 3)
        _resumes_once_from(w, tmp_path, (CTRL_G, "r"), HELD_LOOP_LINE,
                           n + HELD_PASS)
        _its_own_sigint_is_shown(w, tmp_path)


# @scenario SC-debug-07
@pytest.mark.parametrize("key", ["F8", "Continue"])
def test_an_interrupt_gdb_reports_at_the_next_resume_is_not_shown(tmp_path,
                                                                   key):
    """The program reaches its breakpoint just before the interrupt a
    toggle sends, and gdb keeps that SIGINT for the program: it reports it
    at the next F8 or Continue, before the program moved.  The user asked
    for the step or the Continue, not for that stop: the command is sent
    again and the program stops where it would have."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_HELD_RACE_PROG, filename="p.c",
                    wait=1.5, env_extra=_gdb_stand_in(tmp_path, "late")) as w:
        n = _the_stop_wins_the_race(w, tmp_path)
        if key == "F8":
            _resumes_once_from(w, tmp_path, (F8,), HELD_NEXT_LINE, n + 1)
            sent = "CMD n\n"
        else:
            _resumes_once_from(w, tmp_path, (CTRL_G, "r"), HELD_SET_LINE,
                               n + 3)
            sent = "CMD c\n"
        log = _stand_in_log(tmp_path)
        assert "SIGINT reported at the resume" in log, log
        assert log.split("stop won")[1].count(sent) == 2, \
            "the command should have gone out again once:\n%s" % log


PY_OWN_SIGINT_SLEEP_S = 6        # room to move the cursor and toggle
# pdb puts its own SIGINT handler back at every continue, so the script sets
# its handler while it runs, as a program that installs one late does.
PY_OWN_SIGINT_PROG = (
    "import signal, time\n"
    "def own(sig, frame):\n"
    "    print('own handler', flush=True)\n"
    "n = 0\n"
    "while True:\n"
    "    signal.signal(signal.SIGINT, own)\n"
    "    time.sleep(%d)\n"
    "    n += 1\n"
    "    m = n\n" % PY_OWN_SIGINT_SLEEP_S
)
PY_OWN_SIGINT_START_LINE = 4      # n = 0
PY_OWN_SIGINT_TOGGLE_LINE = 8     # n += 1, marked while it sleeps
PY_OWN_SIGINT_STOP_LINE = 9       # m = n, armed: the next stop
OWN_SIGINT_WORDS = "handles Ctrl-C itself"


# @scenario SC-debug-07
def test_pdb_falls_back_to_the_next_stop_when_the_program_handles_sigint(
        tmp_path):
    """A script with its own SIGINT handler is not stopped by the interrupt:
    the breakpoint set while it runs is applied at its next stop, and the
    status row says why it did not take effect at once."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_OWN_SIGINT_PROG, filename="p.py",
                    wait=1.5) as w:
        _go_to_line(w, PY_OWN_SIGINT_STOP_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        _stop_on_a_breakpoint_at(w, PY_OWN_SIGINT_START_LINE,
                                 START_TIMEOUT_S)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == PY_OWN_SIGINT_STOP_LINE,
                         LIVE_STOP_TIMEOUT_S), _screen(w)
        w.key(CTRL_G, "r", delay=0.3)    # into the sleep
        _go_to_line(w, PY_OWN_SIGINT_TOGGLE_LINE)
        w.key(CTRL_G, "b", delay=0.6)
        assert _mark_line(w) is None, \
            "the program should still sleep, unmarked:\n%s" % _screen(w)
        assert _wait_for(w, lambda: _mark_line(w) == PY_OWN_SIGINT_STOP_LINE,
                         LIVE_STOP_TIMEOUT_S), \
            "the program should stop at its next breakpoint (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        assert _wait_for(w, lambda: OWN_SIGINT_WORDS in _bottom_row(w),
                         HOLLOW_WAIT_S), \
            "the status row should say the program handles Ctrl-C:\n%s" \
            % _screen(w)
        _assert_solid(w, PY_OWN_SIGINT_TOGGLE_LINE, "at that stop")
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) ==
                         PY_OWN_SIGINT_TOGGLE_LINE, LIVE_STOP_TIMEOUT_S), \
            "the next pass should stop on line %d (mark on %s):\n%s" \
            % (PY_OWN_SIGINT_TOGGLE_LINE, _mark_line(w), _screen(w))


XWPERC_DEFAULT_SCHEME = "[Color/Term]\net : 11 4\ndb : 0 1\n"
XWPERC_WITH_DU = "[Color/Term]\net : 11 4\ndb : 0 1\ndu : 13 4\n"
HOLLOW_DEFAULT = ("red", False, "blue")     # du = 1 on 4
HOLLOW_SEEDED = ("magenta", True, "blue")   # du = 13 on 4


def _write_xwperc(home, text):
    d = os.path.join(home, ".config", "xwpe")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "xwperc"), "w") as fh:
        fh.write(text)


def _hollow_cell(w, line):
    """(fg, bold, bg) of the first text cell of the hollow @line."""
    row = w.screen.buffer[line + 1]
    for x in range(1, w.screen.columns - 1):
        if row[x].data.strip():
            return (row[x].fg, row[x].bold, row[x].bg)
    return None


def _hollow_line_of_a_running_gdb(w, fg=UNCONFIRMED_BREAKPOINT_FG):
    """Step over gdb's program's long sleep, set a breakpoint on its tail
    line while that step runs, and return that line: hollow, in the text
    colour @fg, since a change made during a step waits for its stop."""
    case = BRK_RUN_CASES["gdb"]
    _stop_on_a_breakpoint_at(w, case["break"], case["start_s"])
    for line in range(case["break"] + 1, GDB_SLEEP_LINE + 1):
        assert _step(w, F8, line), \
            "F8 should mark line %d (mark on %s):\n%s" \
            % (line, _mark_line(w), _screen(w))
    w.key(F8, delay=0.5)                     # over sleep(600)
    _wait_for_the_pause_bar(w)
    _go_to_line(w, case["tail"])
    w.key(CTRL_G, "b", delay=0.6)
    _assert_hollow(w, case["tail"], "while the program runs", fg)
    return case["tail"]


# @scenario SC-debug-07
def test_an_xwperc_without_du_keeps_the_default_hollow_colour(tmp_path):
    """A colour file that never heard of the unconfirmed-breakpoint entry
    leaves its default: the bar's red as the text colour on the editor's
    blue."""
    _need("gcc", "gdb")
    _write_xwperc(str(tmp_path), XWPERC_DEFAULT_SCHEME)
    with WpeSession(str(tmp_path), GDB_SLEEP_PROG, filename="p.c",
                    wait=1.5) as w:
        line = _hollow_line_of_a_running_gdb(w)
        assert _hollow_cell(w, line) == HOLLOW_DEFAULT, \
            "the hollow line should be red on blue, got %r:\n%s" \
            % (_hollow_cell(w, line), _screen(w))
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-07
def test_du_in_xwperc_recolours_the_hollow_breakpoint(tmp_path):
    """The du entry of a colour file sets the unconfirmed breakpoint's
    colours."""
    _need("gcc", "gdb")
    _write_xwperc(str(tmp_path), XWPERC_WITH_DU)
    with WpeSession(str(tmp_path), GDB_SLEEP_PROG, filename="p.c",
                    wait=1.5) as w:
        line = _hollow_line_of_a_running_gdb(w, HOLLOW_SEEDED[0])
        assert _hollow_cell(w, line) == HOLLOW_SEEDED, \
            "the hollow line should follow du, got %r:\n%s" \
            % (_hollow_cell(w, line), _screen(w))
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-07
def test_a_breakpoint_set_while_running_turns_solid_while_it_runs_under_scala(
        tmp_path):
    """The same under the Scala adapter."""
    _need("scala-cli")
    workdir = str(tmp_path)
    try:
        with WpeSession(workdir, SCALA_PAUSE_PROG, filename="p.scala",
                        wait=2.0) as w:
            _run_into_the_program(w, SCALA_PAUSE_LINES[0],
                                  SCALA_START_TIMEOUT_S)
            _go_to_line(w, SCALA_TAIL_LINE)
            w.key(CTRL_G, "b", delay=0.6)
            _assert_solid(w, SCALA_TAIL_LINE, "while the program runs")
            w.key(*CTRL_G_I, delay=0.5)
            assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                             PAUSE_TIMEOUT_S), \
                "Ctrl-G I should stop the program:\n%s" % _screen(w)
            _assert_solid(w, SCALA_TAIL_LINE, "after the stop")
            _quit_ends_the_scala_program(w, workdir)
    finally:
        for pid in procinfo.processes_in(workdir, JVM_COMM):
            os.kill(pid, signal.SIGKILL)


SCALA_TAIL_LINE = 7        # println(n), past the loop


# @scenario SC-debug-07
def test_a_breakpoint_set_while_running_stops_the_program_there_under_scala(
        tmp_path):
    """The same as the other adapters' row under the Scala adapter: a mark
    set on the loop's body while the program runs stops it there.  The
    session ends with Debug > Quit, leaving no program behind."""
    _need("scala-cli")
    workdir = str(tmp_path)
    body = SCALA_PAUSE_LINES[1][1]          # n += 1
    try:
        with WpeSession(workdir, SCALA_PAUSE_PROG, filename="p.scala",
                        wait=2.0) as w:
            _run_into_the_program(w, SCALA_PAUSE_LINES[0],
                                  SCALA_START_TIMEOUT_S)
            _go_to_line(w, body)
            w.key(CTRL_G, "b", delay=0.6)
            _assert_stops_on(w, body, "under Scala")
            _quit_ends_the_scala_program(w, workdir)
    finally:
        for pid in procinfo.processes_in(workdir, JVM_COMM):
            os.kill(pid, signal.SIGKILL)


# ---- watches changed while the program runs ----
#
# The debugger reads no command while the program runs, so a watch added,
# edited or deleted then is listed at once and read at the next stop; the
# stop the editor makes to apply a breakpoint is not such a stop.

WATCH_LATER_WORDS = "(at the next stop)"
WATCH_EDITED_EXPR = "n+1"
WATCH_LIVE_SHOWN_S = 5            # the list changes at once, not at a stop
WATCH_EDIT_KEYS = (CTRL_G, "e")
WATCH_DELETE_KEYS = (CTRL_G, "d")
WATCH_REMOVE_ALL_KEYS = (CTRL_G, "a")
WATCH_ADAPTER_CASE = "lldb-dap"
# Edit and Delete on a line debugger and two adapters of different makers;
# Remove All on gdb, pdb (its own interrupt) and an adapter.
WATCH_EDIT_CASES = ("gdb", WATCH_ADAPTER_CASE, "dlv")
WATCH_REMOVE_ALL_CASES = ("gdb", "pdb", WATCH_ADAPTER_CASE)


def _watch_waits_for_the_stop(w, expr):
    """The Watches window lists @expr with the words for a value to come."""
    return _wait_for(w, lambda: _watch_is_waiting(w, expr),
                     WATCH_LIVE_SHOWN_S)


def _watch_is_waiting(w, expr):
    """A row of the screen lists @expr with the words for a value to come."""
    words = "%s: %s" % (expr, WATCH_LATER_WORDS)
    return any(words in row for row in w.display(drain=0))


def _watch_has_a_value(w, expr):
    """The Watches window (the rows below its title) shows @expr with a
    value, that is with any text but the words for a value to come."""
    pat = re.compile(r"^%s: (\S.*)$" % re.escape(expr))
    below = False
    for row in w.display(drain=0):
        if " Watches " in row:
            below = True
            continue
        m = pat.match(row[1:].rstrip(" x^av"))
        if below and m:
            return m.group(1) != WATCH_LATER_WORDS
    return False


def _pause_and_see_the_watch_filled(w, expr):
    """Pause the program; the stop reads @expr."""
    w.key(*CTRL_G_I, delay=0.5)
    assert _wait_for(w, lambda: _mark_line(w) is not None, PAUSE_TIMEOUT_S), \
        "Ctrl-G I should stop the program:\n%s" % _screen(w)
    _watches_on_top(w)
    assert _wait_for(w, lambda: _watch_has_a_value(w, expr),
                     STEP_TIMEOUT_S), \
        "the stop should fill the watch on %s:\n%s" % (expr, _screen(w))


# @scenario SC-debug-10
@pytest.mark.parametrize("debugger", sorted(BRK_LIVE_CASES))
def test_a_watch_added_while_running_shows_at_the_next_stop(tmp_path,
                                                            debugger):
    """A watch made while the program runs is not refused: it is listed at
    once, saying its value comes at the next stop, and a Pause then fills
    it."""
    case = BRK_LIVE_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _add_watch(w, "n")
        assert RUNNING_REFUSAL not in _screen(w), \
            "a watch is no debugger command to refuse:\n%s" % _screen(w)
        assert _watch_waits_for_the_stop(w, "n"), \
            "the watch should wait for the next stop (row %r):\n%s" \
            % (_watch_row(w, "n"), _screen(w))
        _pause_and_see_the_watch_filled(w, "n")
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-10
@pytest.mark.parametrize("debugger", WATCH_EDIT_CASES)
def test_a_watch_edited_and_deleted_while_running(tmp_path, debugger):
    """The entry of a running program's watch is edited (its new text shows
    the words for a value to come) and deleted (it leaves the list), with the
    program still running."""
    case = BRK_LIVE_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _add_watch(w, "n")
        assert _watch_waits_for_the_stop(w, "n"), _screen(w)
        w.key(*WATCH_EDIT_KEYS, delay=0.8)
        w.key(WATCH_EDITED_EXPR, delay=0.2)
        w.key("\r", delay=0.5)
        assert RUNNING_REFUSAL not in _screen(w), _screen(w)
        assert _watch_waits_for_the_stop(w, WATCH_EDITED_EXPR), \
            "the edited watch should wait for the next stop:\n%s" \
            % _screen(w)
        w.key(*WATCH_DELETE_KEYS, delay=0.5)
        assert RUNNING_REFUSAL not in _screen(w), _screen(w)
        assert _wait_for(w, lambda: not _watch_is_waiting(w, WATCH_EDITED_EXPR),
                         WATCH_LIVE_SHOWN_S), \
            "the deleted watch should leave the list:\n%s" % _screen(w)
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-10
@pytest.mark.parametrize("debugger", WATCH_REMOVE_ALL_CASES)
def test_all_watches_removed_while_running(tmp_path, debugger):
    """Remove All watches closes the Watches window of a running program,
    and the next stop opens none."""
    case = BRK_LIVE_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _add_watch(w, "n")
        assert _watch_waits_for_the_stop(w, "n"), _screen(w)
        w.key(*WATCH_REMOVE_ALL_KEYS, delay=0.6)
        assert RUNNING_REFUSAL not in _screen(w), _screen(w)
        assert _wait_for(w, lambda: " Watches " not in _screen(w),
                         WATCH_LIVE_SHOWN_S), \
            "Remove All should close the Watches window:\n%s" % _screen(w)
        w.key(*CTRL_G_I, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) is not None,
                         PAUSE_TIMEOUT_S), _screen(w)
        assert " Watches " not in _screen(w), \
            "no Watches window should come back:\n%s" % _screen(w)
        _quit_and_see_the_session_gone(w)


# gdb answers the watch's "p n" with a value, or, stopped where n is not in
# scope (the hidden stop lands inside sleep), with "No symbol"; both count.
GDB_WATCH_ANSWER = re.compile(
    r'\(gdb\) (?:\$\d+ = |[^\n]*No symbol "n" in current context)')


def _gdb_watch_answers(tmp_path):
    """How many times gdb has answered the watch's "p n" so far."""
    log = tmp_path / GDB_LOG_NAME
    if not log.exists():
        return 0
    return len(GDB_WATCH_ANSWER.findall(log.read_text(errors="replace")))


# @scenario SC-debug-10
def test_a_sync_stop_evaluates_no_watch(tmp_path):
    """The stop the editor makes to tell gdb a breakpoint set while the
    program runs is hidden: it reads no watch, and the window shows nothing
    new until the program stops for the user."""
    _need("gcc", "gdb")
    (tmp_path / ".gdbinit").write_text(
        GDB_LOG_INIT % (tmp_path / GDB_LOG_NAME))
    case = BRK_LIVE_CASES["gdb"]
    with WpeSession(str(tmp_path), GDB_LIVE_PROG, filename="p.c",
                    wait=1.5) as w:
        _go_to_line(w, case["break"])
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == case["break"],
                         case["start_s"]), _screen(w)
        _add_watch(w, "n")
        _source_window_on_top(w)
        w.key(CTRL_G, "r", delay=0.5)
        _wait_for_the_pause_bar(w)
        w._drain(PAUSE_SETTLE_S)
        before = _gdb_watch_answers(tmp_path)
        assert before >= 1, "the watch should have been read once"
        _go_to_line(w, GDB_TWO_TOGGLE_LINES[0])
        w.key(CTRL_G, "b", delay=0.6)
        assert _wait_for(w, lambda: _mark_line(w) in GDB_TWO_TOGGLE_LINES,
                         LIVE_STOP_TIMEOUT_S), _screen(w)
        assert _gdb_sigint_reports(tmp_path) == 1, \
            "the breakpoint should have cost one interrupt:\n%s" % _screen(w)
        w._drain(PAUSE_SETTLE_S)
        assert _gdb_watch_answers(tmp_path) == before + 1, \
            "the user's stop reads the watch once, the sync stop never " \
            "(gdb answered %d times, %d before):\n%s" \
            % (_gdb_watch_answers(tmp_path), before, _screen(w))
        _quit_and_see_the_session_gone(w)


# Programs that end 8 s after their first stop, so a watch added
# while they run is still waiting when they end.
GDB_ENDING_PROG = (
    "#include <stdio.h>\n"
    "#include <unistd.h>\n"
    "int main(void)\n"
    "{\n"
    "  long n = 0;\n"
    "  printf(\"READY\\n\");\n"
    "  fflush(stdout);\n"
    "  for (n = 0; n < 8; n++)\n"
    "    sleep(1);\n"
    "  return 0;\n"
    "}\n"
)
RUST_ENDING_PROG = (
    "fn main() {\n"
    "    let n: u64 = 7;\n"
    "    println!(\"ready\");\n"
    "    std::thread::sleep(std::time::Duration::from_secs(8));\n"
    "    println!(\"{}\", n);\n"
    "}\n"
)
WATCH_END_CASES = {
    "gdb": dict(BRK_LIVE_CASES["gdb"], prog=GDB_ENDING_PROG),
    WATCH_ADAPTER_CASE: dict(BRK_LIVE_CASES[WATCH_ADAPTER_CASE],
                             prog=RUST_ENDING_PROG),
}
PROGRAM_END_S = 20                # the program's 8 s, the debugger's end
WATCH_NO_SESSION_WORDS = "No symbol in current context"   # any watch, no session


def _watch_window_value(w, expr):
    """The text after "@expr: " in the Watches window (below its title, so
    not a source line that reads the same), or None."""
    pat = re.compile(r"^%s: (\S.*)$" % re.escape(expr))
    below = False
    for row in w.display(drain=0):
        if " Watches " in row:
            below = True
            continue
        m = pat.match(row[1:].rstrip(" x^av"))
        if below and m:
            return m.group(1)
    return None


# @scenario SC-debug-10
@pytest.mark.parametrize("debugger", sorted(WATCH_END_CASES))
def test_a_waiting_watch_says_no_next_stop_when_the_program_ends(tmp_path,
                                                                debugger):
    """A watch added while the program runs, which then ends, does not keep
    promising a value "at the next stop": there is none.  It shows what a
    watch shows without a session."""
    case = WATCH_END_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _add_watch(w, "n")
        assert _watch_waits_for_the_stop(w, "n"), _screen(w)
        _source_window_on_top(w)
        assert _wait_for(w, lambda: "^Brk Pause" not in _bottom_bar(w),
                         PROGRAM_END_S), \
            "the program should have ended:\n%s" % _screen(w)
        w._drain(PAUSE_SETTLE_S)
        _watches_on_top(w)
        assert not _watch_is_waiting(w, "n"), \
            "the program ended; the watch must not wait for a stop " \
            "(row %r):\n%s" % (_watch_window_value(w, "n"), _screen(w))
        assert _watch_window_value(w, "n") == WATCH_NO_SESSION_WORDS, \
            "the watch should read %r without a session:\n%s" \
            % (WATCH_NO_SESSION_WORDS, _screen(w))


WATCH_UNDEFINED_NAME = "nosuchname"   # no variable of C_PROG is called this


# @scenario SC-debug-10
def test_a_watch_on_an_undefined_name_under_gdb_says_no_symbol(tmp_path):
    """gdb answers a print of a name the program does not have without an
    "=": the watch reads the editor's whole "No symbol in current context",
    its first letter included."""
    _need("gcc", "gdb")
    with _c_session(tmp_path) as w:
        _start_c_session(w)
        _add_watch(w, WATCH_UNDEFINED_NAME)
        assert _wait_for(
            w, lambda: _watch_window_value(w, WATCH_UNDEFINED_NAME)
            is not None, STEP_TIMEOUT_S), \
            "the Watches window should list the watch:\n%s" % _screen(w)
        assert _watch_window_value(w, WATCH_UNDEFINED_NAME) == \
            WATCH_NO_SESSION_WORDS, \
            "the watch should read %r:\n%s" \
            % (WATCH_NO_SESSION_WORDS, _screen(w))


# @scenario SC-debug-10
def test_a_watch_added_while_running_shows_at_the_next_stop_under_scala(
        tmp_path):
    """The same under the Scala adapter."""
    _need("scala-cli")
    workdir = str(tmp_path)
    try:
        with WpeSession(workdir, SCALA_PAUSE_PROG, filename="p.scala",
                        wait=2.0) as w:
            _run_into_the_program(w, SCALA_PAUSE_LINES[0],
                                  SCALA_START_TIMEOUT_S)
            _add_watch(w, "n")
            assert _watch_waits_for_the_stop(w, "n"), _screen(w)
            _pause_and_see_the_watch_filled(w, "n")
            _quit_ends_the_scala_program(w, workdir)
    finally:
        for pid in procinfo.processes_in(workdir, JVM_COMM):
            os.kill(pid, signal.SIGKILL)


# @scenario SC-debug-07
def test_the_colours_dialog_samples_the_unconfirmed_breakpoint(tmp_path):
    """Options > Adjust Colors > Editor Colors shows a sample line for the
    unconfirmed breakpoint below the confirmed one's: red text on the
    editor's blue, where the confirmed sample is the solid red bar."""
    with WpeSession(str(tmp_path), C_PROG, filename="p.c", wait=1.5) as w:
        w.menu(ALT.OPTIONS, "a", delay=0.8)
        w.key("\r", delay=0.8)
        disp = w.display()
        rows = {name: y for y, line in enumerate(disp)
                for name in ("This is a Breakpoint", "An unconfirmed")
                if name in line}
        assert len(rows) == 2, \
            "the dialog should show both samples:\n%s" % "\n".join(disp)
        solid = w.screen.buffer[rows["This is a Breakpoint"]][
            disp[rows["This is a Breakpoint"]].index("This")]
        hollow = w.screen.buffer[rows["An unconfirmed"]][
            disp[rows["An unconfirmed"]].index("An unconfirmed")]
        assert solid.bg == "red", \
            "the confirmed sample should be the red bar, got %r" % solid.bg
        assert (hollow.fg, hollow.bg) == ("red", "blue"), \
            "the unconfirmed sample should be red on blue, got %r on %r" \
            % (hollow.fg, hollow.bg)


# A Java program that spins on one line and prints a tick now and then: a
# breakpoint on JDB_HOT_BODY_LINE is reached again within microseconds, and
# the ticks growing in Messages show the program was never held.
JDB_HOT_BREAK_LINE = 4            # System.out.println("ready");
JDB_HOT_BODY_LINE = 6             # n++;
JDB_HOT_TICK_EVERY = 20000000
JDB_HOT_PROG = (
    "public class P {\n"
    "    public static void main(String[] a) throws Exception {\n"
    "        long n = 0;\n"
    "        System.out.println(\"ready\");\n"
    "        while (true) {\n"
    "            n++;\n"
    "            if (n %% %dL == 0) {\n"
    "                System.out.println(\"tick \" + n);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "}\n" % JDB_HOT_TICK_EVERY
)
JDB_HOT_CASE = dict(BRK_LIVE_CASES["jdb"], prog=JDB_HOT_PROG,
                    break_at=JDB_HOT_BREAK_LINE, body=JDB_HOT_BODY_LINE)
JDB_TICK_RE = re.compile(r"^tick (\d+)$")
JDB_GATE_INPUT_LOG = "jdb-input.log"
JDB_GATE_BIN_DIR = "gatejdb"
JDB_BATCH_WATCH_S = 4             # a stop on the line would have come by now
JDB_TICKS_WATCH_S = 3             # ticks must have grown over this time
JDB_REAL_ROUNDS = 8               # on and off again, with the real jdb
JDB_REAL_GAP_S = 0.0              # between the two writes of a round

# A jdb that runs the real one and records every line the editor sends it.
# With HOLD set it also acts the way a busy VM does: once a "stop at" for
# the hot line has gone in, jdb's report of the hit is kept back until the
# editor's "clear" has been read, so the hit always comes before the clear
# is taken.
FAKE_JDB_GATE = r'''#!%(python)s -I
import os, subprocess, sys, threading
REAL = %(real)r
LOG = %(log)r
HOLD = %(hold)r
GATE_LINE = ":%(line)d"
HIT = b"Breakpoint hit"
GATE_WAIT_S = 30
child = subprocess.Popen([REAL] + sys.argv[1:], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         bufsize=0)
lock = threading.Lock()
held = []
state = {"armed": False, "holding": False}


def emit(data):
    os.write(1, data)


def release():
    with lock:
        state["holding"] = False
        state["armed"] = False
        data = b"".join(held)
        del held[:]
    if data:
        emit(data)


def pump():
    while True:
        data = os.read(child.stdout.fileno(), 4096)
        if not data:
            break
        with lock:
            if state["holding"]:
                held.append(data)
                continue
            at = data.find(HIT)
            if HOLD and state["armed"] and at >= 0:
                state["holding"] = True
                if at:
                    emit(data[:at])
                held.append(data[at:])
                threading.Timer(GATE_WAIT_S, release).start()
                continue
        emit(data)
    os._exit(child.wait())


threading.Thread(target=pump, daemon=True).start()
while True:
    raw = sys.stdin.buffer.readline()
    if not raw:
        break
    text = raw.decode(errors="replace")
    with open(LOG, "a") as log:
        log.write(text)
    if text.startswith("stop at ") and text.rstrip().endswith(GATE_LINE):
        state["armed"] = True
    child.stdin.write(raw)
    if text.startswith("clear "):
        release()
child.stdin.close()
os._exit(child.wait())
'''


def _gate_jdb_env(tmp_path, hold):
    """The environment of a session whose jdb is FAKE_JDB_GATE, logging
    what the editor sends to JDB_GATE_INPUT_LOG, and keeping back a hit
    until its clear when @hold."""
    bindir = tmp_path / JDB_GATE_BIN_DIR
    bindir.mkdir()
    _write_script(bindir / "jdb", FAKE_JDB_GATE % {
        "python": sys.executable, "real": shutil.which("jdb"),
        "log": str(tmp_path / JDB_GATE_INPUT_LOG), "hold": hold,
        "line": JDB_HOT_BODY_LINE})
    return {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}


def _gate_jdb_session(tmp_path, hold):
    """The editor on JDB_HOT_PROG with the gate in front of jdb."""
    _need("jdb", "javac")
    return WpeSession(str(tmp_path), JDB_HOT_PROG, filename="P.java",
                      wait=1.5, env_extra=_gate_jdb_env(tmp_path, hold))


def _jdb_inputs(tmp_path):
    """The lines the editor has sent jdb so far."""
    log = tmp_path / JDB_GATE_INPUT_LOG
    if not log.exists():
        return []
    return log.read_text(errors="replace").splitlines()


def _last_tick(w):
    """The highest tick the program has printed that Messages shows."""
    ticks = [int(m.group(1)) for m in
             (JDB_TICK_RE.match(line) for line in _messages_lines(w)) if m]
    return max(ticks) if ticks else 0


def _assert_the_program_runs_on(w, what):
    """No stop was shown and the program's ticks keep growing."""
    before = _last_tick(w)
    assert _never_marked_at(w, JDB_HOT_BODY_LINE, JDB_TICKS_WATCH_S), \
        "%s: the program must not stop on line %d:\n%s" \
        % (what, JDB_HOT_BODY_LINE, _screen(w))
    assert PAUSED_LINE not in _bottom_row(w), \
        "%s: nothing should be shown as paused:\n%s" % (what, _screen(w))
    assert "^Brk Pause" in _bottom_bar(w), \
        "%s: the program should still run:\n%s" % (what, _screen(w))
    assert _last_tick(w) > before, \
        "%s: the program's ticks stopped at %d:\n%s" \
        % (what, before, _screen(w))


# @scenario SC-debug-07
def test_a_jdb_mark_set_and_removed_in_one_batch_sends_nothing(tmp_path):
    """Two toggles of one line typed together while the program runs reach
    jdb as their net change, which is none: it is sent no "stop at" for the
    line, so a loop passing it a million times a second cannot stop there
    before the removal follows."""
    case = JDB_HOT_CASE
    with _gate_jdb_session(tmp_path, hold=False) as w:
        _run_into_the_program(w, case["break_at"], case["start_s"])
        _go_to_line(w, case["body"])
        w.key(CTRL_G + "b" + CTRL_G + "b", delay=0.6)
        assert case["body"] not in \
            _breakpoint_lines(w) + _unconfirmed_lines(w), \
            "toggled twice, the line should be unmarked:\n%s" % _screen(w)
        w._drain(JDB_BATCH_WATCH_S)
        assert not [c for c in _jdb_inputs(tmp_path)
                    if c.startswith("stop at ") and
                    c.endswith(":%d" % case["body"])], \
            "jdb must not be sent the line's stop at:\n%s" \
            % "\n".join(_jdb_inputs(tmp_path))
        _assert_the_program_runs_on(w, "a mark set and removed in a batch")
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-07
@pytest.mark.parametrize("debugger", ["gdb", "dlv"])
def test_a_mark_set_and_removed_in_one_batch_arms_nothing_under(tmp_path,
                                                                debugger):
    """The same two toggles in one batch under gdb and an adapter: the
    program keeps running and never stops on the line."""
    case = BRK_LIVE_CASES[debugger]
    with _live_session(tmp_path, case) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _go_to_line(w, case["body"])
        w.key(CTRL_G + "b" + CTRL_G + "b", delay=0.6)
        assert case["body"] not in \
            _breakpoint_lines(w) + _unconfirmed_lines(w), \
            "toggled twice, the line should be unmarked:\n%s" % _screen(w)
        assert _never_marked_at(w, case["body"], JDB_BATCH_WATCH_S), \
            "the program must not stop on line %d:\n%s" \
            % (case["body"], _screen(w))
        assert "^Brk Pause" in _bottom_bar(w), _screen(w)
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-07
def test_a_gdb_mark_set_and_removed_in_one_batch_costs_no_interrupt(tmp_path):
    """gdb is interrupted to be told of a breakpoint; two toggles that
    cancel in one batch need no interrupt, so it reports no SIGINT stop."""
    _need("gcc", "gdb")
    (tmp_path / ".gdbinit").write_text(
        GDB_LOG_INIT % (tmp_path / GDB_LOG_NAME))
    case = BRK_LIVE_CASES["gdb"]
    with WpeSession(str(tmp_path), GDB_LIVE_PROG, filename="p.c",
                    wait=1.5) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _go_to_line(w, case["body"])
        w.key(CTRL_G + "b" + CTRL_G + "b", delay=0.6)
        w._drain(PAUSE_SETTLE_S)
        assert _gdb_sigint_reports(tmp_path) == 0, \
            "the toggles cancel: gdb should not be interrupted:\n%s" \
            % _screen(w)
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-07
def test_a_jdb_stale_breakpoint_hit_is_not_shown(tmp_path):
    """A mark set, and removed in a later batch, while the program spins on
    its line: jdb reports the hit before it has taken the clear, and the
    hit is not shown.  No mark, no Paused, and the program goes on."""
    case = JDB_HOT_CASE
    with _gate_jdb_session(tmp_path, hold=True) as w:
        _run_into_the_program(w, case["break_at"], case["start_s"])
        _go_to_line(w, case["body"])
        w.key(CTRL_G + "b", CTRL_G + "b", delay=0.3)
        assert case["body"] not in \
            _breakpoint_lines(w) + _unconfirmed_lines(w), \
            "toggled twice, the line should be unmarked:\n%s" % _screen(w)
        assert [c for c in _jdb_inputs(tmp_path)
                if c.startswith("stop at ") and
                c.endswith(":%d" % case["body"])], \
            "the two batches should each have gone to jdb:\n%s" \
            % "\n".join(_jdb_inputs(tmp_path))
        _assert_the_program_runs_on(w, "a stale hit")
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-07
def test_a_jdb_stale_hit_is_not_shown_with_the_real_jdb(tmp_path):
    """The two toggles written back to back, JDB_REAL_ROUNDS times, with
    the real jdb and nothing held back: the user is never shown a stop on
    the line.  Written this close they reach the editor as one burst and
    jdb is sent nothing for the line; the hit jdb reports before it takes
    a clear is the gate row's case above.  With a pause between the two,
    jdb can stop on the line while it is still marked, and that stop is
    rightly the user's."""
    case = JDB_HOT_CASE
    _need("jdb", "javac")
    with WpeSession(str(tmp_path), JDB_HOT_PROG, filename="P.java",
                    wait=1.5) as w:
        _run_into_the_program(w, case["break_at"], case["start_s"])
        _go_to_line(w, case["body"])
        for round_no in range(JDB_REAL_ROUNDS):
            w.key(CTRL_G + "b", delay=JDB_REAL_GAP_S)
            w.key(CTRL_G + "b", delay=0.3)
            assert _mark_line(w) != case["body"], \
                "round %d: a stop was shown on the removed line:\n%s" \
                % (round_no, _screen(w))
        _assert_the_program_runs_on(w, "on and off again")
        _quit_and_see_the_session_gone(w)


# jdb can write its answer to a "stop at" sent while the program runs into
# the middle of its report of the hit, with a prompt before the place:
#     Breakpoint hit: Set breakpoint P:6
#     main[1] "thread=main", P.main(), line=6 bci=18
# A jdb that runs the real one and always answers that way, with a pause
# after the first prompt so the editor reads the two parts apart.
JDB_INTERLEAVE_BIN_DIR = "interleavejdb"
JDB_INTERLEAVE_GAP_S = 0.5
FAKE_JDB_INTERLEAVE = r'''#!%(python)s -I
import os, subprocess, sys, threading, time
REAL = %(real)r
SET_LINE = ":%(line)d"
GAP_S = %(gap)r
HIT = b"Breakpoint hit: "
PLACE = b'"thread='
PROMPT = b"main[1] "
child = subprocess.Popen([REAL] + sys.argv[1:], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         bufsize=0)
state = {"armed": False, "spec": b""}


def pump():
    kept = b""
    while True:
        data = os.read(child.stdout.fileno(), 4096)
        if not data:
            break
        if not state["armed"] and not kept:
            os.write(1, data)
            continue
        kept += data
        place = kept.find(PLACE)
        if kept.find(HIT) < 0 or place < 0 or not kept.endswith(PROMPT):
            continue
        state["armed"] = False
        os.write(1, b"\n" + HIT + b"Set breakpoint " + state["spec"] +
                 b"\n" + PROMPT)
        time.sleep(GAP_S)
        os.write(1, kept[place:])
        kept = b""
    os._exit(child.wait())


threading.Thread(target=pump, daemon=True).start()
while True:
    raw = sys.stdin.buffer.readline()
    if not raw:
        break
    text = raw.decode(errors="replace").rstrip()
    if text.startswith("stop at ") and text.endswith(SET_LINE):
        state["spec"] = text[len("stop at "):].encode()
        state["armed"] = True
    child.stdin.write(raw)
child.stdin.close()
os._exit(child.wait())
'''


# @scenario SC-debug-07
def test_a_jdb_hit_with_the_set_answer_inside_is_marked(tmp_path):
    """A breakpoint set while the program runs, whose hit jdb reports with
    the answer to the "stop at" and a prompt written inside it: the stop is
    read to its place and its line is marked, not taken as a stop on no
    line that leaves the program held with nothing shown."""
    case = BRK_LIVE_CASES["jdb"]
    _need(*case["need"])
    bindir = tmp_path / JDB_INTERLEAVE_BIN_DIR
    bindir.mkdir()
    _write_script(bindir / "jdb", FAKE_JDB_INTERLEAVE % {
        "python": sys.executable, "real": shutil.which("jdb"),
        "line": case["body"], "gap": JDB_INTERLEAVE_GAP_S})
    env = {"PATH": "%s:%s" % (bindir, os.environ.get("PATH", ""))}
    with WpeSession(str(tmp_path), case["prog"], filename=case["file"],
                    wait=case["wait"], env_extra=env) as w:
        _run_into_the_program(w, case["break"], case["start_s"])
        _go_to_line(w, case["body"])
        w.key(CTRL_G, "b", delay=0.6)
        _assert_stops_on(w, case["body"], "a hit with the answer inside")
        _quit_and_see_the_session_gone(w)


# ---- gdb's answers at a stop, read while the editor keeps its keys ----
#
# tests/stubs/gdb stands in front of the real gdb and holds back one command
# (XWPE_STUB_STALL) until the editor sends another or the test writes to its
# release pipe.  It never waits on a clock, so these rows hold the debugger
# exactly as long as they need to, and the log says when the hold began.

STUBS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stubs")
GDB_STUB_LOG = "gdb-stub.log"
GDB_STUB_RELEASE = "gdb-stub.release"
GDB_ANSWER_TIMEOUT_S = 2          # the deadline row's XWPE_DEBUG_ANSWER_TIMEOUT
WAITING_FOR_GDB = "waiting for gdb"
WAITING_FOR_WHERE = "waiting for gdb to answer `where`"
GDB_DID_NOT_ANSWER = "gdb did not answer `where`"
GDB_STALL_WAIT_S = 20
NOTHING_MORE_S = 3                # a deferred key would have acted by then

# A global, so a watch on it has one value whichever frame gdb is in when a
# `where` is given up on.
GDB_GLOBAL_PROG = (
    "#include <stdio.h>\n"
    "int g = 42;\n"
    "int main(void)\n"
    "{\n"
    "  printf(\"READY\\n\");\n"
    "  return g;\n"
    "}\n"
)
GDB_GLOBAL_BREAK_LINE = 5         # printf("READY\n")

# Four steps in a row, so two typed-ahead F8s have lines to land on.
GDB_STEPS_PROG = (
    "int main(void)\n"
    "{\n"
    "  int x = 1;\n"
    "  x = x + 1;\n"
    "  x = x + 1;\n"
    "  x = x + 1;\n"
    "  return x;\n"
    "}\n"
)
GDB_STEPS_BREAK_LINE = 3
GDB_STEPS_AFTER_TWO = 6           # two more F8s from line 4


def _gdb_stub_env(stall, nth=1, extra=None):
    """PATH with the holding gdb first, holding the @nth command that starts
    with @stall."""
    env = {"PATH": STUBS_DIR + ":" + os.environ.get("PATH", ""),
           "XWPE_STUB_REAL_GDB": shutil.which("gdb") or "",
           "XWPE_STUB_STALL": stall,
           "XWPE_STUB_STALL_NTH": str(nth)}
    if extra:
        env.update(extra)
    return env


def _gdb_stub_log(tmp_path):
    try:
        with open(str(tmp_path / GDB_STUB_LOG)) as fh:
            return fh.read()
    except OSError:
        return ""


def _gdb_stub_holds(w, tmp_path, stall):
    """Wait until the stub says it holds the command that starts with
    @stall: the editor is then waiting for gdb's answer."""
    assert _wait_for(w, lambda: ("STALL " + stall) in _gdb_stub_log(tmp_path),
                     GDB_STALL_WAIT_S), \
        "the editor should have sent %r (stub log:\n%s):\n%s" \
        % (stall, _gdb_stub_log(tmp_path), _screen(w))


def _gdb_stub_release(tmp_path):
    """Let the held command go to gdb."""
    with open(str(tmp_path / GDB_STUB_RELEASE), "w") as fh:
        fh.write("go\n")


def _where_held_at_the_first_stop(w, tmp_path):
    """Breakpoint and Ctrl-G R: at the stop gdb is asked `where` for the
    frame to mark, and the stub holds it."""
    _go_to_line(w, GDB_GLOBAL_BREAK_LINE)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    _gdb_stub_holds(w, tmp_path, "where")


# @scenario SC-debug-49
def test_gdb_keys_reach_the_editor_while_it_answers(tmp_path):
    """While gdb has not answered `where` at a stop, Ctrl-G K reaches the
    editor and is refused with the waiting line, which names what gdb is
    answering; no Stack window opens and its letter is not typed into the
    file.  Once gdb answers, the stop is marked as usual."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_GLOBAL_PROG, filename="p.c",
                    wait=1.5, env_extra=_gdb_stub_env("where")) as w:
        _where_held_at_the_first_stop(w, tmp_path)
        w.key(CTRL_G, "k", delay=0.5)
        assert _status_row_says(w, WAITING_FOR_WHERE, STEP_TIMEOUT_S), \
            "Ctrl-G K should be refused with the waiting line:\n%s" \
            % _screen(w)
        assert " Stack " not in _screen(w), \
            "Ctrl-G K must wait for gdb's answer, not ask it:\n%s" \
            % _screen(w)
        w.save()
        assert w.text() == GDB_GLOBAL_PROG, \
            "a key typed while gdb answered landed in the file:\n%s" \
            % _screen(w)
        _gdb_stub_release(tmp_path)
        assert _wait_for(w, lambda: _mark_line(w) == GDB_GLOBAL_BREAK_LINE,
                         STEP_TIMEOUT_S), \
            "once gdb answers the stop should be marked (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))


ANSWER_ENDS = {
    "esc": {"key": ESC, "env": None, "says": GDB_DID_NOT_ANSWER},
    "ctrl_c": {"key": CTRL_C, "env": None, "says": GDB_DID_NOT_ANSWER},
    "deadline": {"key": None,
                 "env": {"XWPE_DEBUG_ANSWER_TIMEOUT":
                         str(GDB_ANSWER_TIMEOUT_S)},
                 "says": GDB_DID_NOT_ANSWER + " within"},
}


# @scenario SC-debug-50
@pytest.mark.parametrize("how", sorted(ANSWER_ENDS))
def test_gdb_stalled_answer_ends_with_a_message_and_quit_works(tmp_path, how):
    """A gdb that does not answer `where` at a stop: the waiting line comes,
    Esc, Ctrl-C or the deadline ends the wait with a line naming the
    command, and Debug > Quit then ends the session with nothing left."""
    case = ANSWER_ENDS[how]
    _need("gcc", "gdb")
    env = _gdb_stub_env("where", extra=case["env"])
    with WpeSession(str(tmp_path), GDB_GLOBAL_PROG, filename="p.c",
                    wait=1.5, env_extra=env) as w:
        _where_held_at_the_first_stop(w, tmp_path)
        assert _status_row_says(w, WAITING_FOR_GDB, STEP_TIMEOUT_S), \
            "the waiting line should come:\n%s" % _screen(w)
        if case["key"]:
            w.key(case["key"], delay=0.5)
        assert _status_row_says(w, case["says"], STEP_TIMEOUT_S), \
            "the wait should end with %r:\n%s" % (case["says"], _screen(w))
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-50
def test_gdb_answer_after_a_cancel_is_not_taken_for_the_next(tmp_path):
    """gdb answers `where` only after the user gave up on it: that late
    answer is dropped, and the next command, a watch on a global, shows its
    own value, not a row of the stack."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_GLOBAL_PROG, filename="p.c",
                    wait=1.5, env_extra=_gdb_stub_env("where")) as w:
        _where_held_at_the_first_stop(w, tmp_path)
        assert _status_row_says(w, WAITING_FOR_GDB, STEP_TIMEOUT_S), \
            "the waiting line should come:\n%s" % _screen(w)
        w.key(ESC, delay=0.5)
        assert _status_row_says(w, GDB_DID_NOT_ANSWER, STEP_TIMEOUT_S), \
            "Esc should end the wait:\n%s" % _screen(w)
        _add_watch(w, "g")
        assert _wait_for(w, lambda: _watch_row(w, "g") is not None,
                         STEP_TIMEOUT_S), \
            "the watch should get an answer:\n%s" % _screen(w)
        assert _watch_row(w, "g").startswith("42"), \
            "the watch took another command's answer (%r):\n%s" \
            % (_watch_row(w, "g"), _screen(w))


# @scenario SC-debug-49
def test_gdb_f8_f8_typed_during_the_watches_steps_twice(tmp_path):
    """Two F8s typed while gdb has not answered a watch at a stop are kept,
    in order: once the answer comes the program steps twice, and nothing
    lands in the file."""
    _need("gcc", "gdb")
    with WpeSession(str(tmp_path), GDB_STEPS_PROG, filename="p.c", wait=1.5,
                    env_extra=_gdb_stub_env("p x", nth=2)) as w:
        _go_to_line(w, GDB_STEPS_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == GDB_STEPS_BREAK_LINE,
                         START_TIMEOUT_S), _screen(w)
        _add_watch(w, "x")
        assert _wait_for(w, lambda: _watch_row(w, "x") is not None,
                         STEP_TIMEOUT_S), _screen(w)
        w.key(F8, delay=0.3)
        _gdb_stub_holds(w, tmp_path, "p x")
        w.key(F8, delay=0.2)
        w.key(F8, delay=0.2)
        _gdb_stub_release(tmp_path)
        assert _wait_for(w, lambda: _mark_line(w) == GDB_STEPS_AFTER_TWO,
                         STEP_TIMEOUT_S), \
            "the two F8s typed during the watch should step twice (mark " \
            "on %s):\n%s" % (_mark_line(w), _screen(w))
        w._drain(NOTHING_MORE_S)
        assert _mark_line(w) == GDB_STEPS_AFTER_TWO, \
            "only the two F8s typed should step (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        w.save()
        assert w.text() == GDB_STEPS_PROG, \
            "a key typed while gdb answered landed in the file:\n%s" \
            % _screen(w)


GDB_LOOP_SLEEP_LINE = 9           # sleep(3), in GDB_LOOP_PROG's loop
GDB_LOOP_BREAK_LINE = 6           # printf("READY\n")


# @scenario SC-debug-49
def test_gdb_f8_during_a_sync_interrupt_is_refused_as_running(tmp_path):
    """A breakpoint set while the program runs stops it for a moment to tell
    gdb.  F8 pressed while gdb has not answered is refused as while the
    program runs, and not kept: after the program reaches the breakpoint
    the mark stays on it."""
    _need("gcc", "gdb")
    stall = "b p.c:%d" % GDB_LOOP_SLEEP_LINE
    with WpeSession(str(tmp_path), GDB_LOOP_PROG, filename="p.c", wait=1.5,
                    env_extra=_gdb_stub_env(stall)) as w:
        _stop_then_run_on(w, GDB_LOOP_BREAK_LINE, START_TIMEOUT_S)
        _go_to_line(w, GDB_LOOP_SLEEP_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        _gdb_stub_holds(w, tmp_path, stall)
        w.key(F8, delay=0.3)
        assert _status_row_says(w, RUNNING_REFUSAL, STEP_TIMEOUT_S), \
            "F8 should be refused as while the program runs:\n%s" % _screen(w)
        _gdb_stub_release(tmp_path)
        assert _wait_for(w, lambda: _mark_line(w) == GDB_LOOP_SLEEP_LINE,
                         PAUSE_TIMEOUT_S), \
            "the program should stop on the new breakpoint (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        w._drain(NOTHING_MORE_S)
        assert _mark_line(w) == GDB_LOOP_SLEEP_LINE, \
            "the refused F8 must not step later (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))


GDB_LOOP_PASSES_S = 7             # two turns of GDB_LOOP_PROG's sleep(3)
GDB_LOOP_SECOND_ID = 2            # gdb's number for the second breakpoint


# @scenario SC-debug-49
def test_gdb_breakpoint_set_again_while_gdb_removes_it_stays(tmp_path):
    """A breakpoint taken off while the program runs, and set again on the
    same line before gdb has answered its removal: the editor goes on, the
    line is drawn solid, and the session ends cleanly."""
    _need("gcc", "gdb")
    remove = "d 1"
    with WpeSession(str(tmp_path), GDB_LOOP_PROG, filename="p.c", wait=1.5,
                    env_extra=_gdb_stub_env(remove)) as w:
        _stop_then_run_on(w, GDB_LOOP_BREAK_LINE, START_TIMEOUT_S)
        _go_to_line(w, GDB_LOOP_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        _gdb_stub_holds(w, tmp_path, remove)
        w.key(CTRL_G, "b", delay=0.4)
        _gdb_stub_release(tmp_path)
        _assert_solid(w, GDB_LOOP_BREAK_LINE, "set again during its removal")
        w._drain(NOTHING_MORE_S)
        assert w.proc.poll() is None, \
            "the editor should go on after gdb answered the removal:\n%s" \
            % _screen(w)
        _quit_and_see_the_session_gone(w)


# @scenario SC-debug-49
def test_gdb_breakpoint_taken_off_while_gdb_sets_it_does_not_stop(tmp_path):
    """A breakpoint set while the program runs, and taken off again before
    gdb has answered it: gdb is told to remove the one it made.  The program
    may reach the line once before gdb hears of it, as with any breakpoint
    taken off while the program runs; after that it never stops there."""
    _need("gcc", "gdb")
    add = "b p.c:%d" % GDB_LOOP_SLEEP_LINE
    remove = "CMD d %d" % GDB_LOOP_SECOND_ID
    with WpeSession(str(tmp_path), GDB_LOOP_PROG, filename="p.c", wait=1.5,
                    env_extra=_gdb_stub_env(add)) as w:
        _stop_then_run_on(w, GDB_LOOP_BREAK_LINE, START_TIMEOUT_S)
        _go_to_line(w, GDB_LOOP_SLEEP_LINE)
        w.key(CTRL_G, "b", delay=0.4)
        _gdb_stub_holds(w, tmp_path, add)
        w.key(CTRL_G, "b", delay=0.4)
        _gdb_stub_release(tmp_path)
        assert _wait_for(w, lambda: remove in _gdb_stub_log(tmp_path),
                         GDB_LOOP_PASSES_S), \
            "gdb should be told to remove the breakpoint it set (stub " \
            "log:\n%s):\n%s" % (_gdb_stub_log(tmp_path), _screen(w))
        if _mark_line(w) == GDB_LOOP_SLEEP_LINE:
            w.key(CTRL_G, "r", delay=0.5)
        w._drain(GDB_LOOP_PASSES_S)
        assert _mark_line(w) != GDB_LOOP_SLEEP_LINE, \
            "the program stopped again on a line the margin no longer " \
            "marks (stub log:\n%s):\n%s" % (_gdb_stub_log(tmp_path),
                                            _screen(w))
        _quit_and_see_the_session_gone(w)
