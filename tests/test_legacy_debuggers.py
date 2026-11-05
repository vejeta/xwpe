"""The sdb, dbx and xdb debugger backends, driven through a stub of each.

sdb, dbx and xdb are proprietary debuggers of commercial UNIX: none is in
Debian, none has a free implementation and none would run on Linux.  xwpe's
own enum names them (classic sdb with prompt "*", "dbx -i" with "(dbx)", HP
"xdb -L" with ">"), so each is stood in here by a stub that answers only the
commands xwpe's source says it sends, in the wording xwpe's own parsers look
for, and that logs every command it is given.  The stub is
tests/stubs/legacy/legacy-debugger, copied under each debugger's name; its
docstring says what it assumes where xwpe's source is silent (an interrupt
at the prompt writes nothing; each prints a string it is asked to), and how
it holds back one command for the rows that need a debugger slow to answer.

What that proves, and what it does not, is the point of the file: the stub
shows that xwpe sends the documented command and reads the answer it expects,
so a backend that answers cannot wedge the editor.  It says nothing about how
a real sdb, dbx or xdb behaves.

Every test asserts on the command log as well as on the screen, because the
pairing is what matters: which command xwpe sent, and which answer it then
waited for.

What nothing here can cover is the debugger dialog's seventh row.  That row
exists only in a build configured with --with-xdb, and the automated suite runs
the default one, so it stays a manual check: ./configure --with-xdb && make,
then open Debug > debugger Options and confirm seven rows render inside the box
(Gdb, Sdb, Dbx, Xdb, Jdb, Pdb, A68g), that Ok and Cancel still sit below them,
and that choosing Dbx and Xdb starts "dbx -i" and "xdb -L" respectively.
"""
import os
import re
import shutil
import sys
import time

import pytest

import procinfo
from wpe_driver import WpeSession, ALT, WAIT_SCALE

CTRL_G = "\x07"         # debugger command prefix
DOWN = "\033OB"
ALT_X = "\033x"         # quit the editor

POLL_S = 0.2
START_TIMEOUT_S = 20    # build + debugger start, before scaling
STEP_TIMEOUT_S = 8

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
    '  printf("OUTMARK %d\\n", y);\n'
    "  x = y + 1;\n"
    "  return 0;\n"
    "}\n"
)
C_BREAK_LINE = 8          # int x = 3;
C_CURSOR_LINE = 11        # x = y + 1;
C_SOURCE = "p.c"

# The options file forces the backend, so the dialog is not in the way: a
# missing sdb, dbx or xdb on PATH is what the stub stands in for.
DEBUGGER_TYPE = {"sdb": 1, "dbx": 2, "xdb": 3}
BACKENDS = ("sdb", "dbx", "xdb")

# What each backend is sent to set a breakpoint on a line, and to take it out
# again.  dbx and xdb name a breakpoint by the number they gave it, so their
# clear names that number, which is what the second entry asserts.
ARM_COMMAND = {
    "sdb": ["e p.c", "11 b"],
    "dbx": ['stop at "p.c":11'],
    "xdb": ["b p.c:11"],
}
DISARM_COMMAND = {
    "sdb": ["e p.c", "11 d"],
    "dbx": ["delete 2"],
    "xdb": ["db 2"],
}
# What each backend is sent to clear every breakpoint at once.
REMOVE_ALL_COMMAND = {"sdb": "D", "dbx": "delete all", "xdb": "db *"}
# What each backend is sent to go on running after a stop.
CONTINUE_COMMAND = {"sdb": "c", "dbx": "cont", "xdb": "c"}
# The command that starts the program, without its "> /dev/pts/N" redirect.
START_RUN_COMMAND = {"sdb": "r", "dbx": "run", "xdb": "r"}
# Everything sent before the run: the breakpoint, plus xdb's "sm".
START_COMMANDS = {
    "sdb": ["e p.c", "8 b"],
    "dbx": ['stop at "p.c":8'],
    "xdb": ["sm", "b p.c:8"],
}
# What Messages shows when the program reaches its end.  sdb and dbx say
# "exited", which xwpe turns into its own line; xdb's wording has no "exited"
# in it, so xwpe shows the line xdb printed.
END_LINE = {
    "sdb": "Program exited.",
    "dbx": "Program exited.",
    "xdb": "Child process terminated normally",
}

# The stand-in for all three, run under the debugger's own name
# (tests/stubs/legacy/legacy-debugger tells what it answers and how).
STUB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "stubs", "legacy", "legacy-debugger")


def _need(*tools):
    """Skip the test, naming the first missing binary."""
    for tool in tools:
        if shutil.which(tool) is None:
            pytest.skip("no %s" % tool)


def _stub_body():
    """The stand-in's source without its "#!" line."""
    with open(STUB_PATH) as fh:
        text = fh.read()
    return text.split("\n", 1)[1]


def _stub_env(tmp_path, backend, continue_runs_on=False, continue_delay_s=0,
              continue_dies=False, interrupt_at=0, stall="", stall_nth=1):
    """Write the stub for @backend and return the environment that runs it.

    The stub is one script under the debugger's own name, so the backend is
    the name xwpe exec's, exactly as the real one would be; its "#!" line is
    this Python itself, so the process runs under that name too.  With
    @continue_runs_on, a continue after the first stop never comes back: the
    program runs on and the debugger says nothing.  With @continue_delay_s,
    a continue answers only after that many seconds.  With @continue_dies,
    a continue writes half a line and the debugger exits.  With
    @interrupt_at, a program that runs on is stopped by SIGINT on that line.
    With @stall, the @stall_nth command that starts with it is held until the
    editor sends another line or the test releases it (_release)."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    stub = bindir / backend
    stub.write_text("#!%s\n%s" % (sys.executable, _stub_body()))
    stub.chmod(0o755)
    env = {
        "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
        "XWPE_STUB_LOG": str(tmp_path / (backend + ".log")),
        "XWPE_STUB_RELEASE": str(tmp_path / (backend + ".release")),
        "XWPE_STUB_SOURCE": C_SOURCE,
    }
    if continue_runs_on:
        env["XWPE_STUB_CONTINUE_RUNS_ON"] = "1"
    if continue_delay_s:
        env["XWPE_STUB_CONTINUE_DELAY_S"] = str(continue_delay_s)
    if continue_dies:
        env["XWPE_STUB_CONTINUE_DIES"] = "1"
    if interrupt_at:
        env["XWPE_STUB_INTERRUPT_AT"] = str(interrupt_at)
    if stall:
        env["XWPE_STUB_STALL"] = stall
        env["XWPE_STUB_STALL_NTH"] = str(stall_nth)
    return env


def _session(tmp_path, backend, continue_runs_on=False, continue_delay_s=0,
             continue_dies=False, interrupt_at=0, stall="", stall_nth=1,
             extra=None):
    """A session on p.c with @backend forced through the options file.

    The options file is how a user picks a backend the dialog of this build
    does not offer, so the test does not depend on the dialog's rows.
    @extra: more of the editor's environment."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text(
        "[Programming]\nDebugger : %d\n" % DEBUGGER_TYPE[backend])
    env = _stub_env(tmp_path, backend, continue_runs_on, continue_delay_s,
                    continue_dies, interrupt_at, stall, stall_nth)
    if extra:
        env.update(extra)
    return WpeSession(str(tmp_path), C_PROG, filename=C_SOURCE, wait=1.5,
                      env_extra=env)


