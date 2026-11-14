"""The cursor and the block marks always stand on a glyph boundary.

A character of several bytes (an `é` is two) is one place to the user: the
cursor is before it or after it, never between its bytes, and neither is a
block mark.  A key that leaves a position between the bytes lets the next
key split the character (a typed letter, Delete, Ctrl-K V) and the file is
no longer valid UTF-8.  Each row names one key that leaves a position inside
a character, then the keys that split it.

Found by the covering run of the edit model (rows r005, r016, r019, r024,
r030 and r043): every `utf8-valid` and `inside` finding of that run starts
from a cursor or a mark between the bytes of a character.
"""
import pytest

pytest.importorskip("pyte")

from edit_model import selfcheck  # noqa: E402
from edit_model.oracle import (BACKSPACE, CTRL_LEFT, DELETE, DOWN, END, ENTER,  # noqa: E402
                               HOME, PGDN, PGUP, RIGHT, SHIFT_LEFT, SHIFT_RIGHT, UP,
                               EditSession, alt, ctrl)

SHIFT_PGUP = "\033[5;2~"
CK = ctrl("k")
NARROW_RC = "[General]\nMaxColumn : 20\n"
POSITION_TRACES = ("position off a glyph", "position inside")


def still_open(reason):
    """Mark a row whose defect is known and not fixed yet.

    The row keeps its full assertion and runs every time; strict means the
    suite fails as soon as the row passes, so whoever fixes the defect also
    removes the mark and the row starts guarding the fix."""
    return pytest.mark.xfail(strict=True, reason=reason)


def _session(tmp_path, seed, **kw):
    return EditSession(str(tmp_path), seed, **kw)


def _glyph_start(w, y, x):
    return 0 <= y < len(w.lines) and selfcheck._on_boundary(w.lines[y][1], x)


def _where(w):
    return "cursor %s marks %s %s rows %s" % (w.cursor, w.mb, w.me,
                                              [r[1] for r in w.lines[:3]])


def _assert_no_position_trace(s):
    """The painter puts a cursor that is off a glyph right at the next paint,
    so a row that only reads the cursor would pass without the owner: the
    editor's own trace says whether any writer left one off."""
    bad = [t for t in s.trace_since_last() if any(p in t for p in POSITION_TRACES)]
    assert bad == [], "the editor traced a position off a glyph: %s" % bad


def _assert_on_glyphs(w, s):
    assert _glyph_start(w, w.y, w.x), "cursor inside a character: " + _where(w)
    for y, x in (w.mb, w.me):
        if y >= 0:
            assert _glyph_start(w, y, x), "mark inside a character: " + _where(w)
    _assert_no_position_trace(s)


# @scenario SC-edit-model-utf8-01
def test_page_up_keeps_the_cursor_between_characters(tmp_path):
    """Page Up from the second column of a short row lands on a row that
    starts with an `é`: the byte column is kept, which is between its bytes."""
    with _session(tmp_path, "é\nab\n") as s:
        s.send([DOWN, RIGHT])
        w = s.send([PGUP]).win
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-utf8-02
def test_page_down_keeps_the_cursor_between_characters(tmp_path):
    with _session(tmp_path, "ab\né\n") as s:
        s.send([RIGHT])
        w = s.send([PGDN]).win
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-utf8-03
def test_shift_page_up_puts_no_block_mark_inside_a_character(tmp_path):
    with _session(tmp_path, "é\nab\n") as s:
        s.send([DOWN, RIGHT])
        w = s.send([SHIFT_PGUP]).win
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-utf8-04
def test_a_letter_typed_after_page_up_leaves_the_file_valid_utf8(tmp_path):
    """The row that holds the cursor is typed into: the `é` must stay whole."""
    with _session(tmp_path, "é\nab\n") as s:
        s.send([DOWN, RIGHT, PGUP])
        w = s.send(["a"]).win
        w.disk().decode("utf-8")
        _assert_no_position_trace(s)


