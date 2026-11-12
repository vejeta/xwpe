"""The model's blocks, clipboard, Replace and option dialog, on their own.

No editor runs here.  Each row writes down what a user sees from one
block command in a small text, and the walks at the end check what must
hold after any of them: Ctrl-U gives the text back as one step, the marks
and the cursor stay inside the text, a copy and a paste keep every byte,
and a breakpoint on a line that was not moved stays on it.
"""
import random

import pytest

pytest.importorskip("pyte")   # the key tables share the session's key names

from edit_model import blocks, keymap, model, render  # noqa: E402

A = model.Action
WIDTH = 120
TEXT = [("H", b"alpha one"), ("H", b"beta two"), ("H", b"gamma three"), ("H", b"delta")]


def _state(lines=TEXT, y=0, x=0, flg=0, width=WIDTH, **options):
    st = model.State()
    for end, data in lines:
        st.rows.append(model.Row(data, end, st.next_ident))
        st.next_ident += 1
    st.y, st.x, st.flg, st.width = y, x, flg, width
    st.mb = st.me = (len(lines) - 1, len(lines[-1][1]))
    for k, v in options.items():
        setattr(st.options, k, v)
    return st


def _do(st, *actions):
    for a in actions:
        if isinstance(a, str):
            a = A(a)
        st = model.apply(st, a)
    return st


def _mark(st, begin, end):
    """The block from @begin to @end, set the way a user does: cursor, Ctrl-K B,
    cursor, Ctrl-K K."""
    st.y, st.x = begin
    st = _do(st, "block_begin")
    st.y, st.x = end
    return _do(st, "block_end")


def _at(st, y, x):
    st.y, st.x = y, x
    return st


def _rows(st):
    return st.lines()


# @scenario SC-edit-model-19
def test_every_block_rule_names_its_source_and_where_it_is_written():
    """The rules the block family adds register in the same table as the
    key loop's, each with a source (the manual, the chapter, the user's
    decision, a probe of the editor, the design or the code) and a place."""
    added = [r for r in model.RULES.values() if r.name.startswith(
        ("block_", "indent_", "help_markers", "clip_", "marker_", "replace_",
         "option_", "save_fails"))]
    assert len(added) >= 25
    for r in added:
        assert r.source in model.SOURCES and r.ref and r.says and r.fields, r
        assert set(r.fields) <= set(model.FIELDS), r
    sources = set(r.source for r in added)
    assert {model.SOURCE_MANUAL, model.SOURCE_CODE, model.SOURCE_DECISION} <= sources


# @scenario SC-edit-model-19
def test_every_block_action_has_keys_in_both_key_styles():
    """Each block, clipboard, replace, option and marker action is made by
    keys in the old and in the CUA style, and every clipboard alias stands
    for a block action."""
    for style in keymap.STYLES:
        for name in blocks.BLOCK_ACTIONS:
            assert keymap.keys_for(style, name, keymap.sample_arg(name)), name
    for alias in keymap.CLIPBOARD_ALIASES:
        assert keymap.model_action(alias) in blocks.BLOCK_ACTIONS


# @scenario SC-edit-model-19
def test_begin_mark_starts_an_empty_block_unless_the_block_mode_is_wordstar():
    """Ctrl-K B makes an empty block at the cursor in the modern mode, so
    marking again leaves no old block; in WordStar mode it moves only the
    beginning and the end stays."""
    st = _mark(_state(), (0, 2), (2, 3))
    assert (st.mb, st.me) == ((0, 2), (2, 3))
    modern = _do(_at(st, 3, 1), "block_begin")
    assert (modern.mb, modern.me) == ((3, 1), (3, 1))
    ws = _state(wordstar=True)
    ws = _mark(ws, (0, 2), (2, 3))
    ws = _do(_at(ws, 3, 1), "block_begin")
    assert (ws.mb, ws.me) == ((3, 1), (2, 3)), "the end stays: an inverted block"