def _commands(tmp_path, backend):
    """Every command the stub for @backend was sent, in order."""
    log = tmp_path / (backend + ".log")
    if not log.exists():
        return []
    return [line[4:] for line in log.read_text().splitlines()
            if line.startswith("CMD ")]


def _plain(command):
    """A command without the "> /dev/pts/N" redirect, whose number varies."""
    return command.split(" > ")[0].strip()


def _sent(tmp_path, backend):
    """The command log with every redirect stripped, for comparison."""
    return [_plain(each) for each in _commands(tmp_path, backend)]


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


def _mark_line(w):
    """The source line carrying the current-line mark, or None."""
    return w.mark_line()


def _move_down(w, lines):
    for _ in range(lines):
        w.key(DOWN, delay=0.1)


def _cursor_field(w):
    """The line:column field of the source window's status row, or None.

    It moves only when the editor took a key, which is what makes it usable
    to show the editor is not waiting for an answer that never comes."""
    for row in w.display(drain=0):
        found = re.search(r"\s(\d+):(\d+)\s", row)
        if found:
            return found.group(0).strip()
    return None


def _messages(w):
    """The text of the Messages window, which is below the source window."""
    rows = w.display(drain=0)
    for i, row in enumerate(rows):
        if "Messages" in row:
            return "\n".join(rows[i + 1:])
    return ""


def _start_session(w, tmp_path, backend):
    """Breakpoint on line 8, then Ctrl-G R; wait for the stop."""
    _need("gcc")
    _move_down(w, C_BREAK_LINE - 1)
    w.key(CTRL_G, "b", delay=0.4)
    w.key(CTRL_G, "r", delay=1.0)
    assert _wait_for(w, lambda: _mark_line(w) == C_BREAK_LINE,
                     START_TIMEOUT_S), \
        "Ctrl-G R should stop %s on line %d; it was sent %r:\n%s" \
        % (backend, C_BREAK_LINE, _sent(tmp_path, backend), _screen(w))


# @scenario SC-debug-01
@pytest.mark.parametrize("backend", BACKENDS)
def test_the_session_starts_and_stops_where_the_editor_says(tmp_path, backend):
    """Ctrl-G R starts the backend, arms the breakpoint the margin shows and
    stops the program on that line.

    Limit: the stub proves xwpe's half of the protocol, that it sends the
    documented commands and reads the answers, not a real %s.""" % backend
    with _session(tmp_path, backend) as w:
        _start_session(w, tmp_path, backend)
        sent = _sent(tmp_path, backend)
        assert sent[:len(START_COMMANDS[backend])] == START_COMMANDS[backend], \
            "%s should have been sent %r before the run; it got %r" \
            % (backend, START_COMMANDS[backend], sent)
        assert START_RUN_COMMAND[backend] in sent, \
            "%s should have been told to run; it got %r" % (backend, sent)
        w.key(ALT_X, delay=0.5)


# @scenario SC-debug-07
@pytest.mark.parametrize("backend", BACKENDS)
def test_a_breakpoint_armed_mid_session_stops_and_a_disarmed_one_does_not(
        tmp_path, backend):
    """A line marked while the session runs is sent to the backend and stops
    the next run; the same line unmarked is sent again and does not stop it.

    Limit: the stub proves xwpe's half of the protocol, that it sends the
    documented commands and reads the answers, not a real %s.""" % backend
    with _session(tmp_path, backend) as w:
        _start_session(w, tmp_path, backend)
        before = len(_commands(tmp_path, backend))
        _move_down(w, C_CURSOR_LINE - C_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.6)
        assert _sent(tmp_path, backend)[before:] == ARM_COMMAND[backend], \
            "arming line %d should have sent %r; it sent %r" \
            % (C_CURSOR_LINE, ARM_COMMAND[backend],
               _sent(tmp_path, backend)[before:])
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_LINE,
                         STEP_TIMEOUT_S), \
            "the armed line should have stopped the run:\n%s" % _screen(w)

        before = len(_commands(tmp_path, backend))
        w.key(CTRL_G, "b", delay=0.6)
        assert _sent(tmp_path, backend)[before:] == DISARM_COMMAND[backend], \
            "disarming line %d should have sent %r; it sent %r" \
            % (C_CURSOR_LINE, DISARM_COMMAND[backend],
               _sent(tmp_path, backend)[before:])
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) is None, STEP_TIMEOUT_S), \
            "the disarmed line should not have stopped the run:\n%s" \
            % _screen(w)
        w.key(ALT_X, delay=0.5)