# @scenario SC-edit-model-utf8-05
def test_delete_after_page_up_leaves_the_file_valid_utf8(tmp_path):
    with _session(tmp_path, "é\nab\n") as s:
        s.send([DOWN, RIGHT, PGUP])
        w = s.send(["\033[3~"]).win
        w.disk().decode("utf-8")
        _assert_no_position_trace(s)


# @scenario SC-edit-model-utf8-06
def test_ctrl_y_keeps_the_cursor_between_characters(tmp_path):
    """Ctrl-Y on the first row brings the next row up under the cursor, whose
    byte column falls between the bytes of that row's `é`."""
    with _session(tmp_path, "ab\néx\n") as s:
        s.send([RIGHT])
        w = s.send([ctrl("y")]).win
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-utf8-08
def test_block_delete_leaves_the_cursor_between_characters(tmp_path):
    """Ctrl-K Y on a block of another row: the cursor keeps its row, and
    the column is shifted by the width of the deleted block on a row it is
    not on (the covering row r019)."""
    with _session(tmp_path, "éa\nxyz\n") as s:
        s.send([DOWN, SHIFT_RIGHT, UP, RIGHT])
        w = s.send([CK, "y"]).win
        _assert_on_glyphs(w, s)


# The rows of the covering run, replayed key by key against the model
# (tests/edit_model/lockstep.py): the actions are what the shrinking left of
# a walk, after the row's own prelude.
LIT = ("literal_toggle", None)


def _replay(tmp_path, row_name, actions, no_traces=False):
    from edit_model import covering, lockstep
    row = dict((r.name, r) for r in covering.load())[row_name]
    walk = lockstep.Walk(str(tmp_path), None, 1, len(actions), row=row)
    walk.drawer.queue = list(actions)
    walk.run()
    assert not walk.lost, walk.lost
    if no_traces:
        assert walk.position_traces == [], walk.position_traces
    return walk.disagreements


def _found(found, action, field):
    return [(d["action"], d["field"], d["model"], d["editor"]) for d in found
            if d["field"] == field and d["action"].split("(")[0] == action]


# @scenario SC-edit-model-utf8-17
@still_open("undo of an overwrite gives back other text than the model; not yet classified")
def test_covering_row_r001_undo_after_an_overwrite_gives_back_what_the_model_says(tmp_path):
    """Editor-attributed `undo text`, the largest group of the run: an `é`,
    a brace and a blank typed, Shift-Left, a blank, Ctrl-U, in overwrite mode
    on the first row of the C file."""
    found = _replay(tmp_path, "r001", [LIT, ("type", "é".encode()), ("type", b"}"),
                                       ("type", b" "), ("shift_left", None),
                                       ("type", b" "), ("undo", None)])
    assert _found(found, "undo", "text") == []


# @scenario SC-edit-model-utf8-10
def test_covering_row_r019_block_delete_leaves_no_position_inside_a_character(tmp_path):
    """Replay of the covering row r019: an Undo and a Redo leave the block on
    another row, a letter with an accent is typed, Ctrl-K Y deletes the block
    and the cursor ends between the bytes of the accent."""
    found = _replay(tmp_path, "r019", [
        LIT, ("type", b"q"), ("type", b"}"), ("page_down", None), ("delete", None),
        ("undo", None), ("undo", None), ("undo", None), ("redo", None), ("redo", None),
        ("type", "é".encode()), ("block_delete", None), ("block_begin", None)],
        no_traces=True)
    assert [d for d in found if d["field"] in ("inside", "utf8-valid")] == []


