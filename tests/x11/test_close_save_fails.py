"""Close and Quit keep the window when the save the user agreed to fails
(xwpe/X11 twin of tests/test_close_save_fails.py).

A window with unsaved edits asks whether to save.  Yes with a failing write
(a read-only file in a read-only folder) must leave the window open with the
edits in it: the row unlocks the folder afterwards and saves with F2, and
the file on disk then holds the edit.  No and a working save still close.
"""
import os
import stat
import subprocess
import time

import pytest

from conftest import xtool_env


def _lock(xwpe):
    os.chmod(xwpe.srcfile, stat.S_IRUSR)
    os.chmod(os.path.dirname(xwpe.srcfile), stat.S_IRUSR | stat.S_IXUSR)


def _unlock(xwpe):
    os.chmod(os.path.dirname(xwpe.srcfile), stat.S_IRWXU)
    os.chmod(xwpe.srcfile, stat.S_IRUSR | stat.S_IWUSR)


def _bytes(xwpe):
    with open(xwpe.srcfile, "rb") as fh:
        return fh.read()


def _ask(xwpe, how):
    """Edit, then Window > Close or File > Quit: the question is up."""
    xwpe.type("X")
    if how == "close":
        xwpe.menu("w", "c")
    else:
        xwpe.menu("f", "q")


def _release(letter):
    """The editor ends on the answer's key press: the server may keep that
    key down (auto-repeat) into the next session's window, so lift it."""
    subprocess.run(["xdotool", "keyup", letter],
                   env=xtool_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _ended(xwpe, letter, wait=4.0):
    end = time.time() + wait
    while time.time() < end:
        if xwpe.proc.poll() is not None:
            _release(letter)
            return True
        time.sleep(0.2)
    _release(letter)
    return False


# @scenario SC-edit-model-54
@pytest.mark.parametrize("how", ["close", "quit"])
def test_failed_save_keeps_the_window(xwpe, how):
    original = _bytes(xwpe)
    _ask(xwpe, how)
    _lock(xwpe)
    try:
        xwpe.key("y")
        xwpe.key("Escape", "Escape", "Escape")   # the write's error boxes
        assert xwpe.proc.poll() is None, "the editor ended, edits lost"
        assert _bytes(xwpe) == original, "disk changed"
    finally:
        _unlock(xwpe)
    xwpe.key("F2")                          # the window is still there to save
    assert _bytes(xwpe).startswith(b"X"), \
        "the window closed with the edit: %r" % _bytes(xwpe)[:20]


# @scenario SC-edit-model-54
@pytest.mark.parametrize("how", ["close", "quit"])
def test_declined_save_still_closes(xwpe, how):
    original = _bytes(xwpe)
    _ask(xwpe, how)
    xwpe.key("n")
    if how == "quit":
        assert _ended(xwpe, "n"), "Quit with No should end the editor"
    else:
        xwpe.key("F2")                      # nothing left to save
    assert _bytes(xwpe) == original


# @scenario SC-edit-model-54
@pytest.mark.parametrize("how", ["close", "quit"])
def test_working_save_still_closes(xwpe, how):
    _ask(xwpe, how)
    xwpe.key("y")
    assert _bytes(xwpe).startswith(b"X"), "the save did not happen"
    if how == "quit":
        assert _ended(xwpe, "y"), "Quit after a good save should end the editor"