# @scenario SC-debug-07
@pytest.mark.parametrize("backend", BACKENDS)
def test_remove_all_returns_and_the_editor_still_takes_keys(tmp_path, backend):
    """Ctrl-G M clears every breakpoint in the backend and comes back: the
    editor takes the next key instead of waiting for an answer.

    Limit: the stub proves xwpe's half of the protocol, that it sends the
    documented commands and reads the answers, not a real %s.""" % backend
    with _session(tmp_path, backend) as w:
        _start_session(w, tmp_path, backend)
        _move_down(w, C_CURSOR_LINE - C_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.6)
        assert C_CURSOR_LINE in (w.breakpoint_lines() or []), \
            "the second breakpoint should be in the margin:\n%s" % _screen(w)

        before = len(_commands(tmp_path, backend))
        w.key(CTRL_G, "m", delay=1.0)
        assert _wait_for(w, lambda: REMOVE_ALL_COMMAND[backend]
                         in _sent(tmp_path, backend)[before:],
                         STEP_TIMEOUT_S), \
            "Remove all should have sent %r to %s; it sent %r" \
            % (REMOVE_ALL_COMMAND[backend], backend,
               _sent(tmp_path, backend)[before:])
        assert _wait_for(w, lambda: not w.breakpoint_lines(),
                         STEP_TIMEOUT_S), \
            "the margin should show no breakpoint left:\n%s" % _screen(w)

        was = _cursor_field(w)
        w.key(DOWN, delay=0.8)
        assert _cursor_field(w) != was, \
            "the editor took no key after Remove all; the field still reads " \
            "%r, so it is waiting for an answer:\n%s" % (was, _screen(w))
        w.key(ALT_X, delay=0.5)


# @scenario SC-debug-06
@pytest.mark.parametrize("backend", BACKENDS)
def test_go_to_cursor_runs_to_the_line_under_the_cursor(tmp_path, backend):
    """Ctrl-G U arms a breakpoint of its own on the cursor's line, goes on
    running, stops there and takes that breakpoint out again.

    Limit: the stub proves xwpe's half of the protocol, that it sends the
    documented commands and reads the answers, not a real %s.""" % backend
    with _session(tmp_path, backend) as w:
        _start_session(w, tmp_path, backend)
        _move_down(w, C_CURSOR_LINE - C_BREAK_LINE)
        before = len(_commands(tmp_path, backend))
        w.key(CTRL_G, "u", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_LINE,
                         STEP_TIMEOUT_S), \
            "Go to cursor should have stopped on line %d; %s was sent %r:" \
            "\n%s" % (C_CURSOR_LINE, backend, _sent(tmp_path, backend),
                      _screen(w))
        sent = _sent(tmp_path, backend)[before:]
        assert sent[:len(ARM_COMMAND[backend])] == ARM_COMMAND[backend], \
            "Go to cursor should have armed line %d with %r; it sent %r" \
            % (C_CURSOR_LINE, ARM_COMMAND[backend], sent)
        assert CONTINUE_COMMAND[backend] in sent, \
            "Go to cursor should have told %s to go on; it sent %r" \
            % (backend, sent)


# @scenario SC-debug-13
@pytest.mark.parametrize("backend", BACKENDS)
def test_the_programs_end_is_named_where_the_editor_says(tmp_path, backend):
    """With no breakpoint left, the next run reaches the program's end and the
    editor says so in the words it uses for that backend.

    Limit: the stub proves xwpe's half of the protocol, that it sends the
    documented commands and reads the answers, not a real %s.""" % backend
    with _session(tmp_path, backend) as w:
        _start_session(w, tmp_path, backend)
        w.key(CTRL_G, "m", delay=1.0)
        assert _wait_for(w, lambda: REMOVE_ALL_COMMAND[backend]
                         in _sent(tmp_path, backend), STEP_TIMEOUT_S), \
            "Remove all should have reached %s:\n%s" % (backend, _screen(w))
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: END_LINE[backend] in _screen(w),
                         STEP_TIMEOUT_S), \
            "the end of the program should be named %r; %s was sent %r and " \
            "the screen reads:\n%s" \
            % (END_LINE[backend], backend, _sent(tmp_path, backend),
               _screen(w))
        assert _wait_for(w, lambda: _mark_line(w) is None, STEP_TIMEOUT_S), \
            "the current-line mark should go once the program ran:\n%s" \
            % _screen(w)
        w.key(ALT_X, delay=0.5)


def _stub_running(w, backend):
    """PIDs of the editor's children running the stub for @backend."""
    return procinfo.children_named(w.proc.pid, backend)


def _quit_while_the_program_runs(tmp_path, backend):
    """Stop on line 8, continue into a program that never stops again, then
    Debug > Quit (Alt-D, Q): the debugger ends and the editor edits again."""
    with _session(tmp_path, backend, continue_runs_on=True) as w:
        _start_session(w, tmp_path, backend)
        before = len(_commands(tmp_path, backend))
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: CONTINUE_COMMAND[backend]
                         in _sent(tmp_path, backend)[before:],
                         STEP_TIMEOUT_S), \
            "Ctrl-G R should have told %s to go on; it sent %r" \
            % (backend, _sent(tmp_path, backend)[before:])
        assert _stub_running(w, backend), \
            "%s should be running the program:\n%s" % (backend, _screen(w))
        w.menu(ALT.DEBUG, "q", delay=0.5)
        assert _wait_for(w, lambda: not _stub_running(w, backend),
                         STEP_TIMEOUT_S), \
            "Debug > Quit should end %s while the program runs; it was " \
            "sent %r and the screen reads:\n%s" \
            % (backend, _sent(tmp_path, backend), _screen(w))
        w.key("Q", delay=0.3)
        w.save()
        assert "Q" in w.text(), \
            "typing after the session should edit the source:\n%s" \
            % _screen(w)


# @scenario SC-debug-18
def test_debug_quit_while_the_program_runs_under_sdb(tmp_path):
    """Parity with gdb: Debug > Quit ends an sdb session while the program
    runs.  The run was read by a loop that sent every key but Ctrl-C to the
    debugger, so the menu never opened.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _quit_while_the_program_runs(tmp_path, "sdb")


# @scenario SC-debug-18
def test_debug_quit_while_the_program_runs_under_dbx(tmp_path):
    """Parity with gdb: Debug > Quit ends a dbx session while the program
    runs.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _quit_while_the_program_runs(tmp_path, "dbx")


# @scenario SC-debug-18
def test_debug_quit_while_the_program_runs_under_xdb(tmp_path):
    """Parity with gdb: Debug > Quit ends an xdb session while the program
    runs.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb."""
    _quit_while_the_program_runs(tmp_path, "xdb")


CTRL_W = "\x17"           # close the window on top
CLOSE_DELAY_S = 4         # how long the continue runs before its stop