# @scenario SC-edit-model-19
def test_mark_line_mark_all_hide_and_goto_move_the_marks_and_the_cursor():
    st = _state()
    st = _do(_at(st, 1, 3), "block_line")
    assert (st.mb, st.me) == ((1, 0), (2, 0))
    st = _do(_at(st, 3, 2), "block_line")
    assert (st.mb, st.me) == ((3, 0), (3, 5)), "the last line ends at its end"
    st = _do(st, "block_all")
    assert (st.mb, st.me) == ((0, 0), (3, 5))
    st = _do(_at(st, 2, 2), "block_goto_end")
    assert (st.y, st.x) == (3, 5)
    st = _do(st, "block_goto_begin")
    assert (st.y, st.x) == (0, 0)
    st = _do(st, "block_hide")
    assert (st.mb, st.me) == ((0, 0), (0, 0))


# @scenario SC-edit-model-20
def test_copy_puts_the_block_at_the_cursor_marks_the_copy_and_is_one_undo():
    """Ctrl-K C on the first line, dropped above the last: the copy is
    marked, the cursor is at its end, the modified count is Max. Changes + 1,
    and one Ctrl-U takes it back and leaves both marks where it began."""
    st = _mark(_state(), (0, 0), (1, 0))
    st = _at(st, 3, 0)
    done = _do(st, "block_copy")
    assert _rows(done)[3:5] == [("H", b"alpha one"), ("H", b"delta")]
    assert (done.mb, done.me, (done.y, done.x)) == ((3, 0), (4, 0), (4, 0))
    # Max. Changes + 1 is above the limit, so the autosave check that opens
    # the next key's turn sets the count to 1 before the editor waits again
    assert done.save == 1 and done.ud == 1
    back = _do(done, "undo")
    assert _rows(back) == _rows(st)
    assert (back.mb, back.me) == ((3, 0), (3, 0))
    assert _rows(_do(back, "redo")) == _rows(done)


# @scenario SC-edit-model-20
def test_a_copy_that_would_land_inside_its_own_block_is_refused():
    st = _mark(_state(), (0, 2), (2, 3))
    for y, x in ((1, 0), (0, 4), (2, 1)):
        same = _do(_at(st, y, x), "block_copy")
        assert _rows(same) == _rows(st) and same.ud == 0, (y, x)
    ok = _do(_at(st, 3, 1), "block_copy")
    assert ok.ud == 1


# @scenario SC-edit-model-20
def test_an_empty_or_inverted_block_is_refused_by_copy_move_and_delete():
    for mb, me in (((1, 2), (1, 2)), ((2, 0), (1, 0))):
        st = _state()
        st.mb, st.me = mb, me
        st.y, st.x = 3, 0
        for name in ("block_copy", "block_move", "block_delete", "edit_cut", "edit_copy"):
            same = _do(st, name)
            assert _rows(same) == _rows(st) and same.ud == 0, (name, mb, me)
            assert same.clip == st.clip, name


# @scenario SC-edit-model-20
def test_move_takes_the_block_to_the_cursor_and_marks_it_there():
    st = _mark(_state(), (0, 0), (1, 0))
    st = _at(st, 3, 0)
    done = _do(st, "block_move")
    assert [d for _e, d in _rows(done)] == [b"beta two", b"gamma three", b"alpha one", b"delta"]
    assert (done.mb, done.me, (done.y, done.x)) == ((2, 0), (3, 0), (3, 0))
    assert done.ud == 1
    assert _rows(_do(done, "undo")) == _rows(st)
    back = _do(_at(_mark(_state(), (2, 0), (3, 0)), 0, 0), "block_move")
    assert [d for _e, d in _rows(back)] == [b"gamma three", b"alpha one", b"beta two", b"delta"]


# @scenario SC-edit-model-20
def test_a_move_with_the_cursor_inside_the_block_is_refused():
    st = _mark(_state(), (0, 2), (2, 3))
    for y, x in ((0, 2), (1, 0), (2, 3)):
        same = _do(_at(st, y, x), "block_move")
        assert _rows(same) == _rows(st), (y, x)
    ok = _do(_at(st, 3, 1), "block_move")
    assert _rows(ok) != _rows(st)


