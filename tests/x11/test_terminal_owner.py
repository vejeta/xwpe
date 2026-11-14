"""A graphical editor writes nothing to the terminal it was started from.

xwpe started from a shell has that shell's terminal as its standard streams
but draws in its own window, so the terminal was never the editor's: nothing
it does, and nothing it does when it ends, may reach it.  The rows start xwpe
with its standard streams on a pty and read every byte written there:

- the Options box opened and closed (its close used to switch the terminal's
  mouse reporting on);
- the end by SIGTERM, which used to finish with a full terminal reset;
- the end by Alt-X, the same.

The Wayland twin (tests/wayland/test_terminal_owner.py) runs the same body.
"""
import os
import select
import signal
import subprocess

import pytest

from conftest import _WAIT_SCALE, _sleep

PTY = {"XWPE_TEST_PTY": True}
EXIT_DEADLINE_S = 10 * _WAIT_SCALE
TYPED = "hello terminal"


def written_to_the_terminal(xwpe):
    """Every byte the editor has written to its standard streams so far."""
    got = b""
    while select.select([xwpe.pty_master], [], [], 0.3)[0]:
        try:
            chunk = os.read(xwpe.pty_master, 65536)
        except OSError:
            break
        if not chunk:
            break
        got += chunk
    return got


def wait_for_the_end(xwpe):
    try:
        xwpe.proc.wait(timeout=EXIT_DEADLINE_S)
    except subprocess.TimeoutExpired:
        pass
    assert xwpe.proc.poll() is not None, \
        "xwpe still running %ds after it was asked to end" % EXIT_DEADLINE_S


# @scenario SC-term-11
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_options_open_and_close_write_nothing_to_the_terminal(xwpe):
    """Alt-O, Editor, Esc: the box closes and the editor's mouse queue is
    emptied; no mouse mode, pointer shape or screen switch goes to the
    terminal that launched the editor."""
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)
    xwpe.key("Escape", delay=1.0)
    written = written_to_the_terminal(xwpe)
    assert written == b"", \
        "the Options box wrote %d bytes to the launching terminal:\n%r" \
        % (len(written), written[:200])


# @scenario SC-term-11
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_sigterm_writes_nothing_to_the_terminal(xwpe):
    """SIGTERM after an Options box was opened and closed: the unsaved work
    goes to the .ESV file and the launching terminal gets no byte, not even
    a reset."""
    xwpe.type(TYPED, delay=0.8)
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)
    xwpe.key("Escape", delay=1.0)
    os.kill(xwpe.proc.pid, signal.SIGTERM)
    wait_for_the_end(xwpe)
    saved = xwpe.srcfile + ".ESV"
    # The one line the editor does say, to tell where the work went, is a
    # message for the user's shell, not a terminal control.
    report = ("File %s saved!\r\n" % os.path.basename(saved)).encode()
    written = written_to_the_terminal(xwpe).replace(report, b"")
    assert written == b"", \
        "SIGTERM wrote %d bytes to the launching terminal:\n%r" \
        % (len(written), written[:200])
    assert os.path.exists(saved), "the unsaved work was not written to an .ESV"


# @scenario SC-term-11
@pytest.mark.parametrize("xwpe", [PTY], indirect=True)
def test_alt_x_writes_nothing_to_the_terminal(xwpe):
    """Alt-X after an Options box was opened and closed: the editor ends and
    the launching terminal gets no byte, not even a reset."""
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)
    xwpe.key("Escape", delay=1.0)
    xwpe.key("alt+x", delay=1.0)
    wait_for_the_end(xwpe)
    written = written_to_the_terminal(xwpe)
    assert written == b"", \
        "Alt-X wrote %d bytes to the launching terminal:\n%r" \
        % (len(written), written[:200])
