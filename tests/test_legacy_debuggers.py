"""The sdb, dbx and xdb debugger backends, driven through a stub of each.

sdb, dbx and xdb are proprietary debuggers of commercial UNIX: none is in
Debian, none has a free implementation and none would run on Linux.  xwpe's
own enum names them (classic sdb with prompt "*", "dbx -i" with "(dbx)", HP
"xdb -L" with ">"), so each is stood in here by a stub that answers only the
commands xwpe's source says it sends, in the wording xwpe's own parsers look
for, and that logs every command it is given.

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

from wpe_driver import WpeSession, WAIT_SCALE

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

STUB_SOURCE = r'''
import os
import sys

BACKEND = os.path.basename(sys.argv[0])
PROMPT = {"sdb": "*", "dbx": "(dbx) ", "xdb": ">"}[BACKEND]
LOG = os.environ.get("XWPE_STUB_LOG", "")
SOURCE = os.environ.get("XWPE_STUB_SOURCE", "p.c")

armed = []          # breakpoint lines, in the order they were armed
current = [0]       # line the program stands at; 0 before the first run


def note(text):
    if LOG:
        with open(LOG, "a") as fh:
            fh.write(text + "\n")


def say(text):
    sys.stdout.write(text)
    sys.stdout.flush()


def line(text):
    say(text + "\n")


def prompt():
    say(PROMPT)


def arm(at):
    armed.append(at)
    return len(armed)


def disarm_line(at):
    for i, each in enumerate(armed):
        if each == at:
            del armed[i]
            return


def disarm_number(num):
    if 1 <= num <= len(armed):
        del armed[num - 1]


def stop_at():
    """Print the stop where this backend's parser looks for it."""
    n = current[0]
    if BACKEND == "sdb":
        line("BREAKPOINT at %d in main" % n)
        line("%s:%d" % (SOURCE, n))
    elif BACKEND == "dbx":
        line('stopped in main at line %d in file "%s"' % (n, SOURCE))
        line("%d       int x = 3;" % n)
    else:
        line("%s:main:%d" % (SOURCE, n))


def next_stop():
    """The lowest armed line ahead of where the program stands, or None.

    A real debugger goes on past its current position, so a breakpoint behind
    it does not stop this run; Go to cursor needs that to reach a line further
    down while an earlier breakpoint is still armed."""
    ahead = [each for each in armed if each > current[0]]
    if not ahead:
        return None
    return min(ahead)


def run(first):
    """Answer a run or a continue: the next stop, or the program's end."""
    if first and BACKEND in ("dbx", "xdb"):
        line("Running: p.e")
    stop = next_stop()
    if stop is not None:
        current[0] = stop
        stop_at()
        return
    if BACKEND == "sdb":
        line("Program exited")
    elif BACKEND == "dbx":
        line("program exited")
    else:
        line("Child process terminated normally")


def step():
    current[0] = current[0] + 1
    stop_at()


def answer_sdb(cmd):
    if cmd == "e":                      # the file the stop was in
        line('"%s"' % SOURCE)
        return
    if cmd.startswith("e "):            # take the file the next command names
        return
    parts = cmd.split()
    if len(parts) == 2 and parts[0].isdigit() and parts[1] == "b":
        arm(int(parts[0]))
        return
    if len(parts) == 2 and parts[0].isdigit() and parts[1] == "d":
        disarm_line(int(parts[0]))
        return
    if cmd == "D":
        del armed[:]
        return
    if cmd == "b":                      # the editor's own start breakpoint
        line("breakpoint 1 at main:%d" % current[0])
        return
    if cmd == "t":
        line(">  main [%s:%d]" % (SOURCE, current[0]))
        return
    if cmd.split()[0] == "r":
        run(True)
        return
    if cmd == "c":
        run(False)
        return
    if cmd in ("S", "s"):
        step()
        return
    if cmd.endswith("/"):               # a watch
        line("%s: 3" % cmd[:-1])
        return
    if cmd == "q":
        sys.exit(0)


def answer_dbx(cmd):
    if cmd.startswith("stop at ") or cmd.startswith("stop in "):
        num = arm(int(cmd.rsplit(":", 1)[1]))
        line("(%d) %s" % (num, cmd))    # dbx answers with the number first
        return
    if cmd == "delete all":
        del armed[:]
        return
    if cmd.startswith("delete "):
        disarm_number(int(cmd.split()[1]))
        return
    if cmd.startswith("run"):
        run(True)
        return
    if cmd == "cont":
        run(False)
        return
    if cmd in ("next", "step"):
        step()
        return
    if cmd == "where":
        line('>  main  [line %d in file "%s"]' % (current[0], SOURCE))
        return
    if cmd.startswith("print "):
        line("%s = 3" % cmd.split(None, 1)[1])
        return
    if cmd == "quit":
        sys.exit(0)


def answer_xdb(cmd):
    if cmd == "sm":                     # xdb's own setting, sent at the start
        return
    if cmd.startswith("b "):
        if ":" in cmd:
            num = arm(int(cmd.rsplit(":", 1)[1]))
        else:
            num = arm(current[0])
        line("Added:")                  # xdb gives the number on the next line
        line("b%d  break at %s" % (num, cmd[2:]))
        return
    if cmd == "db *":
        del armed[:]
        return
    if cmd.startswith("db "):
        disarm_number(int(cmd.split()[1]))
        return
    if cmd.split()[0] == "r":
        run(True)
        return
    if cmd == "c":
        run(False)
        return
    if cmd in ("S", "s"):
        step()
        return
    if cmd == "t":
        line(">  main [%s:%d]" % (SOURCE, current[0]))
        return
    if cmd.startswith("p "):
        line("%s = 3" % cmd.split(None, 1)[1])
        return
    if cmd == "y":                      # the "really quit?" answer
        sys.exit(0)
    if cmd == "q":
        line("Really quit?")
        return


ANSWER = {"sdb": answer_sdb, "dbx": answer_dbx, "xdb": answer_xdb}[BACKEND]

prompt()
while True:
    raw = sys.stdin.buffer.readline()
    if not raw:
        break
    cmd = raw.decode("utf-8", "replace").rstrip("\r\n")
    note("CMD " + cmd)
    ANSWER(cmd)
    prompt()
'''.lstrip()


def _need(*tools):
    """Skip the test, naming the first missing binary."""
    for tool in tools:
        if shutil.which(tool) is None:
            pytest.skip("no %s" % tool)


def _stub_env(tmp_path, backend):
    """Write the stub for @backend and return the environment that runs it.

    The stub is one script under the debugger's own name, so the backend is
    the name xwpe exec's, exactly as the real one would be."""
    bindir = tmp_path / "bin"
    bindir.mkdir(exist_ok=True)
    stub = bindir / backend
    stub.write_text("#!%s\n%s" % (sys.executable, STUB_SOURCE))
    stub.chmod(0o755)
    return {
        "PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
        "XWPE_STUB_LOG": str(tmp_path / (backend + ".log")),
        "XWPE_STUB_SOURCE": C_SOURCE,
    }


def _session(tmp_path, backend):
    """A session on p.c with @backend forced through the options file.

    The options file is how a user picks a backend the dialog of this build
    does not offer, so the test does not depend on the dialog's rows."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text(
        "[Programming]\nDebugger : %d\n" % DEBUGGER_TYPE[backend])
    return WpeSession(str(tmp_path), C_PROG, filename=C_SOURCE, wait=1.5,
                      env_extra=_stub_env(tmp_path, backend))


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