# @scenario SC-edit-model-20
def test_delete_inside_one_row_closes_the_gap_and_across_rows_joins_the_ends():
    st = _mark(_state(), (0, 2), (0, 5))
    one = _do(st, "block_delete")
    assert _rows(one)[0] == ("H", b"al one")
    assert (one.mb, one.me, one.x) == ((0, 2), (0, 2), 2)
    many = _do(_mark(_state(), (0, 5), (2, 5)), "block_delete")
    assert [d for _e, d in _rows(many)] == [b"alpha" + b"gamma three"[5:], b"delta"]
    assert (many.mb, many.me, (many.y, many.x)) == ((0, 5), (0, 5), (0, 5))
    assert many.save == 1 and many.ud == 1
    assert [d for _e, d in _rows(_do(many, "undo"))] == [d for _e, d in TEXT]


# @scenario SC-edit-model-21
def test_copy_cut_and_paste_through_the_clipboard_window():
    """Ctrl-C leaves the clipboard holding the block (a block that ends at
    a line start leaves an empty last row) and the text alone; Ctrl-V puts
    it at the cursor; Ctrl-X is Ctrl-C and then deleting the block; an
    empty clipboard pastes nothing."""
    st = _mark(_state(), (0, 0), (1, 0))
    copied = _do(st, "edit_copy")
    assert copied.clip == [("H", b"alpha one"), ("H", b"")]
    assert _rows(copied) == _rows(st) and copied.save == st.save and copied.ud == 0
    pasted = _do(_at(copied, 2, 0), "edit_paste")
    assert [d for _e, d in _rows(pasted)][2:4] == [b"alpha one", b"gamma three"]
    assert pasted.ud == 1 and pasted.save == 1
    cut = _do(st, "edit_cut")
    assert cut.clip == [("H", b"alpha one"), ("H", b"")]
    assert [d for _e, d in _rows(cut)] == [b"beta two", b"gamma three", b"delta"]
    assert _rows(_do(_state(), "edit_paste")) == _rows(_state()), "nothing to paste"
    one = _do(_mark(_state(), (0, 2), (0, 5)), "edit_copy")
    assert one.clip == [("H", b"pha")]
    assert _do(_at(one, 3, 2), "edit_paste").lines()[3] == ("H", b"dephalta")


# @scenario SC-edit-model-21
def test_a_locked_window_refuses_every_text_change_but_copy_works():
    """In a window closed to changes: Copy and Ctrl-K C fill the
    clipboard, Cut copies and refuses the delete, and Paste, Move, Delete,
    Indent, Unindent, the help markers and Change All change nothing."""
    st = _mark(_state(), (0, 0), (1, 0))
    st.ins = model.INS_READONLY
    assert _do(st, "edit_copy").clip != st.clip
    assert _do(st, "block_copy").clip != st.clip
    cut = _do(st, "edit_cut")
    assert cut.clip != st.clip and _rows(cut) == _rows(st)
    full = _do(st, "edit_copy")
    for name in ("edit_paste", "block_move", "block_delete", "block_indent",
                 "block_unindent", "help_button", "help_header"):
        assert _rows(_do(full, name)) == _rows(st), name
    assert _rows(_do(full, A("replace_all", (b"a", b"Q")))) == _rows(st)


# @scenario SC-edit-model-21
def test_a_refused_command_in_a_locked_window_says_so_on_the_status_row():
    """Ctrl-K I and U, Ctrl-O U, M, H and E, and Change All are refused in a
    read-only window and the status row says the text is unchanged; the
    block Delete, Move, Paste and Copy, typing and Undo say nothing."""
    say = model.STATUS_READ_ONLY
    st = _mark(_state([("H", b"  alpha one"), ("H", b"beta")]), (0, 0), (1, 0))
    st.ins = model.INS_READONLY
    assert _do(st, "block_indent").status == say
    assert _do(st, "block_unindent").status == say
    inner = _mark(_state(), (0, 2), (0, 5))
    inner.ins = model.INS_READONLY
    for name in ("help_button", "help_highlight", "help_header", "help_end"):
        assert _do(inner, name).status == say, name
    assert _do(inner, A("replace_all", (b"a", b"Q"))).status == say
    for name in ("block_delete", "block_move", "edit_paste", "block_copy", "undo"):
        assert _do(inner, name).status is None, name
    typed = _do(inner, A("type", b"x"))
    assert typed.status is None and _rows(typed) == _rows(inner)
    # nothing to attempt, nothing said: a shift left of rows with no indent,
    # and a block that is not inside one row
    flush = _mark(_state(), (0, 0), (1, 0))
    flush.ins = model.INS_READONLY
    assert _do(flush, "block_unindent").status is None
    assert _do(flush, "help_button").status is None
    # a writable window never says it
    free = _mark(_state(), (0, 0), (1, 0))
    assert _do(free, "block_indent").status is None


