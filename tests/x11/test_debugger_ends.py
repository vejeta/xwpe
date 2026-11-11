"""The editor's end during a debug session leaves no debugger behind (X11).

A SIGTERM to xwpe while dbx (the stand-in of tests/stubs/legacy) holds the
program at a breakpoint: the editor ends, the debugger is told to quit and
goes, and it is never sent a signal that dumps core.  The terminal row of
the same scenario is in tests/test_debugger_ends.py.

Guard row here: under the graphical editors the editor's child is the
helper script around the debugger, which the old end signalled instead of
the debugger, so the stand-in was never hit; the row holds the new end to
the same result.  The Wayland twin runs the same body.
"""
import os
import shutil
import signal
import time

import pytest

from test_debug_answer_wait import (
    DBX_BREAK_LINE, DBX_STUB_ENV, DBX_STUB_LOG, MIN_BAR_PIXELS, STOP_GREEN,
    STOP_TIMEOUT_S, _count, _go_to_line, _wait, dbx_stub)

STUB_PID_FILE = "stub.pid"     # relative: the debugger runs in the source's directory
EXIT_DEADLINE_S = 10
GONE_S = 6
CORE_SIGNALS = ("SIGBUS", "SIGSEGV", "SIGQUIT", "SIGABRT", "SIGILL",
                "SIGFPE")
EXIT_STUB_ENV = dict(DBX_STUB_ENV, XWPE_STUB_NOTE_SIGNALS="1",
                     XWPE_STUB_PID_FILE=STUB_PID_FILE)


def _read_pid(path):
    try:
        with open(path) as fh:
            text = fh.read().strip()
    except OSError:
        return None
    return int(text) if text.isdigit() else None


def _gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return True
    try:
        with open("/proc/%d/stat" % pid) as fh:
            return fh.read().split(")")[-1].split()[0] == "Z"
    except OSError:
        return False


# @scenario SC-debug-57
@pytest.mark.skipif(not shutil.which("gcc"), reason="no gcc")
@pytest.mark.parametrize("xwpe", [EXIT_STUB_ENV], indirect=True)
def test_the_editors_exit_ends_the_debugger_and_its_program(dbx_stub, xwpe):
    """SIGTERM to xwpe at a stop under dbx: xwpe ends, dbx goes, and dbx
    was sent no signal that dumps core."""
    home = os.path.dirname(xwpe.srcfile)
    _go_to_line(xwpe, DBX_BREAK_LINE)
    xwpe.key("ctrl+g", "b", delay=0.6)
    xwpe.key("ctrl+g", "r", delay=1.0)
    assert _wait(lambda: _count(xwpe.screenshot(), STOP_GREEN) >= MIN_BAR_PIXELS,
                 STOP_TIMEOUT_S), "Ctrl-G R should stop on the breakpoint"
    pid = _read_pid(os.path.join(home, STUB_PID_FILE))
    assert pid, "the stand-in wrote no PID"
    try:
        xwpe.proc.send_signal(signal.SIGTERM)
        xwpe.proc.wait(timeout=EXIT_DEADLINE_S)
        deadline = time.time() + GONE_S
        while time.time() < deadline and not _gone(pid):
            time.sleep(0.1)
        assert _gone(pid), "dbx (pid %d) was left running" % pid
        with open(os.path.join(home, DBX_STUB_LOG)) as fh:
            log = fh.read()
        cores = [s for s in CORE_SIGNALS if ("SIGNAL " + s) in log]
        assert not cores, "dbx was sent %s, which dumps core" % cores
    finally:
        if not _gone(pid):
            os.kill(pid, signal.SIGKILL)
