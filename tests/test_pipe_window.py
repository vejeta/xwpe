"""The faces around a window whose text came from a pipe.

Text read from a pipe is on no disk and has no name to write back to: its
title says where it came from.  The editor never writes it under that title,
says "[pipe]" in the frame, makes F2 a Save As, and asks the "NOT saved"
question when the window is closed or the editor quit after the user typed,
running the Save As box to its end before the window goes.

No reader of a pipe exists yet in this build, so these rows play the state
the reader will give a window: XWPE_TEST_FAIL=window-from-pipe makes the file
window a row opens start as one that came from a pipe.  The text itself is the
file's.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_pipe_window.py
"""
import os

import pytest

pytest.importorskip("pyte")

from wpe_driver import ALT, WpeSession, wait_until  # noqa: E402

SEED = "alpha\nbeta\n"
F2 = "\033OQ"
CTRL_W = "\x17"                 # close the window on top
ALT_S = "\033s"                 # the Save As box's Save
PIPE_ENV = {"XWPE_TEST_FAIL": "window-from-pipe"}
MARK = "[pipe]"
NOT_A_FILE = "This window is not a file."
QUESTION = "NOT saved"
SAVE_AS_BOX = "File-Manager"    # the title of the box Save As opens
STATE_WAIT_S = 8.0
END_WAIT_S = 8.0


def _from_pipe(tmp_path, seed=SEED):
    return WpeSession(str(tmp_path), seed, env_extra=PIPE_ENV, cols=100)


def _screen(w, drain=0.6):
    return "\n".join(w.display(drain=drain))


def _wait_said(w, needle):
    """The screen once it shows @needle, or the last one when the wait ends."""
    wait_until(w, lambda: needle in _screen(w, drain=0), STATE_WAIT_S)
    return _screen(w)


def _text_of(path):
    try:
        with open(str(path)) as fh:
            return fh.read()
    except OSError:
        return ""


def _name_into_box(w, name):
    for ch in name:
        w.key(ch, delay=0.05)
    w.key(ALT_S, delay=1.0)


def _changed_and_asked(w, how):
    """Type a letter, then close or quit, until the question is on screen."""
    w.key("X")
    if how == "close":
        w.key(CTRL_W, delay=1.0)
    else:
        w.menu(ALT.FILE, "q")
    return _wait_said(w, QUESTION)


# @scenario SC-pipe-window-01
def test_the_frame_of_a_window_from_a_pipe_says_pipe(tmp_path):
    """The title bar of a window whose text came from a pipe ends in [pipe]."""
    with _from_pipe(tmp_path) as w:
        screen = _wait_said(w, MARK)
        assert "t.c " + MARK in screen, screen


# @scenario SC-pipe-window-02
def test_the_frame_of_an_ordinary_file_says_nothing_of_a_pipe(tmp_path):
    """The control: no seam, no mark, so the mark is not on every window."""
    with WpeSession(str(tmp_path), SEED, cols=100) as w:
        screen = _screen(w)
        assert "t.c" in screen, screen
        assert MARK not in screen, screen


# @scenario SC-pipe-window-03
def test_f2_on_a_window_from_a_pipe_is_save_as(tmp_path):
    """F2 opens the Save As box, a name written there holds the text, the mark
    is gone, and the next F2 saves plainly under that name."""
    saved = tmp_path / "kept.txt"
    with _from_pipe(tmp_path) as w:
        w.key("X")
        w.key(F2, delay=1.0)
        screen = _wait_said(w, SAVE_AS_BOX)
        assert SAVE_AS_BOX in screen, screen
        _name_into_box(w, "kept.txt")
        assert wait_until(w, lambda: _text_of(saved) == "X" + SEED, STATE_WAIT_S), \
            "Save As wrote %r: %s" % (_text_of(saved), _screen(w))
        screen = _screen(w)
        assert MARK not in screen, screen
        assert (tmp_path / "t.c").read_text() == SEED, "the original file was written"
        w.key("Y")
        w.key(F2, delay=1.0)
        assert wait_until(w, lambda: _text_of(saved) == "XY" + SEED, STATE_WAIT_S), \
            "the second F2 did not save plainly: %r" % _text_of(saved)
        assert SAVE_AS_BOX not in _screen(w), _screen(w)


# @scenario SC-pipe-window-04
def test_save_all_writes_nothing_for_a_window_from_a_pipe(tmp_path):
    """A typed letter and Save All: the sentence says the window is not a file
    and the window's own file on disk keeps its text."""
    with _from_pipe(tmp_path) as w:
        w.key("X")
        w.menu(ALT.FILE, "l")
        screen = _wait_said(w, NOT_A_FILE)
        assert NOT_A_FILE in screen, screen
        assert SAVE_AS_BOX not in screen, "Save All opened a box:\n" + screen
    assert (tmp_path / "t.c").read_text() == SEED


# @scenario SC-pipe-window-05
def test_closing_an_unchanged_window_from_a_pipe_asks_nothing(tmp_path):
    """The user typed nothing: closing the window asks no question."""
    with _from_pipe(tmp_path) as w:
        w.key(CTRL_W, delay=1.0)
        screen = _screen(w)
        assert QUESTION not in screen, screen
        assert MARK not in screen, screen


# @scenario SC-pipe-window-06
@pytest.mark.parametrize("how", ["close", "quit"])
@pytest.mark.parametrize("style,key", [("modern", "Ctrl-S"), ("borland", "F2")])
def test_yes_to_the_close_question_says_which_key_saves(tmp_path, how, style, key):
    """Typed, closed, Yes: no box opens from the close.  The status row says
    "This window is not a file: save it with <key> first", with the Save key
    of the key style in use, and the window, its text and the editor stay.

    Interim row: the modal box owner's first pass replaces it with rows that
    run the Save As box itself from the close question."""
    line = "This window is not a file: save it with %s first" % key
    env = dict(PIPE_ENV, XWPE_KEYS=style)
    with WpeSession(str(tmp_path), SEED, env_extra=env, cols=110) as w:
        _changed_and_asked(w, how)
        w.key("y", delay=1.0)
        screen = _wait_said(w, line)
        assert line in screen, screen
        assert w.alive(), "the editor ended with the text unsaved"
        assert "Xalpha" in screen, screen
        assert MARK in screen, screen
        assert SAVE_AS_BOX not in screen, screen
    assert "kept.txt" not in os.listdir(str(tmp_path))
    assert (tmp_path / "t.c").read_text() == SEED


# @scenario SC-pipe-window-08
@pytest.mark.parametrize("how", ["close", "quit"])
def test_no_to_the_close_question_gives_the_text_up(tmp_path, how):
    """The control of the two rows above: No closes, and writes nothing."""
    with _from_pipe(tmp_path) as w:
        _changed_and_asked(w, how)
        w.key("n", delay=1.0)
        if how == "quit":
            w.ended_within(END_WAIT_S, "the editor to end after No")
        else:
            assert "Xalpha" not in _screen(w)
    assert (tmp_path / "t.c").read_text() == SEED