# @scenario SC-edit-model-utf8-11
def test_covering_row_r012_backspace_after_page_up_takes_the_character_the_model_says(tmp_path):
    """Editor-attributed `backspace text`: after Page Up the cursor is between
    the bytes of an `é`, and Backspace (auto-indent on) removes both bytes
    where the manual's rule backspace_char leaves the second one: the
    disagreement exists only because the cursor is inside the character."""
    found = _replay(tmp_path, "r012", [
        LIT, ("type", b"-"), ("type", "é".encode()), ("enter", None), ("type", b"}"),
        ("type", b"#"), ("backspace", None), ("type", b" "), ("page_up", None),
        ("backspace", None)], no_traces=True)
    assert _found(found, "backspace", "text") == []


# @scenario SC-edit-model-utf8-12
def test_covering_row_r012_shift_down_on_the_last_row_selects_what_the_editor_selects(tmp_path):
    """Editor-attributed `shift_down marks`: Shift-Down on the last row after
    typing an `é`. The model selects from column 0; the editor leaves the block
    empty at the cursor. Model bug, no character is split."""
    found = _replay(tmp_path, "r012", [LIT, ("type", "é".encode()), ("shift_down", None)])
    assert _found(found, "shift_down", "marks") == []


# @scenario SC-edit-model-utf8-13
@still_open("a Ctrl-Y that deletes nothing still records an undo step")
def test_covering_row_r009_a_second_ctrl_y_on_an_empty_file_is_no_step(tmp_path):
    """Editor-attributed `delete_line steps`: Ctrl-Y twice on an empty file
    records two undo steps in the editor, one in the model."""
    found = _replay(tmp_path, "r009", [LIT, ("delete_line", None), ("delete_line", None)])
    assert _found(found, "delete_line", "steps") == []


# @scenario SC-edit-model-utf8-14
@still_open("a block indent that changes nothing still records an undo step")
def test_covering_row_r038_block_indent_that_changes_nothing_records_no_step(tmp_path):
    """Editor-attributed `block_indent steps`: Ctrl-K I after a typed brace,
    Backspace and Ctrl-U: the text is unchanged yet the editor records a step
    and drops the redo list."""
    found = _replay(tmp_path, "r038", [("type", b"{"), ("backspace", None),
                                       ("undo", None), ("block_indent", None)])
    assert _found(found, "block_indent", "steps") == []


# @scenario SC-edit-model-utf8-15
def test_covering_row_r039_block_move_wraps_where_the_model_wraps(tmp_path):
    """Two pastes, Page Down, Ctrl-K V on a row that reaches Max. Columns: a
    block of one row leaves the rows above it in its soft-wrapped line as
    they were."""
    found = _replay(tmp_path, "r039", [LIT, ("edit_paste", None), ("edit_paste", None),
                                       ("page_down", None), ("block_move", None)])
    assert _found(found, "block_move", "text") == []


MOVE_RC = "[General]\nMaxColumn : 20\n"
ONE_ROW_MOVES = [
    # the row the block leaves fills up to the width again, a word later
    # than typing would pull it up
    ("a bb dddd a hh-ii a dddd a ccc eeeee hh-ii ccc bb eeeee", 2, 3, 10),
    # the rows after it break at a dash where it fits
    ("bb ccc hh-ii eeeee hh-ii eeeee eeeee hh-ii a eeeee f-g hh-ii hh-ii a f-g "
     "dddd hh-ii hh-ii", 3, 0, 14),
    # the blank at the end of a soft row goes, so two words meet
    ("eeeee hh-ii f-g a jjjjjjj f-g ccc bb jjjjjjj a dddd eeeee ccc dddd hh-ii", 3, 14, 15),
]


