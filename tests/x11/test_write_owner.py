"""A failed write in the xwpe/X11 window (twin of tests/test_write_owner.py).

The sentence a failed write says is read from the terminal's screen in the
terminal rows; here the effect is what is asserted: the window keeps its
name and its text, the disk holds what it held, and the text can be saved
once the folder takes it.  The twin has no row that is red on the old
code: the old code kept the name and the text too and only said it worse,
which a screenshot cannot tell.  What the GUI rows pin is that the
terminal, X11 and Wayland windows agree.

Save As in a read-only folder; F2 on a writable file in a read-only folder
(written, no backup); F2 on a read-only file in a read-only folder (not
written, the edit kept).
"""
import atexit
import os
import shutil
import stat
import tempfile
import time

import pytest

# A state folder outside the folder the rows lock: the editor's HOME is the
# row's folder, so locking it would lock the state folder too.
STATE = tempfile.mkdtemp(prefix="wostate-")
atexit.register(shutil.rmtree, STATE, True)
LOCKED_ENV = [{"XDG_STATE_HOME": STATE}]


def _folder(xwpe):
    return os.path.dirname(xwpe.srcfile)


def _lock_folder(xwpe):
    os.chmod(_folder(xwpe), stat.S_IRUSR | stat.S_IXUSR)


def _unlock_folder(xwpe):
    os.chmod(_folder(xwpe), stat.S_IRWXU)


def _bytes(xwpe):
    with open(xwpe.srcfile, "rb") as fh:
        return fh.read()


def _names(xwpe):
    return sorted(os.listdir(_folder(xwpe)))


# @scenario SC-unsaved-text-10x
@pytest.mark.parametrize("xwpe", LOCKED_ENV, indirect=True)
def test_x11_save_as_in_a_read_only_folder_keeps_the_name(xwpe):
    original = _bytes(xwpe)
    xwpe.type("X")
    _lock_folder(xwpe)
    try:
        xwpe.menu("f", "a")                  # File > Save As
        xwpe.type("out.c")
        xwpe.key("alt+s")
        xwpe.key("Escape", "Escape", "Escape")   # the box, then the dialog
        assert xwpe.proc.poll() is None, "the editor ended, edits lost"
        assert _bytes(xwpe) == original, "the disk changed"
        assert "out.c" not in _names(xwpe)
    finally:
        _unlock_folder(xwpe)
    xwpe.key("F2")                           # still t.c, so F2 writes t.c
    assert _bytes(xwpe).startswith(b"X"), \
        "the window lost its name or its text: %r" % _bytes(xwpe)[:20]
    assert "out.c" not in _names(xwpe)


# @scenario SC-unsaved-text-11x
@pytest.mark.parametrize("xwpe", LOCKED_ENV, indirect=True)
def test_x11_f2_in_a_read_only_folder_writes_without_a_backup(xwpe):
    xwpe.type("X")
    _lock_folder(xwpe)
    try:
        xwpe.key("F2")
        xwpe.key("Escape")
        assert xwpe.proc.poll() is None
        assert _bytes(xwpe).startswith(b"X"), "the save did not happen"
        assert "t.bak" not in _names(xwpe)
    finally:
        _unlock_folder(xwpe)


# @scenario SC-unsaved-text-12x
@pytest.mark.parametrize("xwpe", LOCKED_ENV, indirect=True)
def test_x11_f2_on_a_read_only_file_in_a_read_only_folder_keeps_the_edit(xwpe):
    original = _bytes(xwpe)
    xwpe.type("X")
    os.chmod(xwpe.srcfile, stat.S_IRUSR)
    _lock_folder(xwpe)
    try:
        xwpe.key("F2")
        xwpe.key("Escape")
        assert xwpe.proc.poll() is None, "the editor ended, edits lost"
        assert _bytes(xwpe) == original, "the disk changed"
    finally:
        _unlock_folder(xwpe)
        os.chmod(xwpe.srcfile, stat.S_IRUSR | stat.S_IWUSR)
    xwpe.key("F2")                           # the edit is still there
    time.sleep(0.3)
    assert _bytes(xwpe).startswith(b"X"), \
        "the window lost the edit: %r" % _bytes(xwpe)[:20]