# @scenario SC-debug-18
@pytest.mark.parametrize("backend", BACKENDS)
def test_closing_the_window_while_the_program_runs_keeps_the_editor(
        tmp_path, backend):
    """Ctrl-W closes the source window while the program runs to its next
    breakpoint: when it stops, the stop is marked in the file opened again
    and the editor and the debugger are still there.  The stop used to be
    shown through the window that was closed, freed memory.

    Limit: the stub proves xwpe's half of the protocol, not a real %s.""" \
        % backend
    with _session(tmp_path, backend, continue_delay_s=CLOSE_DELAY_S) as w:
        _start_session(w, tmp_path, backend)
        _move_down(w, C_CURSOR_LINE - C_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.6)
        w.key(CTRL_G, "r", delay=0.5)
        w.key(CTRL_W, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_LINE,
                         STEP_TIMEOUT_S + CLOSE_DELAY_S), \
            "the stop on line %d should be marked after the window closed " \
            "(editor alive: %s); %s was sent %r:\n%s" \
            % (C_CURSOR_LINE, w.alive(), backend, _sent(tmp_path, backend),
               _screen(w))
        assert w.alive(), "the editor should still run"
        assert _stub_running(w, backend), \
            "%s should still run the session:\n%s" % (backend, _screen(w))


IDLE_CPU_TICKS = 20       # below this per wall second the editor is idle
CPU_SAMPLE_S = 2.0


def _cpu_used_over(w, seconds):
    """CPU ticks the editor used while it sat @seconds reading the screen,
    or None when its process is gone."""
    before = procinfo.cpu_ticks(w.proc.pid)
    w._drain(seconds)
    after = procinfo.cpu_ticks(w.proc.pid)
    if before is None or after is None:
        return None
    return after - before


# @scenario SC-debug-15
@pytest.mark.parametrize("backend", BACKENDS)
def test_a_debugger_that_dies_mid_line_while_the_program_runs(tmp_path,
                                                              backend):
    """The debugger writes half a line after a continue and exits: the
    editor stays idle, the session ends and Messages names the debugger
    that ended.  The half line was kept back as a line still to come, so the
    end of file woke the editor without end, a whole core for ever.

    Limit: the stub proves xwpe's half of the protocol, not a real %s.""" \
        % backend
    with _session(tmp_path, backend, continue_dies=True) as w:
        _start_session(w, tmp_path, backend)
        w.key(CTRL_G, "r", delay=0.5)
        used = _cpu_used_over(w, CPU_SAMPLE_S)
        if used is not None:
            assert used < IDLE_CPU_TICKS * CPU_SAMPLE_S * WAIT_SCALE, \
                "the editor should be idle once %s died, used %d ticks in " \
                "%.1fs" % (backend, used, CPU_SAMPLE_S)
        gone = "%s ended" % backend
        assert _wait_for(w, lambda: gone in _screen(w), STEP_TIMEOUT_S), \
            "Messages should say %r; %s was sent %r:\n%s" \
            % (gone, backend, _sent(tmp_path, backend), _screen(w))
        assert _mark_line(w) is None, \
            "the current-line mark should go:\n%s" % _screen(w)


PAUSED_LINE = "Paused"


def _bottom_row(w):
    """The screen's last row: the status line or the window's key hints."""
    return w.display(drain=0)[-1]


def _pause_marks_where_the_program_is(tmp_path, backend):
    """Stop on line 8, continue into a program that runs on, then press
    Ctrl-G I: the backend is sent SIGINT, reports where the program was
    (line 11), and the editor marks that line, says "Paused" and shows no
    error box for the interrupt."""
    with _session(tmp_path, backend, continue_runs_on=True,
                  interrupt_at=C_CURSOR_LINE) as w:
        _start_session(w, tmp_path, backend)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _stub_running(w, backend)
                         and CONTINUE_COMMAND[backend]
                         in _sent(tmp_path, backend), STEP_TIMEOUT_S), \
            "%s should be running the program:\n%s" % (backend, _screen(w))
        w.key(CTRL_G, "i", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_LINE,
                         STEP_TIMEOUT_S), \
            "Ctrl-G I should mark line %d (mark on %s); %s was sent %r:\n%s" \
            % (C_CURSOR_LINE, _mark_line(w), backend,
               _sent(tmp_path, backend), _screen(w))
        assert _wait_for(w, lambda: PAUSED_LINE in _bottom_row(w),
                         STEP_TIMEOUT_S), \
            "the status row should say %r:\n%s" % (PAUSED_LINE, _screen(w))
        assert "interrupt in main" not in _screen(w), \
            "the interrupt is not an error to show:\n%s" % _screen(w)
        w.key(ALT_X, delay=0.5)


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_sdb(tmp_path):
    """Ctrl-G I under sdb: SIGINT reaches sdb, which says where the program
    stands, and the editor marks that line.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _pause_marks_where_the_program_is(tmp_path, "sdb")


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_dbx(tmp_path):
    """Ctrl-G I under dbx: SIGINT reaches dbx, which says where the program
    stands, and the editor marks that line.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _pause_marks_where_the_program_is(tmp_path, "dbx")


# @scenario SC-debug-45
def test_pause_marks_where_the_program_is_under_xdb(tmp_path):
    """Ctrl-G I under xdb: SIGINT reaches xdb, whose report of the interrupt
    is recognised while Pause waits, and the editor marks that line.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb, whose
    wording for the interrupt is not known; the stub's names SIGINT."""
    _pause_marks_where_the_program_is(tmp_path, "xdb")


# What each backend is sent to set a breakpoint on line 9 (the line after the
# one the session stopped on), and the line the stub's program is on when
# Pause interrupts it.
C_LATER_LINE = 9
ARM_LATER_COMMAND = {
    "sdb": ["e p.c", "9 b"],
    "dbx": ['stop at "p.c":9'],
    "xdb": ["b p.c:9"],
}
HOLLOW_WAIT_S = 6


def _sent_in_a_row(sent, commands):
    """True when @commands were sent one right after the other somewhere in
    @sent: the stop's own commands (Pause's stack trace) may come before."""
    size = len(commands)
    return any(sent[i:i + size] == commands
               for i in range(len(sent) - size + 1))


def _breakpoint_drawn(w, line, hollow):
    """Wait until @line is drawn as the outline of the red bar (@hollow) or as
    the solid bar, and return whether it is."""
    def drawn():
        outline = line in w.unconfirmed_breakpoint_lines()
        solid = line in w.breakpoint_lines()
        if hollow:
            return outline and not solid
        return solid and not outline
    return _wait_for(w, drawn, HOLLOW_WAIT_S)