# @scenario SC-edit-model-21
def test_change_all_in_a_locked_window_finds_but_replaces_nothing():
    """The box says how many were found and that none was replaced, with the
    read-only words; a string not there is not found as in any window."""
    st = _state()
    st.ins = model.INS_READONLY
    done = _do(st, A("replace_all", (b"a", b"Q")))
    assert _rows(done) == _rows(st) and done.ud == 0
    assert done.box == "Found 6 Replaced 0 " + model.STATUS_READ_ONLY
    assert _do(st, A("replace_all", (b"zzz", b"Q"))).box == blocks.MSG_NOT_FOUND


# @scenario SC-edit-model-23
def test_ctrl_o_e_puts_the_end_of_page_mark_on_a_row_of_its_own_in_one_step():
    """Above the cursor's row, a hard row holding the mark; the cursor just
    after the mark on the new row; one Ctrl-U takes the row away."""
    st = _at(_state(), 1, 3)
    done = _do(st, "help_end")
    assert _rows(done)[:3] == [("H", b"alpha one"), ("H", bytes([blocks.HELP_PAGE_END])),
                               ("H", b"beta two")]
    assert (done.y, done.x) == (1, 1) and done.ud == 1
    assert _rows(_do(done, "undo")) == _rows(st)
    assert "help_end_line" in done.fired and "help_end_one_step" in done.fired


# @scenario SC-edit-model-22
def test_a_copy_keeps_the_rows_between_its_ends_as_they_are():
    """The rows a block holds between its first and last line are copied
    row for row, soft rows included, where the lines at its two ends are
    wrapped again as the file reader wraps."""
    long_line = [("S", b"w000 w001 w002 w003 w004 w005 w006 w007 w008 w009 w010 w011 "),
                 ("H", b"w012 w013 w014 w015 w016 w017 w018 w019 w020 w021 w022xy")]
    st = _state([("H", b"top")] + long_line + [("H", b"end")], width=60)
    st = _at(_mark(st, (0, 0), (3, 0)), 3, 0)
    done = _do(st, "block_copy")
    assert _rows(done)[3:6] == [("H", b"top")] + long_line
    again = _state([("H", b"top")] + long_line + [("H", b"end")], width=60)
    wide = _do(_at(_mark(again, (1, 0), (2, 56)), 0, 0), "block_copy")
    assert wide.lines()[0][0] == model.SOFT and len(wide.lines()[0][1]) <= 60, \
        "the line at an end is wrapped again at the width"


# @scenario SC-edit-model-22
@pytest.mark.parametrize("piece,width,want", [
    (b"aaa bbb ccc", 8, [(b"aaa bbb ", "S"), (b"ccc", "H")]),
    (b"aaa-bbb-ccc", 8, [(b"aaa-bbb-", "S"), (b"ccc", "H")]),
    (b"abcdefghijkl", 8, [(b"abcdefgh", "S"), (b"ijkl", "H")]),
    (b"abcdefgh", 8, [(b"abcdefgh", "H")]),
    (b"", 8, [(b"", "H")]),
    (b"short", 8, [(b"short", "H")]),
])
def test_the_file_readers_wrap_breaks_after_a_blank_or_a_dash(piece, width, want):
    got = blocks.wrap_reader(piece, width, True)
    assert got == want, got


# @scenario SC-edit-model-22
def test_the_wrap_never_cuts_a_utf8_character_in_two():
    piece = ("é" * 12).encode()
    for width in (5, 6, 7, 9):
        for data, _end in blocks.wrap_reader(piece, width, True):
            data.decode("utf-8")


