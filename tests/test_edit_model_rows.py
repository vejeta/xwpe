"""Rows the edit-model lockstep found, each the shortest keys that show
the editor breaking what the manual says, independent of the model.

Each row is an incoherence until the editor is fixed: it asserts what the
manual promises and is recorded as xfail, so ``pytest -rxX`` lists it.
"""
import pytest

pytest.importorskip("pyte")

from edit_model.oracle import BACKSPACE, DOWN, END, ESC, HOME, RIGHT, ctrl  # noqa: E402
from edit_model.oracle import EditSession  # noqa: E402
from edit_model.render import render_disk  # noqa: E402
from wpe_driver import incoherence  # noqa: E402

NARROW_RC = "[General]\nMaxColumn : 20\n"
# Two lines that the file reader wraps at Max. Columns 20 into soft rows.
WRAPPED = "one two three four five six\nseven-eight nine ten\n"


# @scenario SC-edit-model-17
def test_a_character_typed_at_the_end_of_a_full_wrapped_row_is_kept(tmp_path):
    """At Max. Columns 20, typing into the first row makes it exactly as
    wide as the window; End and one more character: the character is in
    the text afterwards, wherever the row wraps."""
    with EditSession(str(tmp_path), WRAPPED, filename="t.txt", xwperc=NARROW_RC) as s:
        assert s.last.win.mx == 20
        s.send([HOME, RIGHT, RIGHT, RIGHT])
        assert s.last.win.cursor == (0, 3), s.last.win.cursor
        s.send(["a"])
        s.send([END])
        w = s.send(["b"]).win
    assert render_disk(w.lines) == b"onea two three four bfive six\nseven-eight nine ten\n"


# @scenario SC-edit-model-18
@incoherence("Backspace with auto-indent deletes only the first byte of a "
             "UTF-8 character (help.xwpe_eng 593: delete a character backwards)")
def test_backspace_with_auto_indent_deletes_a_whole_character(tmp_path):
    """In a C file, where auto-indent is on, Backspace after a two-byte
    character takes the whole character away and leaves valid UTF-8, as it
    does in a plain text file."""
    with EditSession(str(tmp_path), "int x;\ncafé\n", filename="t.c") as s:
        assert s.last.win.flg & 1, "auto-indent is not on for a C file"
        s.send([DOWN, END])
        w = s.send([BACKSPACE]).win
    assert w.lines[1] == ("H", b"caf")


# @scenario SC-edit-model-32
def test_backspace_in_the_empty_last_row_of_two_rows_joins_it_to_the_row_above(tmp_path):
    """A file of one line, Ctrl-End, Return, Backspace: the new empty row goes
    and the cursor is at the end of the line, as it is with three rows."""
    with EditSession(str(tmp_path), "a\n", filename="t.txt") as s:
        s.send([ESC + ctrl("e"), "\r"])
        assert s.last.win.lines == [("H", b"a"), ("H", b"")]
        w = s.send([BACKSPACE]).win
    assert w.lines == [("H", b"a")] and w.cursor == (0, 1)


# @scenario SC-edit-model-40
def test_backspace_in_the_empty_last_row_of_three_rows_types_at_the_end_of_the_row_above(tmp_path):
    """Two lines, Ctrl-End, Return, Backspace, then a character: the empty
    row goes and the character lands at the end of the second line, not the
    first."""
    with EditSession(str(tmp_path), "first\nsecond\n", filename="t.txt") as s:
        s.send([ESC + ctrl("e"), "\r"])
        w = s.send([BACKSPACE]).win
        assert w.lines == [("H", b"first"), ("H", b"second")] and w.cursor == (1, 6)
        w = s.send(["Z"]).win
    assert w.disk() == b"first\nsecondZ\n"


# @scenario SC-edit-model-41
@pytest.mark.parametrize("seed", ["a\n", "first\nsecond\n"])
def test_undo_brings_back_the_empty_last_row_a_backspace_removed(tmp_path, seed):
    """After Backspace in the empty last row, Ctrl-U gives the empty row
    back, with the cursor on it, and the editor keeps answering."""
    with EditSession(str(tmp_path), seed, filename="t.txt") as s:
        s.send([ESC + ctrl("e"), "\r"])
        rows = list(s.last.win.lines)
        s.send([BACKSPACE])
        w = s.send([ctrl("u")]).win
    assert w.lines == rows and w.cursor[0] == len(rows) - 1