# @scenario SC-edit-model-utf8-15
@pytest.mark.parametrize("para,row,begin,end", ONE_ROW_MOVES)
def test_a_one_row_block_moved_away_leaves_its_line_wrapped_as_the_model_says(
        tmp_path, para, row, begin, end):
    """Ctrl-K V of a block inside one row of a soft-wrapped line, dropped at
    the start of the last line: the rows above the block keep their breaks,
    and the block's row with the rows after it is wrapped where the file
    reader wraps (at the last blank or dash that fits)."""
    from edit_model import model
    with _session(tmp_path, "top line\n" + para + "\nzz end\n", filename="t.txt",
                  xwperc=MOVE_RC) as s:
        keys = ([DOWN] * row + [HOME] + [RIGHT] * begin + [CK, "b"]
                + [RIGHT] * (end - begin) + [CK, "k"])
        w = s.send(keys).win
        target = len(w.lines) - 2 if w.lines[-1][1] == b"" else len(w.lines) - 1
        w = s.send([DOWN] * (target - row) + [HOME]).win
        st = model.state_from_window(w, model.Options.from_xwperc(MOVE_RC))
        pred = model.apply(st, model.Action("block_move"))
        w = s.send([CK, "v"]).win
        assert [tuple(r[:2]) for r in w.lines] == list(pred.lines())


# @scenario SC-edit-model-utf8-16
def test_covering_row_r022_undo_of_typing_restores_the_block_the_model_says(tmp_path):
    """Ctrl-U of two typed letters after Ctrl-K L: the block stays the line
    Ctrl-K L marked, moved only by the letter Ctrl-U takes out."""
    found = _replay(tmp_path, "r022", [("type", b"x"), ("type", b"x"),
                                       ("block_line", None), ("undo", None)])
    assert _found(found, "undo", "marks") == []


# @scenario SC-edit-model-utf8-16
def test_undo_of_a_letter_typed_beside_the_same_letter_moves_the_block_by_its_place(tmp_path):
    """`x` typed before another `x`, a block from between them to the end,
    Ctrl-U: the letter taken out is the one typed, so both ends of the
    block move back one column."""
    found = _replay(tmp_path, "r022", [("type", b"a"), ("type", b"x"), ("left", None),
                                       ("type", b"x"), ("block_begin", None), ("end", None),
                                       ("block_end", None), ("undo", None)])
    assert _found(found, "undo", "marks") == []


# @scenario SC-edit-model-utf8-16
def test_undo_of_a_letter_typed_while_a_block_is_shown_keeps_that_block(tmp_path):
    """A letter typed with a block marked, the block's start set again, and
    Ctrl-U: the block stays where it is now, moved by the letter taken out,
    not the one it was before the letter."""
    found = _replay(tmp_path, "r022", [
        ("type", b"a"), ("type", b"b"), ("type", b"c"), ("home", None), ("right", None),
        ("block_begin", None), ("end", None), ("block_end", None), ("home", None),
        ("type", b"x"), ("block_begin", None), ("undo", None)])
    assert _found(found, "undo", "marks") == []


# @scenario SC-edit-model-utf8-16
def test_undo_of_a_byte_typed_over_a_wide_character_marks_the_character(tmp_path):
    """Overwrite mode, a `}` typed over an `é`, a block set elsewhere, and
    Ctrl-U: the `é` comes back marked, as the text a replacement puts back."""
    found = _replay(tmp_path, "r020", [LIT, ("type", "é".encode()), ("left", None),
                                       ("type", b"}"), ("end", None), ("shift_left", None),
                                       ("undo", None)])
    assert _found(found, "undo", "marks") == []


# @scenario SC-edit-model-utf8-09
def test_block_move_of_a_one_byte_block_cannot_split_a_character(tmp_path):
    """A block whose end is inside an `é` holds one of its two bytes: Ctrl-K V
    carries that byte away from the other (the covering row r016)."""
    with _session(tmp_path, "ab\néx\n") as s:
        s.send([RIGHT, ctrl("y"), SHIFT_RIGHT, "\033[F"])
        w = s.send([CK, "v"]).win
        w.disk().decode("utf-8")
        _assert_no_position_trace(s)