# @scenario SC-debug-07
@pytest.mark.parametrize("backend", BACKENDS)
def test_a_breakpoint_set_while_running_is_hollow_then_solid_at_the_next_stop(
        tmp_path, backend):
    """A line marked while the program runs under sdb, dbx or xdb is not
    refused and not sent while the debugger is busy: its line is drawn hollow,
    and at the next stop (Pause here) it is sent and drawn as the solid bar.

    Limit: the stub proves xwpe's half of the protocol, not a real debugger.
    """
    with _session(tmp_path, backend, continue_runs_on=True,
                  interrupt_at=C_CURSOR_LINE) as w:
        _start_session(w, tmp_path, backend)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _stub_running(w, backend)
                         and CONTINUE_COMMAND[backend]
                         in _sent(tmp_path, backend), STEP_TIMEOUT_S), \
            "%s should be running the program:\n%s" % (backend, _screen(w))
        before = len(_commands(tmp_path, backend))
        _move_down(w, C_LATER_LINE - C_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.6)
        assert _sent(tmp_path, backend)[before:] == [], \
            "nothing should be sent to the busy %s; it got %r" \
            % (backend, _sent(tmp_path, backend)[before:])
        assert _breakpoint_drawn(w, C_LATER_LINE, hollow=True), \
            "line %d should be drawn hollow while the program runs:\n%s" \
            % (C_LATER_LINE, _screen(w))
        w.key(CTRL_G, "i", delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == C_CURSOR_LINE,
                         STEP_TIMEOUT_S), \
            "Ctrl-G I should mark line %d (mark on %s):\n%s" \
            % (C_CURSOR_LINE, _mark_line(w), _screen(w))
        assert _breakpoint_drawn(w, C_LATER_LINE, hollow=False), \
            "line %d should be solid after the stop; %s was sent %r:\n%s" \
            % (C_LATER_LINE, backend, _sent(tmp_path, backend)[before:],
               _screen(w))
        sent = _sent(tmp_path, backend)[before:]
        assert _sent_in_a_row(sent, ARM_LATER_COMMAND[backend]), \
            "the stop should send %r; %s got %r" \
            % (ARM_LATER_COMMAND[backend], backend, sent)
        w.key(ALT_X, delay=0.5)


WATCH_LATER_WORDS = "x: (at the next stop)"
WATCH_FILLED_WORDS = "x: 3"        # what the stub answers for x


def _watch_text_shown(w, words):
    """True when a row of the screen holds @words."""
    return any(words in row for row in w.display(drain=0))


# @scenario SC-debug-10
@pytest.mark.parametrize("backend", BACKENDS)
def test_a_watch_added_while_running_shows_at_the_next_stop(tmp_path,
                                                            backend):
    """A watch made while the program runs under sdb, dbx or xdb is not
    refused and not sent to the busy debugger: it is listed at once, saying
    its value comes at the next stop (Pause here), where it is read.

    Limit: the stub proves xwpe's half of the protocol, not a real debugger.
    """
    with _session(tmp_path, backend, continue_runs_on=True,
                  interrupt_at=C_CURSOR_LINE) as w:
        _start_session(w, tmp_path, backend)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _stub_running(w, backend)
                         and CONTINUE_COMMAND[backend]
                         in _sent(tmp_path, backend), STEP_TIMEOUT_S), \
            "%s should be running the program:\n%s" % (backend, _screen(w))
        before = len(_commands(tmp_path, backend))
        w.key(CTRL_G, "w", delay=0.8)
        w.key("x", delay=0.1)
        w.key("\r", delay=0.6)
        assert _wait_for(w, lambda: _watch_text_shown(w, WATCH_LATER_WORDS),
                         STEP_TIMEOUT_S), \
            "the watch should wait for the next stop:\n%s" % _screen(w)
        assert _sent(tmp_path, backend)[before:] == [], \
            "nothing should be sent to the busy %s; it got %r" \
            % (backend, _sent(tmp_path, backend)[before:])
        w.key(CTRL_G, "i", delay=0.5)
        assert _wait_for(w, lambda: _watch_text_shown(w, WATCH_FILLED_WORDS),
                         STEP_TIMEOUT_S), \
            "the stop should read the watch:\n%s" % _screen(w)
        w.key(ALT_X, delay=0.5)


WATCH_UNDEFINED_NAME = "nosuchname"    # the stub's UNDEFINED_NAME
WATCH_NO_VALUE_WORDS = WATCH_UNDEFINED_NAME + ": No symbol in current context"


# @scenario SC-debug-10
def test_a_watch_answered_without_a_value_says_no_symbol_whole(tmp_path):
    """dbx answers the print of a name the program does not have with a line
    that has no "=": the watch reads the editor's whole "No symbol in current
    context", not the words with their first letter cut off.

    Limit: the stub proves xwpe's half of the protocol, not a real debugger.
    """
    with _session(tmp_path, "dbx") as w:
        _start_session(w, tmp_path, "dbx")
        _add_watch_keys(w, WATCH_UNDEFINED_NAME)
        assert _wait_for(w, lambda: _watch_text_shown(w, WATCH_NO_VALUE_WORDS),
                         STEP_TIMEOUT_S), \
            "the watch should read %r; dbx was sent %r:\n%s" \
            % (WATCH_NO_VALUE_WORDS, _sent(tmp_path, "dbx"), _screen(w))
        w.key(ALT_X, delay=0.5)


def _add_watch_keys(w, expr):
    """Ctrl-G W, @expr, Enter: the watch is made as the user makes it."""
    w.key(CTRL_G, "w", delay=0.8)
    for ch in expr:
        w.key(ch, delay=0.05)
    w.key("\r", delay=0.6)


NOT_INTERRUPTED_S = 4      # longer than an interrupt and its stop take


def _never_stopped(w, seconds):
    """True when no stop is marked for @seconds: the stub reports one only
    when it is interrupted."""
    deadline = time.time() + seconds * WAIT_SCALE
    while time.time() < deadline:
        w._drain(0.2)
        if _mark_line(w) is not None:
            return False
    return True


