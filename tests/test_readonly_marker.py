"""A read-only file window is visibly marked: a padlock at the left of the title
bar (the filename is also drawn dimmed).  xwpe opens any non-writable file
read-only (f->ins==8); we open a 0444 file and check the title bar.  No compiler
or language server needed."""
import os
import pty
import select
import time

import pyte

import gui_owner
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
    owned = gui_owner.Terminal.start(
        gui_owner.current(), WPE_BIN, [name], workdir, workdir, mfd, slave,
        (COLS, ROWS))
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
    return owned, mfd, screen


def _close(owned, mfd):
    owned.end_for_row()
    try:
        os.close(mfd)
    except Exception:
        pass


def test_readonly_file_shows_lock_in_titlebar(tmp_path):
    """A 0444 file opens read-only -> a padlock (U+1F512, '#' on a non-UTF
    terminal) sits at the left of the title bar."""
    owned, mfd, screen = _open(str(tmp_path), "locked.txt",
                               "line one\nline two\n", 0o444)
    try:
        title = screen.display[1]
        assert "locked.txt" in title, "title bar missing the filename\n%s" % title
        assert ("\U0001F512" in title or "#" in title), \
            "read-only padlock not shown in the title bar:\n%r" % title
    finally:
        _close(owned, mfd)


def test_writable_file_has_no_lock(tmp_path):
    """A normal writable (0644) file shows NO padlock -- the marker is specific to
    read-only windows."""
    owned, mfd, screen = _open(str(tmp_path), "editable.txt",
                               "line one\nline two\n", 0o644)
    try:
        title = screen.display[1]
        assert "editable.txt" in title, "title bar missing the filename\n%s" % title
        assert "\U0001F512" not in title and "#" not in title, \
            "a writable file should not show the read-only padlock:\n%r" % title
    finally:
        _close(owned, mfd)


def test_unreadable_file_shows_lock_in_titlebar(tmp_path):
    """A mode-000 file exists but is neither readable nor writable: xwpe cannot
    read its content (the buffer comes up empty), but it is still a LOCKED file,
    so the title bar must show the padlock -- not a silently-empty buffer that
    looks editable but would fail to save."""
    owned, mfd, screen = _open(str(tmp_path), "noperm.txt",
                               "you cannot read this\n", 0o000)
    try:
        title = screen.display[1]
        assert "noperm.txt" in title, "title bar missing the filename\n%s" % title
        assert ("\U0001F512" in title or "#" in title), \
            "an unreadable (0000) file is still locked; padlock missing:\n%r" % title
    finally:
        _close(owned, mfd)
