"""A command with nothing to act on says so, in one line on the status row.

Cut, Copy, Delete, the block commands that need a block, Paste, Undo, Redo
and the debugger commands that need a session used to do nothing and say
nothing, or (Undo, Redo) open an Error box that cost a keystroke for a non
event.  Each now says one line on the bottom row, the same from a key and
from a menu, never a box, and the next key acts at once.  The text of every
line is the editor's own constant (edit.h, E_SAY_*).

Every row runs the programming editor (`wpe`) with the Borland keys and the
anchor block mode, so a key and a menu route reach the same command.
"""
import os

import pytest

from wpe_driver import ALT, WpeSession

SEED = "hello world\nsecond line\nthird\n"
CTRL_X, CTRL_C, CTRL_V, CTRL_U, CTRL_R = "\x18", "\x03", "\x16", "\x15", "\x12"
CTRL_K = "\x0b"
NOTHING_MARKED = "Nothing is marked: the text is unchanged"
CLIPBOARD_EMPTY = "The clipboard is empty: nothing pasted"
NO_UNDO = "No more Undo"
NO_REDO = "No more Redo"
NO_SESSION = "No debug session"
READ_ONLY = "Read-only window: the text is unchanged"
ERROR_FRAME = "Error"
KEY_WAIT = 0.6


def _screen(s):
    return "\n".join(s.display())


def _status(s):
    return s.display()[-1].rstrip()


def _first_text_row(s):
    """The first line of the text window: the row under its frame."""
    return s.display()[2]


def _said(s, line):
    return line in _status(s)


def _count(s, line):
    return _screen(s).count(line)


def _run(tmp_path, *keys):
    """A session after @keys, each a separate key press."""
    s = WpeSession(str(tmp_path), SEED, filename="t.txt")
    for k in keys:
        s.key(k, delay=KEY_WAIT)
    return s


def _disk(s):
    with open(s.path) as fh:
        return fh.read()


# @scenario SC-menus-10
@pytest.mark.parametrize("keys", [(CTRL_X,), (CTRL_C,), (ALT.EDIT, "d"), (ALT.EDIT, "t"),
                                  (ALT.EDIT, "c")],
                         ids=["ctrl-x", "ctrl-c", "edit-delete", "edit-cut", "edit-copy"])
def test_edit_commands_with_nothing_marked_say_so_once(tmp_path, keys):
    with _run(tmp_path, *keys) as s:
        assert _said(s, NOTHING_MARKED), _status(s)
        assert _count(s, NOTHING_MARKED) == 1, "once, not twice"
        assert ERROR_FRAME not in _screen(s)
        assert s.alive()


# @scenario SC-menus-11
@pytest.mark.parametrize("letter", ["c", "m", "w", "i", "t", "a"],
                         ids=["copy", "move", "write", "to-right", "to-left", "change-case"])
def test_block_commands_with_nothing_marked_say_so(tmp_path, letter):
    with _run(tmp_path, ALT.BLOCK, letter) as s:
        assert _said(s, NOTHING_MARKED), _status(s)
        assert _count(s, NOTHING_MARKED) == 1
        assert "Change Case" not in _screen(s) and "Write" not in _screen(s).split("\n")[0]


# @scenario SC-menus-12
@pytest.mark.parametrize("keys", [(CTRL_V,), (ALT.EDIT, "p")], ids=["ctrl-v", "edit-paste"])
def test_paste_with_an_empty_clipboard_says_so(tmp_path, keys):
    with _run(tmp_path, *keys) as s:
        assert _said(s, CLIPBOARD_EMPTY), _status(s)
        assert ERROR_FRAME not in _screen(s)
        s.key("Q", delay=KEY_WAIT)
        assert "Qhello world" in _first_text_row(s), "the next key goes into the text"


# @scenario SC-menus-13
@pytest.mark.parametrize("keys,line", [((CTRL_U,), NO_UNDO), ((CTRL_R,), NO_REDO),
                                       ((ALT.EDIT, "u"), NO_UNDO),
                                       ((ALT.EDIT, "r"), NO_REDO)],
                         ids=["ctrl-u", "ctrl-r", "edit-undo", "edit-redo"])
def test_undo_and_redo_with_nothing_say_so_without_a_box(tmp_path, keys, line):
    with _run(tmp_path, *keys) as s:
        assert _said(s, line), _status(s)
        assert ERROR_FRAME not in _screen(s), "no Error box"
        s.key("Z", delay=KEY_WAIT)
        assert "Zhello world" in _screen(s), "the next key goes into the text"


# @scenario SC-menus-14
@pytest.mark.parametrize("letter", ["q", "i", "e", "d", "a", "k"],
                         ids=["quit", "interrupt", "edit-watch", "delete-watch", "remove-watches",
                              "show-stack"])
def test_debugger_commands_with_no_session_say_so(tmp_path, letter):
    with _run(tmp_path, ALT.DEBUG, letter) as s:
        assert _said(s, NO_SESSION), _status(s)
        assert ERROR_FRAME not in _screen(s)


# @scenario SC-menus-15
def test_commands_that_start_or_act_without_a_session_do_not_say_it(tmp_path):
    """Toggle Breakpoint and Remove all Breakpoints act without a session."""
    for letter in ("b", "m"):
        with _run(tmp_path, ALT.DEBUG, letter) as s:
            assert not _said(s, NO_SESSION), (letter, _status(s))


# @scenario SC-menus-16
def test_with_a_block_cut_cuts_and_says_nothing_of_it(tmp_path):
    """Positive control: a marked block is cut as before, the row is silent."""
    with _run(tmp_path, CTRL_K + "b", "\x1b[C" * 5, CTRL_K + "k", CTRL_X) as s:
        assert not _said(s, NOTHING_MARKED), _status(s)
        assert "hello" not in _first_text_row(s)
        assert " world" in _first_text_row(s)


# @scenario SC-menus-17
def test_the_undo_of_a_block_deletion_says_nothing(tmp_path):
    """The block deletion is replayed by Undo through the silent primitive:
    no line is said on the way."""
    with _run(tmp_path, CTRL_K + "b", "\x1b[C" * 5, CTRL_K + "k", CTRL_K + "y",
              CTRL_U) as s:
        assert not _said(s, NOTHING_MARKED), _status(s)
        assert "hello world" in _first_text_row(s)


# @scenario SC-menus-18
def test_a_window_closed_to_changes_says_read_only_not_nothing_marked(tmp_path):
    """Show Buffer opens the clipboard viewer, a window closed to changes:
    Cut there says the read-only line, never the unmarked one."""
    for keys in ((CTRL_X,), (ALT.EDIT, "d")):
        with _run(tmp_path, ALT.EDIT, "s", *keys) as s:
            assert _said(s, READ_ONLY), _status(s)
            assert NOTHING_MARKED not in _screen(s)
            assert s.alive()
