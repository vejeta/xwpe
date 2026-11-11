"""Scala source-level debugging via Bloop/scala-debug-adapter over DAP, in wpe.

A .scala file is debugged through the DAP bridge, but the JVM has no standalone
DAP server: xwpe runs the BSP bootstrap (we_bsp.c) against scala-cli (which
bundles Bloop, hosting scala-debug-adapter), gets back a tcp:// DAP endpoint,
and connects the existing reverse-TCP DAP engine to it.  Same editor keys as the
Go/Rust slices (Ctrl-G B/R/W, F8); the build server compiles, so there is no
xwpe compile step.

Requires `scala-cli`.  Skips otherwise.  Slow: the first Ctrl-G R starts a JVM
build server and (on a cold coursier cache) resolves the Scala toolchain.
"""
import os
import re
import shutil
import signal
import time

import pytest

import procinfo
from wpe_driver import WpeSession, ALT, WAIT_SCALE

# Iterative factorial; `f` grows 1 -> 2 -> 6 -> 24 -> ...  Breakpoint on line 6
# (the multiply), reached every loop iteration.
PROG = (
    "object Factorial:\n"
    "  def main(args: Array[String]): Unit =\n"
    "    var f = 1L\n"
    "    var i = 1\n"
    "    while i <= 10 do\n"
    "      f = f * i\n"                 # line 6: breakpoint
    "      i = i + 1\n"
    "    println(s\"factorial(10) = $f\")\n"
)

CTRL_G = "\x07"
CTRL_N = "\x0e"     # Emacs "down a line" -- pyte delivers it reliably
F8 = "\033[19~"     # Step (over)

pytestmark = pytest.mark.skipif(
    shutil.which("scala-cli") is None,
    reason="scala-cli required (bundles Bloop / scala-debug-adapter)")


def _text(w):
    return "\n".join(w.display())


def _status_line_no(w):
    m = re.search(r"(\d+):", w.display()[20])
    return int(m.group(1)) if m else -1


def _mark_line(w):
    """The source line carrying the current-line mark, or None."""
    return w.mark_line()


def _watch_val(w, name):
    pat = re.compile(re.escape(name) + r":\s*(-?\d+)")
    for line in w.display():
        m = pat.search(line)
        if m:
            return int(m.group(1))
    return None


# @scenario SC-debug-24
def test_scala_dap_run_step_watch(tmp_path):
    """scala-cli/Bloop starts via BSP, scala-debug-adapter stops at the
    breakpoint over DAP, and a watch on `f` updates as the loop runs."""
    with WpeSession(str(tmp_path), PROG, filename="factorial.scala", wait=2.0) as w:
        for _ in range(5):                 # move the cursor to line 6
            w.key(CTRL_N, delay=0.12)
        w.key(CTRL_G, "b", delay=1.0)      # breakpoint on line 6
        # Park the cursor on line 8, away from the breakpoint: only the
        # adapter can bring the stop back to line 6, so the mark asserted
        # below is the stop's and not where the cursor already was.
        for _ in range(2):
            w.key(CTRL_N, delay=0.12)
        # First Run boots a JVM build server (+ cold-cache coursier resolve):
        # give it plenty of time.  That bootstrap is synchronous, but the
        # adapter's handshake after it runs on the descriptor loop, so the
        # editor takes keys, Messages counts the seconds and Esc ends it.
        w.key(CTRL_G, "r", delay=60.0)
        time.sleep(3.0)
        w._drain(3.0)
        assert w.alive(), "wpe died starting the Scala BSP/DAP session"
        assert _status_line_no(w) == 6, \
            "DAP should stop at the breakpoint on line 6 (status:\n%s)" % _text(w)
        assert _mark_line(w) == 6, \
            "the current-line mark should sit on the breakpoint line 6, not " \
            "on line 8 where the cursor was parked:\n%s" % _text(w)

        w.key(F8, delay=3.0)               # Step (over) one source line
        w._drain(1.5)
        assert w.alive(), "wpe died stepping the Scala program"
        stepped = _mark_line(w)
        assert stepped is not None and stepped != 6, \
            "F8 should move the mark off the breakpoint line 6 (now %s):\n%s" \
            % (stepped, _text(w))

        w.key(CTRL_G, "w", delay=1.5)      # Make Watch dialog
        w.key("f", delay=0.1)
        w.key("\r", delay=2.5)             # JVM evaluate is slower than gdb's
        w._drain(1.5)

        # continue around the loop; f must appear and grow past 1 (factorial
        # 1,1,2,6,24,...).  The watch line refreshes on each stop, so poll it
        # across the iterations rather than insisting on one early value.
        seen = set()
        v = _watch_val(w, "f")
        if v is not None:
            seen.add(v)
        for _ in range(7):
            w.key(CTRL_G, "r", delay=2.5)
            w._drain(1.0)
            v = _watch_val(w, "f")
            if v is not None:
                seen.add(v)
        assert w.alive(), "wpe died continuing the Scala program"
        assert seen, "the watch on f never showed a numeric value:\n%s" % _text(w)
        assert any(v > 1 for v in seen), \
            "watch on `f` never grew past 1 (saw %s)" % sorted(seen)