# @scenario SC-debug-07
@pytest.mark.parametrize("backend", BACKENDS)
def test_a_breakpoint_set_while_running_stops_the_program_there(tmp_path,
                                                                backend):
    """gdb, pdb and a68g are interrupted to take a breakpoint set while the
    program runs; sdb, dbx and xdb are not.  Their way of taking one at that
    moment was never recorded, so the program is left running, nothing is
    sent, and the line stays hollow until the program's next stop.

    Limit: the stub proves xwpe's half of the protocol, not a real debugger.
    """
    with _session(tmp_path, backend, continue_runs_on=True,
                  interrupt_at=C_CURSOR_LINE) as w:
        _start_session(w, tmp_path, backend)
        w.key(CTRL_G, "r", delay=0.5)
        assert _wait_for(w, lambda: _stub_running(w, backend)
                         and CONTINUE_COMMAND[backend]
                         in _sent(tmp_path, backend), STEP_TIMEOUT_S), \
            "%s should be running the program:\n%s" % (backend, _screen(w))
        before = len(_commands(tmp_path, backend))
        _move_down(w, C_LATER_LINE - C_BREAK_LINE)
        w.key(CTRL_G, "b", delay=0.6)
        assert _never_stopped(w, NOT_INTERRUPTED_S), \
            "%s must not be interrupted for a breakpoint:\n%s" \
            % (backend, _screen(w))
        assert _sent(tmp_path, backend)[before:] == [], \
            "nothing should be sent to the busy %s; it got %r" \
            % (backend, _sent(tmp_path, backend)[before:])
        assert _breakpoint_drawn(w, C_LATER_LINE, hollow=True), \
            "line %d should stay hollow until the next stop:\n%s" \
            % (C_LATER_LINE, _screen(w))
        w.key(ALT_X, delay=0.5)


# ---- the answers of sdb, dbx and xdb, read while the editor keeps its keys
#
# The stub holds back one command (XWPE_STUB_STALL) until the editor sends
# another line or the test writes to its release pipe, so the editor waits
# for the answer exactly as long as the test needs.

ESC = "\033"
CTRL_C = "\x03"           # the terminal's interrupt character
F8 = "\033[19~"           # Step (step over)
NOTHING_MORE_S = 3        # a kept key would have acted by then
ANSWER_TIMEOUT_S = 2      # the deadline row's XWPE_DEBUG_ANSWER_TIMEOUT

# What each backend is asked for watch x, and for the breakpoint on line 8.
WATCH_NAME = "x"
WATCH_ASKED = {"sdb": "x/", "dbx": "print x", "xdb": "p x"}
BREAK_ASKED = {"sdb": "8 b", "dbx": 'stop at "p.c":8', "xdb": "b p.c:8"}
# What each backend is sent to take a breakpoint off line 8 whose number it
# never gave: by its line where it can (sdb, dbx), all at once where it
# cannot (xdb, whose other breakpoints are then set again).
CLEAR_BY_PLACE = {"sdb": "8 d", "dbx": 'clear "p.c":8', "xdb": "db *"}
WAITING_FMT = "waiting for %s to answer `%s`"
NOT_ANSWERED_FMT = "%s did not answer `%s`"
LATE_WATCH = "y"          # the stub's value for it is not x's
LATE_VALUE_WORDS = "y: 7"
EARLY_VALUE_WORDS = "y: 3"
STACK_ASKED = {"sdb": "t", "dbx": "where", "xdb": "t"}


def _stub_log(tmp_path, backend):
    """Everything the stub logged: its commands, holds and armed lines."""
    log = tmp_path / (backend + ".log")
    if not log.exists():
        return ""
    return log.read_text()


def _stub_holds(w, tmp_path, backend, command):
    """Wait until the stub says it holds @command: the editor is then waiting
    for the answer."""
    assert _wait_for(w, lambda: ("STALL " + command)
                     in _stub_log(tmp_path, backend), START_TIMEOUT_S), \
        "the editor should have sent %r (stub log:\n%s):\n%s" \
        % (command, _stub_log(tmp_path, backend), _screen(w))


def _release(tmp_path, backend):
    """Let the held command be answered."""
    with open(str(tmp_path / (backend + ".release")), "w") as fh:
        fh.write("go\n")


def _status_row_says(w, text, timeout):
    """Wait until the bottom row carries @text."""
    return _wait_for(w, lambda: text in _bottom_row(w), timeout)


def _quit_and_see_the_stub_gone(w, backend):
    """Debug > Quit ends the debugger the session ran."""
    w.menu(ALT.DEBUG, "q", delay=0.5)
    assert _wait_for(w, lambda: not _stub_running(w, backend),
                     STEP_TIMEOUT_S), \
        "Debug > Quit should end %s:\n%s" % (backend, _screen(w))


def _armed_at_last(tmp_path, backend):
    """The lines the stub had armed after its last breakpoint command."""
    rows = [row[len("ARMED"):].split() for row
            in _stub_log(tmp_path, backend).splitlines()
            if row.startswith("ARMED")]
    if not rows:
        return None
    return [int(each) for each in rows[-1]]


def _watch_held_at_a_step(w, tmp_path, backend):
    """Stop on line 8, watch x, then F8: at the step's stop the backend is
    asked for x again, and the stub holds that second question."""
    _start_session(w, tmp_path, backend)
    _add_watch_keys(w, WATCH_NAME)
    assert _wait_for(w, lambda: _watch_text_shown(w, WATCH_FILLED_WORDS),
                     STEP_TIMEOUT_S), \
        "the watch should read %r:\n%s" % (WATCH_FILLED_WORDS, _screen(w))
    w.key(F8, delay=0.3)
    _stub_holds(w, tmp_path, backend, WATCH_ASKED[backend])


def _keys_reach_the_editor_while_it_answers(tmp_path, backend):
    with _session(tmp_path, backend, stall=WATCH_ASKED[backend],
                  stall_nth=2) as w:
        _watch_held_at_a_step(w, tmp_path, backend)
        w.key(CTRL_G, "k", delay=0.5)
        waiting = WAITING_FMT % (backend, WATCH_ASKED[backend])
        assert _status_row_says(w, waiting, STEP_TIMEOUT_S), \
            "Ctrl-G K should be refused with the waiting line:\n%s" \
            % _screen(w)
        assert " Stack " not in _screen(w), \
            "Ctrl-G K must wait for %s's answer, not ask it:\n%s" \
            % (backend, _screen(w))
        assert STACK_ASKED[backend] not in _sent(tmp_path, backend), \
            "%s must not be asked for the stack while it answers: %r" \
            % (backend, _sent(tmp_path, backend))
        w.save()
        assert w.text() == C_PROG, \
            "a key typed while %s answered landed in the file:\n%s" \
            % (backend, _screen(w))
        _release(tmp_path, backend)
        assert _wait_for(w, lambda: _mark_line(w) == C_LATER_LINE,
                         STEP_TIMEOUT_S), \
            "once %s answers the stop should be marked (mark on %s):\n%s" \
            % (backend, _mark_line(w), _screen(w))


