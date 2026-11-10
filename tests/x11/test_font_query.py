"""The window opens although the desktop's font setting does not answer.

At start-up the graphical editor asks the desktop for its monospace font
(`gsettings get ...`).  A settings service that hangs must cost the user a
short wait at most, never the window: the query is a command of the
children's owner with a deadline, and the editor falls back to "monospace".
"""
import os
import stat
import time

import pytest

from conftest import _find_xwpe_window

# The query's deadline is 1.5 s; the window must be up well inside this.
WINDOW_S = 8.0
GONE_S = 6.0

STUB = '''#!/bin/sh
echo $$ > "%(dir)s/gsettings.pid"
exec sleep 30
'''


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _state(pid):
    """The kernel's one-line state of @pid, for a failure message."""
    try:
        with open("/proc/%d/status" % pid) as fh:
            return next((l.strip() for l in fh if l.startswith("State:")), "?")
    except OSError:
        return "gone"


@pytest.fixture
def hanging_gsettings(tmp_path, monkeypatch):
    """A `gsettings` that never answers, first on the editor's PATH."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    stub = bindir / "gsettings"
    stub.write_text(STUB % {"dir": str(tmp_path)})
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", "%s:%s" % (bindir, os.environ.get("PATH", "")))
    return tmp_path


# @scenario SC-debug-110
def test_the_window_opens_while_the_font_setting_does_not_answer(
        hanging_gsettings, xwpe):
    """A `gsettings` that hangs does not keep the window from opening, and the
    query is ended at its deadline, not left running."""
    tmp_path = hanging_gsettings
    pid_file = tmp_path / "gsettings.pid"
    assert _find_xwpe_window(timeout=WINDOW_S) is not None
    assert pid_file.exists(), "the editor never asked the desktop for its font"
    pid = int(pid_file.read_text().strip())
    deadline = time.time() + GONE_S
    while time.time() < deadline and _pid_alive(pid):
        time.sleep(0.2)
    assert not _pid_alive(pid), \
        "the font query (pid %d) was left running: %s" % (pid, _state(pid))
    assert xwpe.proc.poll() is None, "xwpe died"
