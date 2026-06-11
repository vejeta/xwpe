"""A read-only file window is visibly marked: a padlock at the left of the title
bar (the filename is also drawn dimmed).  xwpe opens any non-writable file
read-only (f->ins==8); we open a 0444 file and check the title bar.  No compiler
or language server needed."""
import os
import pty
import select
import subprocess
import time

import pyte

from test_utf8_border import SafeScreen

WPE_BIN = os.environ.get("WPE_BIN") or os.path.join(os.path.dirname(__file__), "..", "wpe")
COLS, ROWS = 80, 30


def _open(workdir, name, body, mode):
    path = os.path.join(workdir, name)
    with open(path, "w") as fh:
        fh.write(body)
    os.chmod(path, mode)
    screen = SafeScreen(COLS, ROWS)
    stream = pyte.Stream(screen)
    mfd, slave = pty.openpty()
    env = os.environ.copy()
    env.update(TERM="xterm-256color", COLUMNS=str(COLS), LINES=str(ROWS),
               LC_ALL="en_US.UTF-8", HOME=workdir)
    proc = subprocess.Popen([WPE_BIN, name], stdin=slave, stdout=slave,
                            stderr=slave, cwd=workdir, env=env, preexec_fn=os.setsid)
    os.close(slave)
    end = time.time() + 2.0
    while time.time() < end:
        r, _, _ = select.select([mfd], [], [], 0.1)
        if r:
            try:
                data = os.read(mfd, 65536)
            except OSError:
                break
            if not data:
                break
            stream.feed(data.decode("utf-8", "replace"))
    return proc, mfd, screen


def _close(proc, mfd):
    try:
        os.killpg(os.getpgid(proc.pid), 9)
    except Exception:
        pass
    try:
        os.close(mfd)
    except Exception:
        pass
    proc.wait()


def test_readonly_file_shows_lock_in_titlebar(tmp_path):
    """A 0444 file opens read-only -> a padlock (U+1F512, '#' on a non-UTF
    terminal) sits at the left of the title bar."""
    proc, mfd, screen = _open(str(tmp_path), "locked.txt",
                              "line one\nline two\n", 0o444)
    try:
        title = screen.display[1]
        assert "locked.txt" in title, "title bar missing the filename\n%s" % title
        assert ("\U0001F512" in title or "#" in title), \
            "read-only padlock not shown in the title bar:\n%r" % title
    finally:
        _close(proc, mfd)


def test_writable_file_has_no_lock(tmp_path):
    """A normal writable (0644) file shows NO padlock -- the marker is specific to
    read-only windows."""
    proc, mfd, screen = _open(str(tmp_path), "editable.txt",
                              "line one\nline two\n", 0o644)
    try:
        title = screen.display[1]
        assert "editable.txt" in title, "title bar missing the filename\n%s" % title
        assert "\U0001F512" not in title and "#" not in title, \
            "a writable file should not show the read-only padlock:\n%r" % title
    finally:
        _close(proc, mfd)