# @scenario SC-edit-model-53
def test_backspace_below_a_soft_row_takes_its_last_character_whole(tmp_path):
    """Backspace at the start of the row below a row that goes on without a
    line break takes the whole last character of that row: an `é` at the
    margin is not cut in two."""
    seed = "x" * 18 + "é" + "tail\n"
    with _session(tmp_path, seed, filename="t.txt", xwperc=NARROW_RC) as s:
        w = s.send([DOWN, HOME]).win
        assert [r[0] for r in w.lines[:2]] == ["S", "H"], "the row was not cut at the margin"
        w = s.send([BACKSPACE]).win
        assert w.disk().decode("utf-8") == "x" * 18 + "tail\n"
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_goto_line_keeps_the_screen_column_and_lands_on_a_character(tmp_path):
    """Goto Line from the Search menu keeps the column the cursor was on, as
    Up and Down do: one column right on `ab` is after the `é` of the row it
    goes to, not between its bytes."""
    with _session(tmp_path, "ab\né\n") as s:
        s.send([RIGHT])
        w = s.send([alt("s"), "g", ctrl("u"), "2", ENTER]).win
        assert w.cursor == (1, 2), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_ctrl_y_keeps_the_screen_column_over_a_tab(tmp_path):
    """The cursor at the end of a row that starts with a tab is on screen
    column 10; the row that comes up under it is five columns wide, so the
    cursor is at its end, not at the byte column 3 that is inside its
    second character."""
    with _session(tmp_path, "\tab\nééééé\n") as s:
        w = s.send([END]).win
        assert w.cursor == (0, 3)
        w = s.send([ctrl("y")]).win
        assert w.cursor == (0, 10), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_ctrl_y_lands_on_the_character_under_the_column(tmp_path):
    with _session(tmp_path, "ab\néx\n") as s:
        s.send([RIGHT])
        w = s.send([ctrl("y")]).win
        assert w.cursor == (0, 2), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


def _sgr(button, col, row, press=True):
    """An SGR 1006 mouse report at the 1-based screen cell (col, row)."""
    return "\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")


# The right arrow of a window's status row, as a 1-based screen cell: the
# window of an 80x30 screen ends on row 21 and its arrow is two cells left of
# its right edge.
STATUS_ARROW_RIGHT = (78, 21)


# @scenario SC-edit-model-53
def test_the_status_bar_arrow_steps_one_character(tmp_path):
    """A click on the right arrow of the status bar moves the cursor one
    character, not one byte: on a row of ten `é` the byte step would leave it
    between the bytes of the first one, and the letter typed next would split
    that character.  The arrow repeats while the button is down, so the test
    asks for a character boundary, never an exact column."""
    from wpe_driver import WpeSession
    col, row = STATUS_ARROW_RIGHT
    with WpeSession(str(tmp_path), "é" * 10 + "\n") as w:
        w.key(_sgr(0, col, row, True), _sgr(0, col, row, False), delay=0.5)
        w.key("X")
        w.save()
        with open(w.path, "rb") as fh:
            data = fh.read()
        assert data.decode("utf-8") and data.startswith("é".encode()), \
            "the letter went between the bytes of a character: %r" % data
        assert b"X" in data and data.index(b"X") > 0, "the arrow did not move: %r" % data


# @scenario SC-edit-model-53
def test_a_marker_over_moved_text_lands_on_a_character(tmp_path):
    """Marker 1 is set between `b` and `é`; a `€` typed at the start of the
    row covers that column.  The marker keeps its place, and going back to it
    stands on the character that is there now, not between its bytes."""
    with _session(tmp_path, "abé\n") as s:
        s.send([RIGHT, RIGHT, CK, "1", HOME, "€"])
        w = s.send([ctrl("o"), "1"]).win
        assert w.cursor == (0, 0), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_search_again_goes_on_behind_a_hit_that_is_a_character(tmp_path):
    """Find puts the cursor behind the hit and Search Again goes on to the
    next one: each hit is the whole `é`, the find mark and the cursor stand on
    characters at every step."""
    with _session(tmp_path, "ééé\n") as s:
        w = s.send([ctrl("o"), "f", "é", ENTER]).win
        assert (w.cursor, w.pos) == ((0, 2), -1), "cursor %s: %s" % (w.cursor, _where(w))
        w = s.send([ctrl("l")]).win
        assert (w.cursor, w.pos) == ((0, 4), -1), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_no_position_trace(s)