# @scenario SC-edit-model-23
def test_indent_and_unindent_shift_by_half_the_tabstops_one_undo_for_all_rows():
    """Ctrl-K I adds Tabstops / 2 blanks in front of each row the block
    starts in (the first only when the block begins at column 0); Ctrl-K U
    takes as many away; each is one Ctrl-U for the whole block."""
    st = _mark(_state(), (0, 0), (3, 0))
    done = _do(st, "block_indent")
    assert [d for _e, d in _rows(done)] == [b"    alpha one", b"    beta two",
                                            b"    gamma three", b"delta"]
    assert done.ud == 1 and _rows(_do(done, "undo")) == _rows(st)
    back = _do(done, "block_unindent")
    assert [d for _e, d in _rows(back)] == [d for _e, d in TEXT]
    inner = _do(_mark(_state(), (0, 3), (2, 2)), "block_indent")
    assert [d for _e, d in _rows(inner)][:3] == [b"alpha one", b"    beta two",
                                                  b"    gamma three"]
    narrow = _do(_mark(_state(tabn=4), (0, 0), (1, 0)), "block_indent")
    assert _rows(narrow)[0] == ("H", b"  alpha one")


# @scenario SC-edit-model-23
def test_a_row_of_only_blanks_is_indented_after_its_own_blanks():
    """The scan for the first visible character stops at the end of a row of
    blanks (the editor fixed the read past its end), so the row gets the
    blanks after its own, and an empty row gets them at column 0."""
    st = _mark(_state([("H", b"one"), ("H", b"   "), ("H", b""), ("H", b"three")]),
               (0, 0), (3, 5))
    done = _do(st, "block_indent")
    assert [d for _e, d in _rows(done)] == [b"    one", b"       ", b"    ", b"    three"]
    assert "indent_blank_row" in done.fired and done.ud == 1


# @scenario SC-edit-model-23
def test_a_shift_left_over_a_tab_puts_back_the_columns_beyond_it_as_blanks():
    """When the last thing a shift to the left takes is a tab worth more
    columns than the shift, the columns beyond it come back as blanks, so
    the row moves exactly Tabstops / 2 to the left; the model predicts every
    byte and takes nothing from the editor."""
    # a blank after a tab at column 0 worth 8: k = 9, put back 5 = n + 1
    near = _mark(_state([("H", b"\t x")]), (0, 0), (0, 3))
    near.mb, near.me = (0, 0), (0, 3)
    st = _do(_at(near, 0, 0), "block_unindent")
    assert [d for _e, d in _rows(st)] == [b" " * 5 + b"x"]
    assert "unindent_puts_back_blanks" in st.fired
    assert st.resync_fields == set()
    # three blanks after a tab: k = 11, put back 7 = n + 3
    far = _mark(_state([("H", b"\t   x")]), (0, 0), (0, 4))
    far.mb, far.me = (0, 0), (0, 4)
    st = _do(_at(far, 0, 0), "block_unindent")
    assert [d for _e, d in _rows(st)] == [b" " * 7 + b"x"]
    assert "unindent_puts_back_blanks" in st.fired
    assert st.resync_fields == set()


# @scenario SC-edit-model-23
def test_the_help_file_markers_go_around_a_block_inside_one_row_as_one_undo():
    st = _mark(_state(), (0, 2), (0, 5))
    for name, byte in (("help_button", blocks.HELP_BUTTON),
                       ("help_highlight", blocks.HELP_HIGHLIGHT),
                       ("help_header", blocks.HELP_HEADER)):
        done = _do(st, name)
        want = b"al" + bytes([byte]) + b"pha" + bytes([blocks.HELP_END]) + b" one"
        assert _rows(done)[0] == ("H", want), name
        assert done.ud == 1 and _rows(_do(done, "undo")) == _rows(st), name
    several = _mark(_state(), (0, 2), (1, 2))
    assert _rows(_do(several, "help_button")) == _rows(several)


# @scenario SC-edit-model-24
def test_change_all_replaces_every_match_ignoring_case_and_says_how_many():
    """Search > Replace with Global and no prompt: every match, from the
    text's start, case ignored; the box counts them; one Ctrl-U takes them
    all back whatever Num. Undo is."""
    st = _state([("H", b"Foo foo"), ("H", b"bar FOO"), ("H", b"none")], numundo=0)
    done = _do(st, A("replace_all", (b"foo", b"Q")))
    assert [d for _e, d in _rows(done)] == [b"Q Q", b"bar Q", b"none"]
    assert done.box == "Found 3 Replaced 3"
    assert done.ud == 1 and _rows(_do(done, "undo")) == _rows(st)
    nothing = _do(st, A("replace_all", (b"zzz", b"Q")))
    assert nothing.box == "String NOT found" and _rows(nothing) == _rows(st)
    assert nothing.ud == 0


