"""The graphical editor keeps unsaved work when SIGTERM ends it.

A logout or a "kill PID" ends xwpe with SIGTERM.  Like the terminal editor
(tests/test_signal_save.py), it writes every window with unsaved changes to
its .ESV file, which `xwpe -r` reopens, and then exits.
"""
import os
import signal
import subprocess

from conftest import _WAIT_SCALE

EXIT_DEADLINE_S = 10 * _WAIT_SCALE
TYPED = "hello signal"


# @scenario SC-signal-save-06
def test_sigterm_keeps_typed_work_in_an_esv_file(xwpe):
    xwpe.type(TYPED, delay=0.8)
    os.kill(xwpe.proc.pid, signal.SIGTERM)
    try:
        xwpe.proc.wait(timeout=EXIT_DEADLINE_S)
    except subprocess.TimeoutExpired:
        pass
    assert xwpe.proc.poll() is not None, "xwpe still running %ds after SIGTERM" % EXIT_DEADLINE_S
    saved = xwpe.srcfile + ".ESV"
    assert os.path.exists(saved), sorted(os.listdir(os.path.dirname(saved)))
    with open(saved) as fh:
        assert TYPED in fh.read()