# @scenario SC-edit-model-53
@still_open("a regular expression hit runs to the end of the row, so no hit can end inside a character")
def test_a_regular_expression_hit_inside_a_character_is_stepped_over(tmp_path):
    """The regular expression `.` matches the first byte of an `é`: the hit is
    shown and stepped over as the whole character, so Search Again goes on to
    the next one instead of finding the same byte again.  Today the hit of
    every regular expression ends at the row's end on the old editor and the
    new one alike, a defect of its own."""
    with _session(tmp_path, "éa\n") as s:
        w = s.send([ctrl("o"), "f", ".", "\033r", ENTER]).win
        assert w.cursor == (0, 2), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)
        w = s.send([ctrl("l")]).win
        assert w.cursor == (0, 3), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_block_delete_on_another_row_puts_the_cursor_at_the_block(tmp_path):
    """Ctrl-K Y on a block that lies on another row leaves the cursor where
    the deleted text was: the block's row and column, which stand on a
    character, not the cursor's row at the block's column."""
    with _session(tmp_path, "éa\nxyz\n") as s:
        s.send([DOWN, RIGHT, SHIFT_RIGHT, UP])
        w = s.send([CK, "y"]).win
        assert w.disk() == "éa\nxz\n".encode("utf-8")
        assert w.cursor == (1, 1), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_ctrl_left_stops_at_a_word_behind_a_character_of_several_bytes(tmp_path):
    """Ctrl-Left from the end of `¿Qu` lands on the `Q`, after the `¿` that
    starts the row, and Ctrl-O T from there deletes only the word: the walk
    steps over whole characters and stops where it always stopped."""
    with _session(tmp_path, "¿Qu tal\n") as s:
        s.send([RIGHT, RIGHT, RIGHT])
        w = s.send([CTRL_LEFT]).win
        assert w.cursor == (0, 2), "cursor %s: %s" % (w.cursor, _where(w))
        s.send([RIGHT, RIGHT])
        w = s.send([ctrl("o"), "t"]).win
        assert w.disk() == "¿ tal\n".encode("utf-8")
        assert w.cursor == (0, 2), "cursor %s: %s" % (w.cursor, _where(w))
        _assert_on_glyphs(w, s)


# @scenario SC-edit-model-53
def test_a_shift_selection_that_goes_back_past_its_start_is_a_normal_block(tmp_path):
    """Shift-Right and then Shift-Left twice select the character before where
    the selection began: the block is that character, and Ctrl-K Y deletes
    it."""
    with _session(tmp_path, "abcd\n") as s:
        w = s.send([RIGHT, RIGHT, SHIFT_RIGHT, SHIFT_LEFT, SHIFT_LEFT]).win
        assert (w.mb, w.me) == ((0, 1), (0, 2)), "marks %s %s" % (w.mb, w.me)
        w = s.send([CK, "y"]).win
        assert w.disk() == b"acd\n"
        _assert_no_position_trace(s)


# @scenario SC-edit-model-53
def test_delete_that_fuses_stray_bytes_leaves_the_cursor_before_the_character(tmp_path):
    """A Latin-1 file: the lone bytes C3 and A9 around an `x` become one
    character when the `x` goes; the cursor that stood between them stands
    before the character."""
    with _session(tmp_path, b"\xc3x\xa9\n") as s:
        s.send([RIGHT])
        w = s.send([DELETE]).win
        assert w.disk() == b"\xc3\xa9\n"
        assert w.cursor == (0, 0), "cursor %s" % (w.cursor,)
        _assert_no_position_trace(s)