BREAK_LINE = 6
START_S = 150            # a build server, cold, before scaling
PROGRAM_ENDS_S = 10      # the program ends within this after Quit
POLL_S = 0.2
JAVA = "java"            # the kernel's command name of a JVM


def _drain_until(w, predicate, seconds):
    """Drain the editor until predicate() holds or the scaled time ends."""
    deadline = time.time() + seconds * WAIT_SCALE
    while time.time() < deadline:
        w._drain(POLL_S)
        if predicate():
            return True
    return predicate()


def _wait_until(predicate, seconds):
    """Wait, without the editor, until predicate() holds or the scaled time
    ends."""
    deadline = time.time() + seconds * WAIT_SCALE
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(POLL_S)
    return predicate()


def _stopped_with_its_program(w, workdir):
    """Stop the session at the breakpoint; return (program, server): the
    program's JVM, found by its directory, and the build server whose
    child it is (Bloop, whether this session started it or it ran
    already)."""
    for _ in range(BREAK_LINE - 1):
        w.key(CTRL_N, delay=0.12)
    w.key(CTRL_G, "b", delay=1.0)
    w.key(CTRL_G, "r", delay=1.0)
    assert _drain_until(w, lambda: _mark_line(w) == BREAK_LINE, START_S), \
        "the session should stop at line %d:\n%s" % (BREAK_LINE, _text(w))
    programs = procinfo.processes_in(workdir, JAVA)
    assert len(programs) == 1, \
        "one program JVM should run in %s (found %s)" % (workdir, programs)
    return programs[0], procinfo.parent_of(programs[0])


def _end_program_if_left(workdir):
    """A failed row must not leave its program running under the shared
    build server: end that JVM, found by its directory, and nothing else."""
    for pid in procinfo.processes_in(workdir, JAVA):
        os.kill(pid, signal.SIGKILL)


# @scenario SC-debug-47
@pytest.mark.skipif(not procinfo.has_procfs(),
                    reason="finds the program by its directory in /proc")
def test_quit_ends_the_scala_program(tmp_path):
    """Debug > Quit ends the program, not only the editor's side of the
    session: the build server runs no JVM of this session's directory any
    more, and the server itself is left running for others."""
    workdir = str(tmp_path)
    try:
        with WpeSession(workdir, PROG, filename="factorial.scala",
                        wait=2.0) as w:
            program, server = _stopped_with_its_program(w, workdir)
            w.menu(ALT.DEBUG, "q", delay=0.5)
            assert _drain_until(
                w, lambda: not procinfo.children_in(server, workdir, JAVA),
                PROGRAM_ENDS_S), \
                "the program (pid %d) should end on Quit:\n%s" \
                % (program, _text(w))
            assert not procinfo.all_ended([server]), \
                "the build server (pid %d) is shared and goes on" % server
    finally:
        _end_program_if_left(workdir)


