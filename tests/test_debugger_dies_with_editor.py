"""A debugger does not outlive an editor that is killed outright.

A line debugger runs in a session of its own, so Ctrl-C typed in the editor
does not reach it.  The price: when the editor's terminal closes, the
hangup no longer reaches the debugger by session either.  Closing the
terminal is handled (the editor gets SIGHUP and ends the debugger), but a
SIGKILL, or the out-of-memory killer, gives the editor no time to do
anything.  A debugger left behind with a program in a loop keeps a core
busy until someone finds it.

These rows kill the editor with SIGKILL while it is the controlling process
of its terminal, as under `xterm -e wpe` or `exec wpe`, during a pdb run
whose script sleeps and an a68g run whose program loops, and ask the
process table whether the debugger (which is also the program) is gone.
Linux and FreeBSD ask the system to end the child with the editor; the
other systems cannot, and these rows are skipped there.
"""
import os
import signal
import sys
import time

import pytest

import procinfo
from wpe_driver import WpeSession, WAIT_SCALE
from test_debugger_integration import (
    A68_PAUSE_BREAK_LINE, A68_PAUSE_PROG, DAP_GO_PROG, DLV_PIDFILE,
    PY_SLEEP_BREAK_LINE, PY_SLEEP_PROG, START_TIMEOUT_S, _children,
    _dlv_pid, _need, _run_go, _stop_then_run_on, _write_script)

pytestmark = pytest.mark.skipif(
    not sys.platform.startswith(("linux", "freebsd")),
    reason="only Linux and FreeBSD can end a child with its parent")

GONE_WITHIN_S = 5            # after the kill, before scaling
POLL_S = 0.1


def _is_gone(pid):
    """True once pid is out of the table, or ended and waiting to be reaped."""
    return procinfo.run_state(pid) in (procinfo.GONE, procinfo.ZOMBIE)


def _wait_gone(pid):
    deadline = time.time() + GONE_WITHIN_S * WAIT_SCALE
    while time.time() < deadline:
        if _is_gone(pid):
            return True
        time.sleep(POLL_S)
    return _is_gone(pid)


def _kill_editor_and_check(w, debugger_name):
    """SIGKILL the editor while its debugger runs the program; True if the
    debugger is gone within the bound.  The debugger is also stopped by hand
    if it survives, by the PID the test recorded."""
    pids = _children(w, debugger_name)
    assert len(pids) == 1, "the editor's one %s: %s" % (debugger_name, pids)
    debugger = pids[0]
    os.kill(w.proc.pid, signal.SIGKILL)
    w.proc.wait()
    gone = _wait_gone(debugger)
    if not gone:
        os.kill(debugger, signal.SIGKILL)
    return gone, debugger


# @scenario SC-debug-17
def test_pdb_does_not_outlive_a_killed_editor(tmp_path):
    """pdb, with its script asleep, is gone after the editor is killed."""
    _need("python3")
    with WpeSession(str(tmp_path), PY_SLEEP_PROG, filename="p.py",
                    wait=1.5) as w:
        _stop_then_run_on(w, PY_SLEEP_BREAK_LINE, START_TIMEOUT_S,
                          in_messages=False)
        gone, pid = _kill_editor_and_check(w, "python3")
    assert gone, "pdb (pid %d) is still running after the editor was killed" \
        % pid


REVERSE_TCP_START_TIMEOUT_S = 60     # the stand-in must not be ended first

# A dlv stand-in that dials back in, writes its PID down and then sleeps,
# whatever happens to its connection: only a hangup or a signal ends it.
SLEEPING_DLV = r'''#!%(python)s -I
import os, socket, sys, time
with open(%(pidfile)r, "w") as f:
    f.write(str(os.getpid()))
addr = [a for a in sys.argv if a.startswith("--client-addr=")][0]
host, port = addr.split("=", 1)[1].rsplit(":", 1)
conn = socket.create_connection((host, int(port)))
while True:
    time.sleep(60)
'''


# @scenario SC-debug-17
def test_a_reverse_tcp_adapter_does_not_outlive_a_killed_editor(tmp_path):
    """The adapter that dials back in (Go's dlv, here a stand-in that
    sleeps whatever happens to its connection) is gone after the editor is
    killed.  It used to hold the master of its own terminal, so no hangup
    ever reached it."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    _write_script(bindir / "dlv", SLEEPING_DLV % {
        "python": sys.executable, "pidfile": str(tmp_path / DLV_PIDFILE)})
    env = {"PATH": str(bindir) + ":" + os.environ.get("PATH", ""),
           "XWPE_DEBUG_START_TIMEOUT": str(REVERSE_TCP_START_TIMEOUT_S)}
    with WpeSession(str(tmp_path), DAP_GO_PROG, filename="main.go",
                    wait=1.5, env_extra=env) as w:
        _run_go(w)
        pid = _dlv_pid(w, tmp_path)
        os.kill(w.proc.pid, signal.SIGKILL)
        w.proc.wait()
        gone = _wait_gone(pid)
        if not gone:
            os.kill(pid, signal.SIGKILL)
    assert gone, "the adapter (pid %d) is still running after the editor " \
        "was killed" % pid


# @scenario SC-debug-17
def test_a68g_does_not_outlive_a_killed_editor(tmp_path):
    """a68g, with its program in a loop, is gone after the editor is
    killed."""
    _need("a68g")
    with WpeSession(str(tmp_path), A68_PAUSE_PROG, filename="p.a68",
                    wait=2.0) as w:
        _stop_then_run_on(w, A68_PAUSE_BREAK_LINE, START_TIMEOUT_S)
        gone, pid = _kill_editor_and_check(w, "a68g")
    assert gone, "a68g (pid %d) is still running after the editor was " \
        "killed" % pid