# @scenario SC-debug-49
def test_sdb_keys_reach_the_editor_while_it_answers(tmp_path):
    """While sdb has not answered a watch at a step's stop, Ctrl-G K reaches
    the editor and is refused with the waiting line, which names the
    command; no Stack window opens and its letter is not typed into the
    file.  Once sdb answers, the stop is marked.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _keys_reach_the_editor_while_it_answers(tmp_path, "sdb")


# @scenario SC-debug-49
def test_dbx_keys_reach_the_editor_while_it_answers(tmp_path):
    """As under sdb, while dbx has not answered a watch at a step's stop.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _keys_reach_the_editor_while_it_answers(tmp_path, "dbx")


# @scenario SC-debug-49
def test_xdb_keys_reach_the_editor_while_it_answers(tmp_path):
    """As under sdb, while xdb has not answered a watch at a step's stop.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb."""
    _keys_reach_the_editor_while_it_answers(tmp_path, "xdb")


ANSWER_ENDS = {
    "esc": {"key": ESC, "env": None, "within": False},
    "ctrl_c": {"key": CTRL_C, "env": None, "within": False},
    "deadline": {"key": None,
                 "env": {"XWPE_DEBUG_ANSWER_TIMEOUT": str(ANSWER_TIMEOUT_S)},
                 "within": True},
}


def _stalled_answer_ends(tmp_path, backend, how):
    case = ANSWER_ENDS[how]
    says = NOT_ANSWERED_FMT % (backend, WATCH_ASKED[backend])
    if case["within"]:
        says += " within"
    with _session(tmp_path, backend, stall=WATCH_ASKED[backend], stall_nth=2,
                  extra=case["env"]) as w:
        _watch_held_at_a_step(w, tmp_path, backend)
        waiting = WAITING_FMT % (backend, WATCH_ASKED[backend])
        assert _status_row_says(w, waiting, STEP_TIMEOUT_S), \
            "the waiting line should come:\n%s" % _screen(w)
        if case["key"]:
            w.key(case["key"], delay=0.5)
        assert _status_row_says(w, says, STEP_TIMEOUT_S), \
            "the wait should end with %r:\n%s" % (says, _screen(w))
        _quit_and_see_the_stub_gone(w, backend)


# @scenario SC-debug-50
@pytest.mark.parametrize("how", sorted(ANSWER_ENDS))
def test_sdb_stalled_answer_ends_with_a_message_and_quit_works(tmp_path, how):
    """An sdb that does not answer a watch at a stop: the waiting line comes,
    Esc, Ctrl-C or the deadline ends the wait with a line naming the
    command, and Debug > Quit then ends the session.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _stalled_answer_ends(tmp_path, "sdb", how)


# @scenario SC-debug-50
@pytest.mark.parametrize("how", sorted(ANSWER_ENDS))
def test_dbx_stalled_answer_ends_with_a_message_and_quit_works(tmp_path, how):
    """As under sdb, for a dbx that does not answer a watch at a stop.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _stalled_answer_ends(tmp_path, "dbx", how)


# @scenario SC-debug-50
@pytest.mark.parametrize("how", sorted(ANSWER_ENDS))
def test_xdb_stalled_answer_ends_with_a_message_and_quit_works(tmp_path, how):
    """As under sdb, for an xdb that does not answer a watch at a stop.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb."""
    _stalled_answer_ends(tmp_path, "xdb", how)


def _answer_after_a_cancel(tmp_path, backend):
    with _session(tmp_path, backend, stall=WATCH_ASKED[backend],
                  stall_nth=2) as w:
        _watch_held_at_a_step(w, tmp_path, backend)
        waiting = WAITING_FMT % (backend, WATCH_ASKED[backend])
        assert _status_row_says(w, waiting, STEP_TIMEOUT_S), \
            "the waiting line should come:\n%s" % _screen(w)
        w.key(ESC, delay=0.5)
        says = NOT_ANSWERED_FMT % (backend, WATCH_ASKED[backend])
        assert _status_row_says(w, says, STEP_TIMEOUT_S), \
            "Esc should end the wait:\n%s" % _screen(w)
        _add_watch_keys(w, LATE_WATCH)
        assert _wait_for(w, lambda: _watch_text_shown(w, LATE_VALUE_WORDS)
                         or _watch_text_shown(w, EARLY_VALUE_WORDS),
                         STEP_TIMEOUT_S), \
            "the watch should get an answer; %s was sent %r:\n%s" \
            % (backend, _sent(tmp_path, backend), _screen(w))
        assert _watch_text_shown(w, LATE_VALUE_WORDS), \
            "the watch took another command's answer; %s was sent %r:\n%s" \
            % (backend, _sent(tmp_path, backend), _screen(w))


# @scenario SC-debug-50
def test_sdb_answer_after_a_cancel_is_not_taken_for_the_next(tmp_path):
    """sdb answers the watch only after the user gave up on it: that late
    answer is dropped, and the next command, a watch on y, shows its own
    value, not x's.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _answer_after_a_cancel(tmp_path, "sdb")


# @scenario SC-debug-50
def test_dbx_answer_after_a_cancel_is_not_taken_for_the_next(tmp_path):
    """As under sdb, for dbx's late answer to a watch given up on.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _answer_after_a_cancel(tmp_path, "dbx")


# @scenario SC-debug-50
def test_xdb_answer_after_a_cancel_is_not_taken_for_the_next(tmp_path):
    """As under sdb, for xdb's late answer to a watch given up on.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb."""
    _answer_after_a_cancel(tmp_path, "xdb")