# @scenario SC-edit-model-24
def test_change_all_goes_on_after_a_replacement_and_never_matches_across_rows():
    st = _state([("H", b"aaaa"), ("H", b"ab"), ("H", b"ba")])
    grown = _do(st, A("replace_all", (b"a", b"aa")))
    assert [d for _e, d in _rows(grown)] == [b"aaaaaaaa", b"aab", b"baa"]
    assert grown.box == "Found 6 Replaced 6"
    span = _do(_state([("H", b"xa"), ("H", b"by")]), A("replace_all", (b"ab", b"Q")))
    assert span.box == "String NOT found"


# @scenario SC-edit-model-25
def test_the_option_dialog_changes_the_numbers_and_not_the_width_of_an_open_window():
    st = _state(width=118)
    st = _do(st, A("option_set", ("maxcol", 40)))
    assert st.options.maxcol == 40 and st.width == 118
    st = _do(st, A("option_set", ("tabn", 4)), A("option_set", ("maxchg", 7)),
             A("option_set", ("numundo", 2)), A("option_set", ("autoindent_col", 5)))
    assert (st.options.tabn, st.options.maxchg, st.options.numundo,
            st.options.autoindent_col) == (4, 7, 2, 5)
    assert st.ud == 0 and _rows(st) == _rows(_state(width=118))


# @scenario SC-edit-model-25
def test_the_markers_remember_a_place_and_an_unset_one_goes_to_the_start():
    st = _do(_at(_state(), 2, 4), A("marker_set", 3))
    st = _do(_at(st, 0, 0), A("marker_goto", 3))
    assert (st.y, st.x) == (2, 4)
    far = _do(_at(st, 3, 2), A("marker_goto", 5))
    assert (far.y, far.x) == (0, 0)
    ninth = _do(_at(st, 3, 2), A("marker_set", 9))
    assert len(ninth.pt) == 10 and ninth.pt[9] == (3, 2), "there are ten markers, 0 to 9"
    assert ninth.pt[:9] == st.pt[:9], "setting marker 9 moves no other marker"


# @scenario SC-edit-model-26
def test_a_tab_with_auto_indent_has_a_manual_rule_that_the_code_contradicts():
    """The manual says Tab behind the first visible character indents by
    Tabstops; the code uses Auto Ind. Col.  The model follows the code, the
    conflict is registered, and the approval list gives it to the user."""
    rule = model.RULES["tab_autoindent_manual"]
    assert rule.source == model.SOURCE_MANUAL and "2045" in rule.ref
    assert "tab_autoindent" in model.CONFLICTS
    import os
    path = os.path.join(os.path.dirname(__file__), "edit_model", "goldens", "APPROVAL.md")
    with open(path, encoding="utf-8") as fh:
        text = fh.read()
    assert "To decide" in text and "Auto Ind. Col." in text and "Tabstops" in text


# ---- what must hold after any block action ------------------------------------------------

POOL = ["block_begin", "block_end", "block_line", "block_all", "block_hide",
        "block_copy", "block_move", "block_delete", "block_indent",
        "block_unindent", "edit_copy", "edit_cut", "edit_paste", "help_button",
        "down", "up", "end", "home", "enter", "type", "backspace", "delete"]
CHANGING = ("block_copy", "block_move", "block_delete", "block_indent",
            "block_unindent", "edit_cut", "edit_paste", "help_button")
WALKS = 40
WALK_LEN = 50
SEED_ROWS = [("H", b"int main(void)"), ("H", b"{"), ("H", b"\tif (a) {"),
             ("H", b"    int a = 1;"), ("H", b""), ("H", "café au lait".encode()),
             ("H", b" ".join(b"w%03d" % i for i in range(23)) + b"xy"), ("H", b"last")]


def _draw(rng):
    name = rng.choice(POOL)
    if name == "type":
        return A(name, rng.choice([b"x", b" ", b"-", "é".encode()]))
    return A(name)