# @scenario SC-debug-47
@pytest.mark.skipif(not procinfo.has_procfs(),
                    reason="finds the program by its directory in /proc")
def test_sigterm_to_the_editor_ends_the_scala_program(tmp_path):
    """SIGTERM to the editor (a logout, "kill PID") during a Scala session
    saves the work and ends the program too: none is left running under
    the build server."""
    workdir = str(tmp_path)
    try:
        with WpeSession(workdir, PROG, filename="factorial.scala",
                        wait=2.0) as w:
            program, server = _stopped_with_its_program(w, workdir)
            os.kill(w.proc.pid, signal.SIGTERM)
            assert _wait_until(
                lambda: not procinfo.children_in(server, workdir, JAVA),
                PROGRAM_ENDS_S), \
                "the program (pid %d) should end with the editor" % program
    finally:
        _end_program_if_left(workdir)


def _close_the_terminal(w):
    """Close the editor's terminal, as a closed terminal window does: the
    editor reads the end of its input and gets SIGHUP."""
    os.close(w.master_fd)
    if w.input_fd != w.master_fd:
        os.close(w.input_fd)
    w.master_fd = w.input_fd = -1


def _run_to_its_end(w):
    """Take the breakpoint away and continue: the program runs to its end
    with the session open."""
    w.key(CTRL_G, "b", delay=1.0)
    w.key(CTRL_G, "r", delay=1.0)


EDITOR_ENDS = {
    "alt_x": lambda w: w.key("\033x", delay=0.5),
    "sighup": lambda w: os.kill(w.proc.pid, signal.SIGHUP),
    "terminal_closed": _close_the_terminal,
    "program_runs_to_its_end": _run_to_its_end,
}


def _no_program_left(w, workdir, server):
    """True once the build server runs no program of this session; the
    editor is drained while it still has a terminal to drain."""
    if w.alive() and w.master_fd >= 0:
        w._drain(POLL_S)
    return not procinfo.children_in(server, workdir, JAVA)


# @scenario SC-debug-47
@pytest.mark.skipif(not procinfo.has_procfs(),
                    reason="finds the program by its directory in /proc")
@pytest.mark.parametrize("end", sorted(EDITOR_ENDS))
def test_every_end_of_a_scala_session_ends_its_program(tmp_path, end):
    """However the session ends with the editor running its course (Alt-X,
    a hang-up, a closed terminal, the program running to its end), the
    build server runs no program of this session's folder afterwards."""
    workdir = str(tmp_path)
    try:
        with WpeSession(workdir, PROG, filename="factorial.scala",
                        wait=2.0) as w:
            program, server = _stopped_with_its_program(w, workdir)
            EDITOR_ENDS[end](w)
            assert _wait_until(
                lambda: _no_program_left(w, workdir, server),
                PROGRAM_ENDS_S), \
                "the program (pid %d) should end on %s" % (program, end)
    finally:
        _end_program_if_left(workdir)


# @scenario SC-debug-47
@pytest.mark.skipif(not procinfo.has_procfs(),
                    reason="finds the program by its directory in /proc")
def test_a_killed_editors_test_leaves_no_scala_program(tmp_path):
    """A test that ends its session by killing the editor (every session's
    close, SIGKILL to the editor's group) leaves no program behind under
    the shared build server: the program is held at a breakpoint, ignores
    SIGTERM, and would otherwise stay for as long as the server runs."""
    workdir = str(tmp_path)
    try:
        w = WpeSession(workdir, PROG, filename="factorial.scala", wait=2.0)
        try:
            program, server = _stopped_with_its_program(w, workdir)
        finally:
            w.close()
        left = procinfo.children_in(server, workdir, JAVA)
        assert not left, \
            "the session's close left its program running under the build " \
            "server %d: %s" % (server, left)
    finally:
        _end_program_if_left(workdir)
