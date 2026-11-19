"""A crash leaves its file under a name of its own, in the editor's state
folder, and never writes through a name somebody else planted.

The crash handler used to open the fixed name /tmp/xwpe-crash.txt: any user
could have put a link there.  Now it makes "<state>/xwpe/crash-<pid>.txt",
one file per process, new (O_EXCL, O_NOFOLLOW), in a folder it checks.

Run: tests/.venv/bin/python -m pytest -v tests/test_crash_file.py
"""
import os
import signal
import time

import pytest

pytest.importorskip("pyte")

from wpe_driver import WpeSession, WAIT_SCALE  # noqa: E402

EXIT_DEADLINE_S = 10 * WAIT_SCALE
VICTIM_TEXT = "keep me\n"


def _state_env(tmp_path):
    """The state folder this row's editor uses, a folder that does not exist
    yet: the crash makes it."""
    return str(tmp_path / "state")


def _wait_for_exit(w, seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        code = w.proc.poll()
        if code is not None:
            return code
        w._drain(0.1)
    return None


def _crash(w):
    """SIGSEGV to the exact process this row started; its exit code."""
    os.kill(w.proc.pid, signal.SIGSEGV)
    code = _wait_for_exit(w, EXIT_DEADLINE_S)
    assert code is not None, "wpe still running after SIGSEGV"
    return code


# @scenario SC-chosen-names-07
def test_a_crash_leaves_its_file_under_a_per_process_name(tmp_path):
    state = _state_env(tmp_path)
    with WpeSession(str(tmp_path), "int x;\n", env_extra={"XDG_STATE_HOME": state},
                    expect_crash=signal.SIGSEGV) as w:
        pid = w.proc.pid
        assert _crash(w) == -signal.SIGSEGV
    crash = os.path.join(state, "xwpe", "crash-%d.txt" % pid)
    assert os.path.exists(crash), (os.path.exists(state) and sorted(os.listdir(state)))
    with open(crash) as fh:
        assert fh.read().startswith("CRASH: signal %d" % signal.SIGSEGV)
    assert (os.stat(crash).st_mode & 0o777) == 0o600
    assert (os.stat(os.path.dirname(crash)).st_mode & 0o777) == 0o700


# @scenario SC-chosen-names-07
def test_a_crash_does_not_write_through_a_planted_link(tmp_path):
    state = _state_env(tmp_path)
    folder = os.path.join(state, "xwpe")
    os.makedirs(folder, mode=0o700)
    victim = tmp_path / "victim"
    victim.write_text(VICTIM_TEXT)
    with WpeSession(str(tmp_path), "int x;\n", env_extra={"XDG_STATE_HOME": state},
                    expect_crash=signal.SIGSEGV) as w:
        planted = os.path.join(folder, "crash-%d.txt" % w.proc.pid)
        os.symlink(str(victim), planted)
        assert _crash(w) == -signal.SIGSEGV
    assert victim.read_text() == VICTIM_TEXT, "the crash wrote through the planted link"
    assert os.path.islink(planted)
    assert os.listdir(folder) == [os.path.basename(planted)]
