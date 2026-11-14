"""Close and Quit keep the window when the save the user agreed to fails.

A window with unsaved edits asks "Do you want to save File ?".  When the
user answers Yes and the write fails (a read-only file in a read-only
folder, as here), the text exists only in that window: Close and Quit must
leave the window open, with the file on disk unchanged and the write's
error on screen.  Answering No still closes (the user gave the edits up),
and a save that works still closes.

Run: tests/.venv/bin/python -m pytest -v tests/test_close_save_fails.py
"""
import os
import stat

import pytest

pytest.importorskip("pyte")

from wpe_driver import ALT, WpeSession  # noqa: E402

CTRL_W = "\x17"          # Close the active window
SEED = "alpha\n"
ASK = "NOT saved"        # the question's first line
FAILED = "Cannot write"  # the write's error box; checked loosely


def _lock(folder, name):
    """Make the file and its folder read-only, as a user's read-only
    checkout is: fopen for writing then fails."""
    os.chmod(os.path.join(folder, name), stat.S_IRUSR)
    os.chmod(folder, stat.S_IRUSR | stat.S_IXUSR)


def _unlock(folder, name):
    os.chmod(folder, stat.S_IRWXU)
    os.chmod(os.path.join(folder, name), stat.S_IRUSR | stat.S_IWUSR)


def _blob(s):
    return "\n".join(s.display())


def _edited_and_asked(s, how):
    """Edit the file, then close or quit until the question is on screen."""
    s.key("X")
    if how == "close":
        s.key(CTRL_W)
    else:
        s.menu(ALT.FILE, "q")
    assert ASK in _blob(s), "no save question:\n%s" % _blob(s)


def _answer_yes(s):
    s.key("y")


def _answer_no(s):
    s.key("n")


# @scenario SC-edit-model-54
@pytest.mark.parametrize("how", ["close", "quit"])
def test_failed_save_keeps_the_window(tmp_path, how):
    """Yes, but the write fails: the window stays, the disk is unchanged."""
    folder = str(tmp_path)
    with WpeSession(folder, SEED) as s:
        _edited_and_asked(s, how)
        _lock(folder, "t.c")
        try:
            _answer_yes(s)
            assert s.alive(), "wpe ended and the edits are gone"
            assert FAILED in _blob(s), "no write error shown:\n%s" % _blob(s)
            s.key("\033", "\033", "\033")   # the write's error boxes
            shown = _blob(s)
            assert "t.c" in shown, "the window closed:\n%s" % shown
            assert "Xalpha" in shown, "the edit is not on screen:\n%s" % shown
            assert s.disk() == SEED.encode(), "disk changed"
        finally:
            _unlock(folder, "t.c")


# @scenario SC-edit-model-54
@pytest.mark.parametrize("how", ["close", "quit"])
def test_declined_save_still_closes(tmp_path, how):
    """Control: No gives the edits up, as ever."""
    with WpeSession(str(tmp_path), SEED) as s:
        _edited_and_asked(s, how)
        _answer_no(s)
        if how == "quit":
            s.display()
            assert not s.alive(), "Quit with No should end the editor"
        else:
            assert s.alive()
            assert "Xalpha" not in _blob(s), "the window is still open"
        assert s.disk() == SEED.encode()


# @scenario SC-edit-model-54
@pytest.mark.parametrize("how", ["close", "quit"])
def test_working_save_still_closes(tmp_path, how):
    """Control: Yes with a working write saves and closes."""
    with WpeSession(str(tmp_path), SEED) as s:
        _edited_and_asked(s, how)
        _answer_yes(s)
        assert s.disk() == ("X" + SEED).encode(), s.disk()
        if how == "close":
            assert s.alive()
            assert "Xalpha" not in _blob(s), "the window is still open"
        else:
            s.display()
            assert not s.alive(), "Quit after a good save should end the editor"
