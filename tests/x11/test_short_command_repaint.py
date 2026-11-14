"""The window is drawn again while the editor waits for a short command.

Help on a word runs `man` and waits for it, with a deadline, before it opens
the page.  A window that another one covered and uncovered meanwhile must
be drawn again at once, not left blank until the command ends.
"""
import os
import shutil
import stat
import subprocess
import time

import pytest

import procinfo
from conftest import changed_pixels, launch_client, xtool_env

SLOW_MAN = '''#!/bin/sh
echo $$ > "%(dir)s/man.pid"
exec sleep 10
'''
WAIT_STARTS_S = 1.5        # the man stand-in is running by then
COVER_UP_S = 1.5
REPAINT_S = 1.5
COVER_CLASS = "XLogo"
# The editor's top left corner: the menu bar and the first lines of the
# source.
CORNER = (0, 0, 480, 160)
# A window drawn again differs from the one before by almost nothing; a
# blank or covered one by thousands of pixels.
REPAINT_TOLERANCE_PX = 200


def _xdo(*args):
    return subprocess.run(["xdotool", *args], env=xtool_env(),
                          stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL).stdout.decode()


def _alive(pid):
    """True while pid runs: an ended process nobody has collected yet (a
    zombie) is gone, though kill(pid, 0) still finds it."""
    return procinfo.run_state(pid) not in (procinfo.GONE, procinfo.ZOMBIE)


@pytest.fixture
def slow_man(tmp_path, monkeypatch):
    """A `man` that takes ten seconds, first on the editor's PATH."""
    bindir = tmp_path / "bin"
    bindir.mkdir()
    stub = bindir / "man"
    stub.write_text(SLOW_MAN % {"dir": str(tmp_path)})
    stub.chmod(stub.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", "%s:%s" % (bindir, os.environ.get("PATH", "")))
    return tmp_path


@pytest.fixture
def cover(gui_owner, tmp_path, request):
    """A window that covers the editor's once it is mapped; the owner ends
    it with the row, also when the row fails."""
    return launch_client(gui_owner, tmp_path, request, ["xlogo"])


@pytest.mark.skipif(not shutil.which("xlogo"), reason="no xlogo")
# @scenario SC-debug-114
def test_the_window_is_drawn_again_while_a_manual_page_is_made(
        slow_man, xwpe, request):
    """Another window covers the editor's and goes away while `man` runs:
    the corner shows the editor's text again within seconds, though the
    command is still running."""
    reference = xwpe.screenshot().crop(CORNER)
    xwpe.menu("h", "t", delay=0.2)               # Help > Topic search: man int
    time.sleep(WAIT_STARTS_S)
    assert (slow_man / "man.pid").exists(), "help never ran man"
    request.getfixturevalue("cover")
    time.sleep(COVER_UP_S)
    covered = changed_pixels(reference, xwpe.screenshot().crop(CORNER))
    assert covered > REPAINT_TOLERANCE_PX, \
        "the other window did not cover the editor's: the row proves nothing"
    for win in _xdo("search", "--class", COVER_CLASS).split():
        _xdo("windowunmap", win)
    time.sleep(REPAINT_S)
    pid = int((slow_man / "man.pid").read_text().strip())
    assert _alive(pid), "man ended before the check: the row proves nothing"
    differ = changed_pixels(reference, xwpe.screenshot().crop(CORNER))
    assert differ < REPAINT_TOLERANCE_PX, \
        "the window was not drawn again while man ran (%d pixels of its " \
        "corner differ from the idle editor)" % differ
    assert xwpe.proc.poll() is None, "xwpe died"
