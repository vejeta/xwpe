"""Rows the edit-model lockstep found, each the shortest keys that show
the editor breaking what the manual says, independent of the model.

Each row is an incoherence until the editor is fixed: it asserts what the
manual promises and is recorded as xfail, so ``pytest -rxX`` lists it.
"""
import pytest

pytest.importorskip("pyte")

from edit_model.oracle import BACKSPACE, DOWN, END, ESC, UP, ctrl  # noqa: E402
from edit_model.oracle import EditSession  # noqa: E402
from edit_model.render import render_disk  # noqa: E402
from wpe_driver import incoherence  # noqa: E402

NARROW_RC = "[General]\nMaxColumn : 20\n"
# Two lines that the file reader wraps at Max. Columns 20 into soft rows.
WRAPPED = "one two three four five six\nseven-eight nine ten\n"
FILE_END = ESC + ctrl("e")


# @scenario SC-edit-model-17
@incoherence("a character typed at the end of a soft-wrapped row as wide as "
             "the window disappears (help.xwpe_eng 602: insert mode inserts)")
def test_a_character_typed_at_the_end_of_a_full_wrapped_row_is_kept(tmp_path):
    """At Max. Columns 20, typing into the first row makes it exactly as
    wide as the window; End and one more character: the character is in
    the text afterwards, wherever the row wraps."""
    with EditSession(str(tmp_path), WRAPPED, filename="t.txt", xwperc=NARROW_RC) as s:
        assert s.last.win.mx == 20
        s.send([FILE_END, UP, UP, UP])
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