def _inside(st):
    bad = []
    rows = st.rows
    if not (0 <= st.y < len(rows) and 0 <= st.x <= len(rows[st.y].data)):
        bad.append("cursor %s" % ((st.y, st.x),))
    for name, (y, x) in (("mb", st.mb), ("me", st.me)):
        if not (0 <= y < len(rows) and 0 <= x <= len(rows[y].data) + 1):
            bad.append("%s %s" % (name, (y, x)))
    return bad


# @scenario SC-edit-model-27
@pytest.mark.parametrize("width", [WIDTH, 40])
@pytest.mark.parametrize("seed", range(WALKS))
def test_after_any_block_action_undo_gives_the_text_back_as_one_step(seed, width):
    """From a seeded walk of block commands, cursor keys and edits: a
    changing action is one undo step, Ctrl-U gives the rows, their
    identities and the breakpoints back, Ctrl-R the rows after it; and the
    cursor and the marks stay inside the text."""
    rng = random.Random("%d-%d" % (seed, width))
    st = _state(SEED_ROWS, width=width)
    st = _do(_do(st, "block_all"), "block_hide")
    for _ in range(WALK_LEN):
        act = _draw(rng)
        after = model.apply(st, act)
        changed = after.lines() != st.lines()
        if changed and act.name != "type":
            assert after.ud == min(st.ud + 1, st.options.numundo + 1), (act, "one step")
        if changed:
            back = model.apply(after, A("undo"))
            assert [(r.data, r.end, r.ident) for r in back.rows] == \
                [(r.data, r.end, r.ident) for r in st.rows], act
            assert back.brp_rows() == st.brp_rows(), act
            again = model.apply(back, A("redo"))
            assert again.lines() == after.lines(), act
        assert _inside(after) == [], (act, _inside(after))
        st = after


# @scenario SC-edit-model-27
@pytest.mark.parametrize("seed", range(WALKS))
def test_a_copy_and_a_paste_keep_every_byte_of_the_text_and_the_block(seed):
    """Reading the rows as the file they make, Ctrl-K C and Ctrl-V insert
    exactly the block's bytes at the cursor, in a window as wide as the
    lines and in one narrower than some of them."""
    rng = random.Random("copy-%d" % seed)
    for width in (WIDTH, 40):
        st = _state(SEED_ROWS, width=width)
        for _ in range(12):
            y = rng.randrange(len(st.rows))
            st = _at(st, y, rng.randint(0, len(st.rows[y].data)))
            st = _do(st, "block_begin")
            y = rng.randrange(len(st.rows))
            st = _at(st, y, rng.randint(0, len(st.rows[y].data)))
            st = _do(st, "block_end")
            y = rng.randrange(len(st.rows))
            st = _at(st, y, rng.randint(0, len(st.rows[y].data)))
            before = render.render_disk(st.lines())
            a, b = blocks._block_span(st)
            p = blocks._point_offset(st, st.y, st.x)
            copied = _do(st, "block_copy")
            if copied.lines() != st.lines():
                assert render.render_disk(copied.lines()) == before[:p] + before[a:b] + before[p:], \
                    (a, b, p)
            pasted = _do(_do(st, "edit_copy"), "edit_paste")
            if pasted.lines() != st.lines():
                assert render.render_disk(pasted.lines()) == before[:p] + before[a:b] + before[p:], \
                    (a, b, p)
            st = copied if copied.lines() != st.lines() else st


# @scenario SC-edit-model-27
def test_a_breakpoint_on_a_line_a_copy_did_not_touch_stays_on_it():
    st = _state()
    st = _do(_at(st, 2, 0), "toggle_breakpoint")
    marked = _mark(st, (0, 0), (1, 0))
    done = _do(_at(marked, 3, 0), "block_copy")
    assert done.brp_rows() == [2], "the copy below carries no breakpoint"
    above = _do(_at(_mark(st, (1, 0), (2, 0)), 0, 0), "block_copy")
    assert above.brp_rows() == [3], "a copy above pushes the breakpoint down with its line"
    pasted = _do(_at(_do(_mark(st, (0, 0), (1, 0)), "edit_copy"), 0, 0), "edit_paste")
    assert pasted.brp_rows() == [3]