def _breakpoint_given_up(tmp_path, backend):
    _need("gcc")
    with _session(tmp_path, backend, stall=BREAK_ASKED[backend]) as w:
        _move_down(w, C_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        _stub_holds(w, tmp_path, backend, BREAK_ASKED[backend])
        w.key(ESC, delay=0.5)
        says = NOT_ANSWERED_FMT % (backend, BREAK_ASKED[backend])
        assert _status_row_says(w, says, STEP_TIMEOUT_S), \
            "the wait should end with %r:\n%s" % (says, _screen(w))
        w.key(CTRL_G, "r", delay=1.0)
        assert _wait_for(w, lambda: _mark_line(w) == C_BREAK_LINE,
                         START_TIMEOUT_S), \
            "the start set up again should stop on line %d; %s was sent " \
            "%r:\n%s" % (C_BREAK_LINE, backend, _sent(tmp_path, backend),
                         _screen(w))
        log = _stub_log(tmp_path, backend).split("RELEASE", 1)[1]
        clear = "CMD %s\n" % CLEAR_BY_PLACE[backend]
        again = "CMD %s\n" % BREAK_ASKED[backend]
        assert clear in log and again in log and \
            log.index(clear) < log.index(again), \
            "the line %s may have set should be cleared before it is set " \
            "again:\n%s" % (backend, log)
        w.key(CTRL_G, "b", delay=0.6)
        assert _wait_for(w, lambda: _armed_at_last(tmp_path, backend) == [],
                         STEP_TIMEOUT_S), \
            "with its mark off, %s must have no breakpoint left on the line " \
            "(stub log:\n%s):\n%s" \
            % (backend, _stub_log(tmp_path, backend), _screen(w))
        _quit_and_see_the_stub_gone(w, backend)


# @scenario SC-debug-50
def test_sdb_breakpoint_given_up_is_not_set_twice(tmp_path):
    """sdb's answer to the breakpoint of the start is given up on, though
    sdb sets it.  The next Ctrl-G R sets sdb up again, taking the line off
    before setting it once more; once the mark is taken off, sdb has no
    breakpoint left there.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _breakpoint_given_up(tmp_path, "sdb")


# @scenario SC-debug-50
def test_dbx_breakpoint_given_up_is_not_set_twice(tmp_path):
    """As under sdb: dbx's number for the breakpoint given up on was never
    read, so the line is cleared by its place before it is set again.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _breakpoint_given_up(tmp_path, "dbx")


# @scenario SC-debug-50
def test_xdb_breakpoint_given_up_is_not_set_twice(tmp_path):
    """As under sdb: xdb takes a breakpoint off only by the number it gave,
    which was never read, so all are taken off and the user's set again.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb."""
    _breakpoint_given_up(tmp_path, "xdb")


def _keys_typed_during_the_start(tmp_path, backend):
    _need("gcc")
    with _session(tmp_path, backend, stall=BREAK_ASKED[backend]) as w:
        _move_down(w, C_BREAK_LINE - 1)
        w.key(CTRL_G, "b", delay=0.4)
        w.key(CTRL_G, "r", delay=1.0)
        _stub_holds(w, tmp_path, backend, BREAK_ASKED[backend])
        w.key(F8, delay=0.3)
        w.key(CTRL_G, "k", delay=0.5)
        waiting = WAITING_FMT % (backend, BREAK_ASKED[backend])
        assert _status_row_says(w, waiting, STEP_TIMEOUT_S), \
            "Ctrl-G K should be refused with the waiting line:\n%s" \
            % _screen(w)
        assert " Stack " not in _screen(w), \
            "Ctrl-G K must wait for %s's answer, not ask it:\n%s" \
            % (backend, _screen(w))
        _release(tmp_path, backend)
        assert _wait_for(w, lambda: _mark_line(w) == C_LATER_LINE,
                         START_TIMEOUT_S), \
            "the F8 typed during the start should step once the start " \
            "stopped (mark on %s); %s was sent %r:\n%s" \
            % (_mark_line(w), backend, _sent(tmp_path, backend), _screen(w))
        w._drain(NOTHING_MORE_S)
        assert _mark_line(w) == C_LATER_LINE, \
            "only the F8 typed should step (mark on %s):\n%s" \
            % (_mark_line(w), _screen(w))
        w.save()
        assert w.text() == C_PROG, \
            "a key typed while %s started landed in the file:\n%s" \
            % (backend, _screen(w))


# @scenario SC-debug-49
def test_sdb_keys_typed_during_the_start_are_kept_or_refused(tmp_path):
    """While sdb has not answered the breakpoint of the start, F8 is kept
    and steps once the start has stopped on the breakpoint, and Ctrl-G K is
    refused with the waiting line, its letter not typed into the file.

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    _keys_typed_during_the_start(tmp_path, "sdb")


# @scenario SC-debug-49
def test_dbx_keys_typed_during_the_start_are_kept_or_refused(tmp_path):
    """As under sdb, while dbx has not answered the breakpoint of the start.

    Limit: the stub proves xwpe's half of the protocol, not a real dbx."""
    _keys_typed_during_the_start(tmp_path, "dbx")


# @scenario SC-debug-49
def test_xdb_keys_typed_during_the_start_are_kept_or_refused(tmp_path):
    """As under sdb, while xdb has not answered the breakpoint of the start.

    Limit: the stub proves xwpe's half of the protocol, not a real xdb."""
    _keys_typed_during_the_start(tmp_path, "xdb")


SDB_STEP_ROWS_ENV = {"XWPE_STUB_SDB_STEP_ROWS": "1"}


# @scenario SC-debug-03
def test_sdb_step_reported_by_its_source_row_is_marked(tmp_path):
    """sdb reports a step by its source row ("9: ...") under the row that
    names the function and its file ("main() in p.c"), here with a one-word
    row between them: F8 marks line 9 of p.c.  Reading a one-word row as a
    possible "f() in file" row must stay inside that row (the sanitizer
    build shows a read before its start otherwise).

    Limit: the stub proves xwpe's half of the protocol, not a real sdb."""
    with _session(tmp_path, "sdb", extra=SDB_STEP_ROWS_ENV) as w:
        _start_session(w, tmp_path, "sdb")
        w.key(F8, delay=0.5)
        assert _wait_for(w, lambda: _mark_line(w) == C_LATER_LINE,
                         STEP_TIMEOUT_S), \
            "F8 should mark line %d (mark on %s); sdb was sent %r:\n%s" \
            % (C_LATER_LINE, _mark_line(w), _sent(tmp_path, "sdb"),
               _screen(w))
        assert w.alive(), "the editor should survive sdb's step report"
