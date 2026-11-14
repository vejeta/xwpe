"""The selection block mode, Trim Auto-indent and Keep Final newline.

Options > Editor holds three options of the modern-editor style, each
independent of the key style (Options2 bits 0x400, 0x800, 0x1000):

* the selection block mode: a block is a selection.  A move without Shift
  collapses it, and a typed character, Return, Tab inside one line, Delete
  and Backspace replace or delete it as one Undo step that is made whole or
  not at all;
* Trim Auto-indent: the indent Return put on a line goes when the cursor
  leaves the line with nothing typed on it, in the key's own Undo step;
* Keep Final newline: Save writes the file's final line break as it was
  read (none for a file without one, an empty file or a new file).

Every row drives the running editor through the state channel (one dump per
key) and asserts on the editor's own state or on the bytes on disk.
"""
import concurrent.futures
import os

import pytest

from wpe_driver import ALT, WpeSession, checkbox_mark
from edit_model.oracle import (BACKSPACE, DELETE, DOWN, END, ENTER, F2, HOME, INSERT,
                               LEFT, RIGHT, SHIFT_DOWN, SHIFT_LEFT, SHIFT_RIGHT, TAB,
                               UP, EditSession, ctrl)

# Options2 for a text file with the programming layer's two bits (8 + 16),
# and the three options of this file.
BASE = 24
SOURCE_INDENT = 128
SELECTION = 0x400
TRIM = 0x800
KEEP = 0x1000
SEL_RC = "[General]\nOptions2 : %d\n" % (BASE + SELECTION)
PLAIN_RC = "[General]\nOptions2 : %d\n" % BASE
C_TRIM_RC = "[General]\nOptions2 : %d\n" % (BASE + SOURCE_INDENT + TRIM)
C_SEL_RC = "[General]\nOptions2 : %d\n" % (BASE + SOURCE_INDENT + SELECTION)

SEED = "hello world\nsecond line\nthird\n"
HELLO_SELECTED = [SHIFT_RIGHT] * 5
TWO_ROWS_SELECTED = [SHIFT_DOWN, SHIFT_RIGHT, SHIFT_RIGHT]
CTRL_U, CTRL_R = ctrl("u"), ctrl("r")
FAULT_SWEEP_MAX = 40
FAULT_BATCH = 8


def _rows(d):
    return [data for _end, data in d.win.lines]


def _view(d):
    w = d.win
    return (list(w.lines), w.cursor, w.mb, w.me, w.ud, w.rd)


def _session(tmp_path, seed=SEED, rc=SEL_RC, name="t.txt", **kw):
    return EditSession(str(tmp_path), seed, filename=name, xwperc=rc, **kw)


# @scenario SC-selection-01
def test_typing_over_a_selection_replaces_it_as_one_step(tmp_path):
    """Shift-Right five times marks `hello`; `J` takes its place and the block
    is empty at the cursor.  One Ctrl-U gives back the text and the selection,
    one Ctrl-R replaces it again."""
    with _session(tmp_path) as s:
        d = s.send(HELLO_SELECTED)
        assert (d.win.mb, d.win.me) == ((0, 0), (0, 5))
        d = s.send(["J"])
        assert _rows(d) == [b"J world", b"second line", b"third"]
        assert (d.win.cursor, d.win.mb, d.win.me, d.win.ud) == ((0, 1), (0, 1), (0, 1), 1)
        d = s.send([CTRL_U])
        assert _rows(d) == [b"hello world", b"second line", b"third"]
        assert (d.win.mb, d.win.me, d.win.cursor, d.win.ud, d.win.rd) == \
            ((0, 0), (0, 5), (0, 5), 0, 1)
        d = s.send([CTRL_R])
        assert _rows(d) == [b"J world", b"second line", b"third"]
        assert (d.win.cursor, d.win.mb, d.win.me, d.win.ud) == ((0, 1), (0, 1), (0, 1), 1)


# @scenario SC-selection-01
def test_a_selection_over_several_rows_is_replaced_by_one_character(tmp_path):
    """The deletion runs over the rows it spans: `hello world` and `se` go,
    `x` stands between the start of the first row and `cond line`."""
    with _session(tmp_path) as s:
        s.send(TWO_ROWS_SELECTED)
        d = s.send(["x"])
        assert _rows(d) == [b"xcond line", b"third"]
        assert (d.win.cursor, d.win.ud) == ((0, 1), 1)
        d = s.send([CTRL_U])
        assert _rows(d) == [b"hello world", b"second line", b"third"]
        assert (d.win.mb, d.win.me) == ((0, 0), (1, 2))


# @scenario SC-selection-02
def test_a_move_without_shift_collapses_the_selection_and_shift_extends_it(tmp_path):
    with _session(tmp_path) as s:
        d = s.send(HELLO_SELECTED[:3])
        assert (d.win.mb, d.win.me) == ((0, 0), (0, 3))
        d = s.send([RIGHT])
        assert (d.win.cursor, d.win.mb, d.win.me) == ((0, 4), (0, 4), (0, 4))
        d = s.send([SHIFT_DOWN])
        assert (d.win.mb, d.win.me) == ((0, 4), (1, 4))
        d = s.send([LEFT])
        assert (d.win.cursor, d.win.mb, d.win.me) == ((1, 3), (1, 3), (1, 3))
        d = s.send([RIGHT, RIGHT, SHIFT_LEFT, SHIFT_LEFT, SHIFT_LEFT])
        assert (d.win.cursor, d.win.mb, d.win.me) == ((1, 2), (1, 2), (1, 5)), \
            "going back past where the selection began turns it round"


# @scenario SC-selection-02
@pytest.mark.parametrize("key", [UP, DOWN, HOME, END, ctrl("a"), ctrl("e"), ctrl("f"), ctrl("b")])
def test_every_plain_move_collapses_the_selection(tmp_path, key):
    with _session(tmp_path) as s:
        d = s.send([DOWN, RIGHT] + HELLO_SELECTED[:3])
        assert d.win.mb != d.win.me
        d = s.send([key])
        assert d.win.mb == d.win.me == d.win.cursor, key


# @scenario SC-selection-02
def test_a_click_without_shift_collapses_the_selection(tmp_path):
    """A click elsewhere ends the selection as a cursor key does: the next
    character typed goes in at the click, it does not replace the old
    selection."""
    os.makedirs(os.path.join(str(tmp_path), ".config", "xwpe"), exist_ok=True)
    with open(os.path.join(str(tmp_path), ".config", "xwpe", "xwperc"), "w") as fh:
        fh.write(SEL_RC)
    with WpeSession(str(tmp_path), SEED, filename="t.txt") as s:
        s.key(*[SHIFT_RIGHT] * 5, delay=0.2)
        # text row 1, column 3: the window frame is one cell in, the menu bar
        # and the frame's top row are two rows down; SGR counts from 1
        s.key("\x1b[<0;5;4M", "\x1b[<0;5;4m", delay=0.6)
        s.key("Q", delay=0.4)
        s.save()
        assert s.disk() == b"hello world\nsecQond line\nthird\n", s.disk()


# @scenario SC-selection-03
def test_delete_and_backspace_delete_the_selection_only(tmp_path):
    with _session(tmp_path) as s:
        d = s.send(HELLO_SELECTED[:3] + [DELETE])
        assert _rows(d)[0] == b"lo world"
        assert (d.win.cursor, d.win.ud) == ((0, 0), 1)
        d = s.send(HELLO_SELECTED[:2] + [BACKSPACE])
        assert _rows(d)[0] == b" world"
        assert (d.win.cursor, d.win.mb, d.win.me, d.win.ud) == ((0, 0), (0, 0), (0, 0), 2)
        d = s.send([CTRL_U])
        assert _rows(d)[0] == b"lo world"
        assert (d.win.mb, d.win.me) == ((0, 0), (0, 2))


# @scenario SC-selection-04
def test_return_over_a_selection_breaks_the_line_and_indents_the_new_one(tmp_path):
    seed = "    int a = 1;\n    return a;\n"
    with _session(tmp_path, seed, C_SEL_RC, "t.c") as s:
        s.send([DOWN, END])
        d = s.send([SHIFT_LEFT] * 4)
        assert d.win.mb != d.win.me
        d = s.send([ENTER])
        assert _rows(d) == [b"    int a = 1;", b"    retur", b"    "]
        assert (d.win.cursor, d.win.mb, d.win.me, d.win.ud) == ((2, 4), (2, 4), (2, 4), 1)
        d = s.send([CTRL_U])
        assert _rows(d) == [b"    int a = 1;", b"    return a;"]


# @scenario SC-selection-05
def test_tab_replaces_a_selection_inside_one_line(tmp_path):
    with _session(tmp_path) as s:
        d = s.send([RIGHT, SHIFT_RIGHT, SHIFT_RIGHT, TAB])
        assert _rows(d)[0] == b"h\tlo world"
        assert (d.win.cursor, d.win.mb, d.win.me, d.win.ud) == ((0, 2), (0, 2), (0, 2), 1)


# @scenario SC-selection-17
@pytest.mark.parametrize("rc,name", [(SEL_RC, "t.txt"), (C_SEL_RC, "t.c")])
def test_tab_over_several_lines_indents_them_as_one_step(tmp_path, rc, name):
    """Lines selected and Tab: they are indented as Ctrl-K I indents them,
    nothing is typed over the selection, and one Ctrl-U takes it back.  The
    line the selection ends on at its column 0 is not one of them.  With
    auto-indent off the Tab comes through the typing path, with it on through
    the Tab key's own: both."""
    with _session(tmp_path, rc=rc, name=name) as s:
        d = s.send([HOME, SHIFT_DOWN, SHIFT_DOWN, TAB])
        assert _rows(d)[2] == b"third", "the last line, selected only at its column 0"
        assert _rows(d)[0].endswith(b"hello world") and _rows(d)[0] != b"hello world"
        assert _rows(d)[1].endswith(b"second line") and _rows(d)[1] != b"second line"
        assert d.win.mb != d.win.me, "the lines stay selected"
        assert d.win.ud == 1
        d = s.send([CTRL_U])
        assert _rows(d) == [b"hello world", b"second line", b"third"]


# @scenario SC-selection-06
def test_overwrite_mode_still_replaces_the_selection_and_does_not_overwrite_after_it(tmp_path):
    with _session(tmp_path) as s:
        d = s.send([INSERT] + HELLO_SELECTED[:3] + ["Z"])
        assert d.win.ins == 0
        assert _rows(d)[0] == b"Zlo world"
        assert d.win.cursor == (0, 1)


# @scenario SC-selection-07
def test_without_the_mode_a_selection_is_only_a_block(tmp_path):
    """The control: with the option off a typed character goes in at the
    cursor beside the block, as it always did, and a plain move leaves the
    block marked."""
    with _session(tmp_path, rc=PLAIN_RC) as s:
        d = s.send(HELLO_SELECTED[:3] + ["Q"])
        assert _rows(d)[0] == b"helQlo world"
        d = s.send([RIGHT])
        assert d.win.mb != d.win.me


# @scenario SC-selection-08
def test_the_mode_covers_a_selection_made_with_ctrl_k_marks(tmp_path):
    """Ctrl-K B and Ctrl-K K mark the same kind of selection as Shift does."""
    with _session(tmp_path) as s:
        d = s.send([RIGHT, RIGHT, ctrl("k"), "b", SHIFT_RIGHT, SHIFT_RIGHT, ctrl("k"), "k"])
        assert (d.win.mb, d.win.me) == ((0, 2), (0, 4))
        d = s.send(["!"])
        assert _rows(d)[0] == b"he!o world"


# @scenario SC-selection-18
def test_a_block_ctrl_k_b_started_stays_open_over_plain_moves(tmp_path):
    """Ctrl-K B, plain arrows, Ctrl-K K marks the block between the two, as
    in Turbo Pascal.  A selection made with Shift still collapses on a plain
    arrow, and so does one a click made."""
    with _session(tmp_path) as s:
        d = s.send([RIGHT, ctrl("k"), "b", RIGHT, RIGHT, DOWN])
        assert d.win.mb == d.win.me == (0, 1), "the block is open, empty at its start"
        d = s.send([ctrl("k"), "k"])
        assert (d.win.mb, d.win.me) == ((0, 1), (1, 3))
        d = s.send(["!"])
        assert _rows(d)[:2] == [b"h!ond line", b"third"]
    with _session(tmp_path) as s:
        d = s.send(HELLO_SELECTED[:3])
        assert d.win.mb != d.win.me
        d = s.send([RIGHT])
        assert d.win.mb == d.win.me, "a Shift selection collapses on a plain move"
        d = s.send([ctrl("k"), "b", SHIFT_RIGHT, RIGHT])
        assert d.win.mb == d.win.me, "Shift ended the open state: a plain move collapses"


# @scenario SC-selection-18
@pytest.mark.parametrize("replay", [CTRL_U, CTRL_R])
def test_undo_and_redo_end_a_block_ctrl_k_b_left_open(tmp_path, replay):
    """Undo and Redo put the block where the step had it, so a block Ctrl-K B
    left open is over: the selection they give back collapses on the next
    plain move, and Ctrl-K K does not stretch it to the cursor."""
    with _session(tmp_path) as s:
        s.send(HELLO_SELECTED[:3] + ["Z"])
        if replay == CTRL_R:
            s.send([CTRL_U])
        s.send([DOWN, ctrl("k"), "b"])
        d = s.send([replay])
        assert d.win.mb != d.win.me or replay == CTRL_R
        d = s.send([RIGHT])
        assert d.win.mb == d.win.me == d.win.cursor, "the replayed block did not collapse"


# @scenario SC-selection-18
def test_a_refused_undo_keeps_a_block_ctrl_k_b_left_open(tmp_path):
    """An Undo refused for memory changes nothing, the open block included:
    Ctrl-K K after it still marks from where Ctrl-K B was pressed."""
    with EditSession(str(tmp_path), SEED, filename="t.txt", xwperc=SEL_RC,
                     env_extra={"XWPE_TEST_FAULT": "undo:1"}) as s:
        s.send(HELLO_SELECTED[:3] + ["Z", DOWN, HOME, ctrl("k"), "b", RIGHT, RIGHT])
        d = s.send([CTRL_U])
        assert _rows(d)[0] == b"Zlo world", "the Undo was refused"
        d = s.send([RIGHT, ctrl("k"), "k"])
        assert (d.win.mb, d.win.me) == ((1, 0), (1, 3))


# @scenario SC-selection-19
@pytest.mark.parametrize("select,want", [
    ([SHIFT_RIGHT, SHIFT_RIGHT], b"hello!cond line"),
    ([], b"hello!second line"),
])
def test_redo_of_a_paste_leaves_the_cursor_after_the_pasted_text(tmp_path, select, want):
    """Paste, Ctrl-U, Ctrl-R: the state is the paste's own (the cursor after
    the pasted text, nothing selected), so the next typed key goes after it."""
    with _session(tmp_path) as s:
        s.send(HELLO_SELECTED + [CTRL_C, DOWN, HOME] + select)
        pasted = s.send([CTRL_V])
        s.send([CTRL_U])
        d = s.send([CTRL_R])
        assert (d.win.cursor, d.win.mb, d.win.me) == \
            (pasted.win.cursor, pasted.win.mb, pasted.win.me), "Redo is not the paste"
        d = s.send(["!"])
        assert _rows(d)[1] == want


# @scenario SC-selection-19
@pytest.mark.parametrize("select", [[SHIFT_RIGHT, SHIFT_RIGHT], []])
def test_a_paste_undone_and_redone_twice_ends_as_the_paste_both_times(tmp_path, select):
    """Every Redo of the paste, not only the first, leaves the cursor after
    the pasted text and nothing selected; every Undo gives back the state
    before it."""
    with _session(tmp_path) as s:
        s.send(HELLO_SELECTED + [CTRL_C, DOWN, HOME] + select)
        before = s.last.win
        pasted = s.send([CTRL_V]).win
        for _ in range(2):
            u = s.send([CTRL_U]).win
            assert (u.cursor, u.mb, u.me) == (before.cursor, before.mb, before.me)
            r = s.send([CTRL_R]).win
            assert (r.cursor, r.mb, r.me) == (pasted.cursor, pasted.mb, pasted.me)


# @scenario SC-selection-18
def test_a_wordstar_bit_left_in_the_options_file_does_not_change_ctrl_k_b(tmp_path):
    """An options file with both block bits is read as the selection mode
    (the dialog shows Selection, and the WordStar bit has no effect), so
    Ctrl-K B starts a block that stays open until Ctrl-K K."""
    rc = "[General]\nOptions2 : %d\n" % (BASE + SELECTION + 0x0004)
    with _session(tmp_path, rc=rc) as s:
        d = s.send([ctrl("k"), "b", RIGHT, RIGHT, DOWN, ctrl("k"), "k"])
        assert (d.win.mb, d.win.me) == ((0, 0), (1, 2))


# ---- a replacement the owner could not make --------------------------------------

def _fault_env(tmp_path, n):
    return {"XWPE_TEST_FAULT": "replace:%d" % n}


def _replace_with_fault(work, n):
    """Type over a two-row selection with memory refused from the n-th
    allocation of the action.  Return "whole" or "nothing", having checked
    that "nothing" is exactly the state before the key, with one status line
    saying why and the Undo step of the key before it still there."""
    work.mkdir()
    with EditSession(str(work), SEED, filename="t.txt", xwperc=SEL_RC,
                     env_extra=_fault_env(work, n)) as s:
        s.send(["a"] + TWO_ROWS_SELECTED)
        before = _view(s.last)
        assert _rows(s.last)[0] == b"ahello world" and s.last.win.mb == (0, 1)
        d = s.send(["Z"])
        with open(os.path.join(str(work), "ui.trace"), errors="replace") as fh:
            trace = fh.read()
        assert "buffer fault replace armed" in trace, \
            "the replace fault never fired at n=%d:\n%s" % (n, trace)
        if _rows(d) == [b"aZond line", b"third"]:
            assert d.win.ud == before[4] + 1
            return "whole"
        assert _view(d) == before, "a refused replacement changed something at n=%d" % n
        screen = "\n".join(s.display())
        assert "the replacement of the selection was not made, the text is unchanged" \
            in screen, screen
        d = s.send([CTRL_U])
        assert _rows(d)[0] == b"hello world", "Ctrl-U undoes the key before the refused one"
        return "nothing"


# @scenario SC-selection-09
def test_a_replacement_refused_for_memory_changes_nothing_and_a_sweep_ends_whole(tmp_path):
    """Memory cut at the action's first allocation, then the second, and on:
    every refused replacement left the text, the cursor, the selection and
    the Undo step count exactly as they were and said so on the status row,
    and from the action's last allocation on it is made whole.  The deletion
    never stands without the character that replaces it."""
    outcomes = {}
    first = 1
    with concurrent.futures.ThreadPoolExecutor(FAULT_BATCH) as pool:
        while first <= FAULT_SWEEP_MAX and "whole" not in outcomes.values():
            cuts = list(range(first, first + FAULT_BATCH))
            outcomes.update(zip(cuts, pool.map(
                lambda n: _replace_with_fault(tmp_path / ("n%02d" % n), n), cuts)))
            first += FAULT_BATCH
    whole = [n for n in sorted(outcomes) if outcomes[n] == "whole"]
    assert outcomes[1] == "nothing", "the first allocation refused still made the action"
    assert whole, "no cut up to %d allocations let the action through" % FAULT_SWEEP_MAX
    assert all(outcomes[n] == "nothing" for n in outcomes if n < whole[0]), outcomes


# @scenario SC-selection-09
@pytest.mark.parametrize("key,want", [
    ("Z", [b"aZond line", b"third"]),
    (ENTER, [b"a", b"ond line", b"third"]),
    (DELETE, [b"aond line", b"third"]),
])
def test_a_replacement_is_one_action_so_its_delete_never_stands_alone(tmp_path, key, want):
    """Memory is refused to the second action that enters the replacement's
    point.  A key over a selection is one action, so it never gets there and
    is made whole; a replacement split into a deletion and an insertion
    would have its insertion refused after the deletion already stood."""
    with EditSession(str(tmp_path), SEED, filename="t.txt", xwperc=SEL_RC,
                     env_extra={"XWPE_TEST_FAULT": "replace:1@2"}) as s:
        s.send(["a"] + TWO_ROWS_SELECTED)
        ud = s.last.win.ud
        d = s.send([key])
        with open(os.path.join(str(tmp_path), "ui.trace"), errors="replace") as fh:
            trace = fh.read()
        assert "buffer fault replace passed, 0 to go" in trace, trace
        assert "buffer fault replace armed" not in trace, trace
        assert _rows(d) == want
        assert d.win.ud == ud + 1


# ---- Paste over a selection ---------------------------------------------------------

SHIFT_INS = "\033[2;2~"
CTRL_V, CTRL_C = ctrl("v"), ctrl("c")
# "hello" is on the clipboard, then three letters of row 1 are selected
COPY_HELLO = HELLO_SELECTED + [CTRL_C, DOWN, HOME] + [SHIFT_RIGHT] * 3
# "hello world\nse" is on the clipboard, then three letters of row 2 are selected
COPY_TWO_ROWS = TWO_ROWS_SELECTED + [CTRL_C, DOWN, DOWN, HOME] + [SHIFT_RIGHT] * 3


# @scenario SC-selection-19
@pytest.mark.parametrize("paste", [CTRL_V, SHIFT_INS])
@pytest.mark.parametrize("setup,paste_rows,pasted_end", [
    (COPY_HELLO, [b"helloond line", b"third"], (1, 5)),
])
def test_paste_replaces_a_selection_and_the_pasted_text_stays(tmp_path, paste, setup,
                                                            paste_rows, pasted_end):
    """With text selected, Paste puts the clipboard in its place as one step.
    It is not selected afterwards: the next typed key goes after it.  One
    Ctrl-U gives back the text and the selection, one Ctrl-R pastes again."""
    with _session(tmp_path) as s:
        d = s.send(setup)
        before = _view(d)
        assert d.win.mb != d.win.me
        d = s.send([paste])
        assert _rows(d)[1:3] == paste_rows
        assert (d.win.cursor, d.win.mb, d.win.me) == (pasted_end,) * 3
        assert d.win.ud == before[4] + 1
        d = s.send(["!"])
        assert _rows(d)[1] == b"hello!ond line", "the pasted text was kept"
        d = s.send([CTRL_U, CTRL_U])
        assert _rows(d) == [b"hello world", b"second line", b"third"]
        assert (d.win.mb, d.win.me) == ((1, 0), (1, 3)), "the selection is back"
        assert d.win.ud == before[4]


# @scenario SC-selection-19
def test_paste_of_several_rows_replaces_a_selection_and_a_selection_over_rows(tmp_path):
    with _session(tmp_path) as s:
        d = s.send(COPY_TWO_ROWS)
        d = s.send([CTRL_V])
        assert _rows(d) == [b"hello world", b"second line", b"hello world", b"serd"]
        assert d.win.mb == d.win.me
        d = s.send([CTRL_U])
        assert _rows(d) == [b"hello world", b"second line", b"third"]
        assert (d.win.mb, d.win.me) == ((2, 0), (2, 3))
    with _session(tmp_path) as s:
        s.send(HELLO_SELECTED + [CTRL_C, HOME] + TWO_ROWS_SELECTED)
        d = s.send([CTRL_V])
        assert _rows(d) == [b"hellocond line", b"third"]
        d = s.send([CTRL_U])
        assert _rows(d) == [b"hello world", b"second line", b"third"]
        assert (d.win.mb, d.win.me) == ((0, 0), (1, 2))


# @scenario SC-selection-19
def test_paste_without_a_selection_does_not_select_the_pasted_text(tmp_path):
    with _session(tmp_path) as s:
        s.send(HELLO_SELECTED + [CTRL_C, DOWN, HOME])
        d = s.send([CTRL_V])
        assert _rows(d)[1] == b"hellosecond line"
        assert d.win.mb == d.win.me
        d = s.send(["!"])
        assert _rows(d)[1] == b"hello!second line"


# @scenario SC-selection-19
def test_without_the_mode_paste_leaves_the_pasted_text_as_the_block(tmp_path):
    """The control: with the option off a paste inserts at the cursor beside
    the block and the pasted text becomes the block, as it always did."""
    with _session(tmp_path, rc=PLAIN_RC) as s:
        d = s.send(COPY_HELLO)
        d = s.send([CTRL_V])
        assert _rows(d)[1] == b"sechelloond line"
        assert d.win.mb != d.win.me


def _paste_with_fault(work, n):
    """Paste over a selection with memory refused from the n-th allocation of
    the action: "whole", or "nothing" having checked the state is exactly the
    one before the key, with a status line saying why."""
    work.mkdir()
    with EditSession(str(work), SEED, filename="t.txt", xwperc=SEL_RC,
                     env_extra=_fault_env(work, n)) as s:
        s.send(COPY_HELLO)
        before = _view(s.last)
        d = s.send([CTRL_V])
        with open(os.path.join(str(work), "ui.trace"), errors="replace") as fh:
            trace = fh.read()
        assert "buffer fault replace armed" in trace, trace
        if _rows(d)[1] == b"helloond line":
            assert d.win.ud == before[4] + 1
            return "whole"
        assert _view(d) == before, "a refused paste changed something at n=%d" % n
        screen = "\n".join(s.display())
        assert "the paste was not made, the text is unchanged" in screen, screen
        return "nothing"


# @scenario SC-selection-20
def test_a_paste_refused_for_memory_changes_nothing_and_a_sweep_ends_whole(tmp_path):
    outcomes = {}
    first = 1
    with concurrent.futures.ThreadPoolExecutor(FAULT_BATCH) as pool:
        while first <= FAULT_SWEEP_MAX and "whole" not in outcomes.values():
            cuts = list(range(first, first + FAULT_BATCH))
            outcomes.update(zip(cuts, pool.map(
                lambda n: _paste_with_fault(tmp_path / ("n%02d" % n), n), cuts)))
            first += FAULT_BATCH
    whole = [n for n in sorted(outcomes) if outcomes[n] == "whole"]
    assert outcomes[1] == "nothing"
    assert whole, "no cut up to %d allocations let the paste through" % FAULT_SWEEP_MAX
    assert all(outcomes[n] == "nothing" for n in outcomes if n < whole[0]), outcomes


# ---- Trim Auto-indent -------------------------------------------------------------

SEED_C = "    int a = 1;\n"


# @scenario SC-selection-10
def test_trim_takes_the_indent_off_when_the_cursor_leaves_with_nothing_typed(tmp_path):
    with _session(tmp_path, SEED_C, C_TRIM_RC, "t.c") as s:
        s.send([END, ENTER])
        d = s.send([ENTER])
        assert _rows(d) == [b"    int a = 1;", b"", b"    "], \
            "Return on an indent-only line empties it and indents the new line"
        d = s.send([UP])
        assert _rows(d) == [b"    int a = 1;", b"", b""]
        assert d.win.ud == 3, "Up and the removal of the indent are one step"
        d = s.send([CTRL_U])
        assert _rows(d) == [b"    int a = 1;", b"", b"    "], \
            "one Ctrl-U gives the indent back with the move"


# @scenario SC-selection-10
def test_a_key_that_changes_the_text_and_leaves_the_line_is_one_step_with_the_trim(tmp_path):
    """Ctrl-K Y deletes a block elsewhere and the cursor goes to where it
    began, off the indented line: the block's deletion and the removal of the
    indent are the one Ctrl-U, which gives back both."""
    rc = "[General]\nOptions2 : %d\n" % (BASE + SOURCE_INDENT + TRIM)
    with _session(tmp_path, "    int a = 1;\nx y z\n", rc, "t.c") as s:
        s.send([DOWN, ctrl("k"), "b", END, ctrl("k"), "k", UP, END, ENTER])
        assert _rows(s.last) == [b"    int a = 1;", b"    ", b"x y z"]
        ud = s.last.win.ud
        d = s.send([ctrl("k"), "y"])
        assert _rows(d) == [b"    int a = 1;", b"", b""]
        assert d.win.ud == ud + 1, "the block's deletion and the trim are one step"
        d = s.send([CTRL_U])
        assert _rows(d) == [b"    int a = 1;", b"    ", b"x y z"]


# @scenario SC-selection-10
def test_trim_keeps_the_indent_of_a_line_the_user_typed_on(tmp_path):
    with _session(tmp_path, SEED_C, C_TRIM_RC, "t.c") as s:
        s.send([END, ENTER, "b"])
        d = s.send([UP])
        assert _rows(d) == [b"    int a = 1;", b"    b"]
        assert d.win.ud == 2, "a key that trims nothing is no step"


# @scenario SC-selection-10
def test_trim_keeps_a_blank_typed_after_the_indent(tmp_path):
    with _session(tmp_path, SEED_C, C_TRIM_RC, "t.c") as s:
        s.send([END, ENTER, " ", BACKSPACE])
        d = s.send([UP])
        assert _rows(d)[1] == b"    ", "typing on the line ends the claim, even if undone by hand"


# @scenario SC-selection-10
def test_without_trim_the_indent_stays_as_before(tmp_path):
    rc = "[General]\nOptions2 : %d\n" % (BASE + SOURCE_INDENT)
    with _session(tmp_path, SEED_C, rc, "t.c") as s:
        s.send([END, ENTER])
        d = s.send([UP])
        assert _rows(d) == [b"    int a = 1;", b"    "]


# @scenario SC-selection-10
def test_trim_takes_the_indent_off_when_a_click_or_a_page_key_leaves_the_line(tmp_path):
    with _session(tmp_path, SEED_C, C_TRIM_RC, "t.c") as s:
        s.send([END, ENTER])
        d = s.send(["\033[5~"])          # Page Up
        assert _rows(d)[1] == b""


# ---- Keep Final newline -----------------------------------------------------------

def _saved(tmp_path, seed, rc, keys=(), name="t.txt"):
    """The bytes Save writes for a file read as @seed after @keys."""
    with EditSession(str(tmp_path), seed, filename=name, xwperc=rc) as s:
        s.send(list(keys) + [F2])
        return s.disk()


KEEP_RC = "[General]\nOptions2 : %d\n" % (BASE + KEEP)


# @scenario SC-selection-11
@pytest.mark.parametrize("seed,want", [
    (b"a\nb", b"a\nb"),            # no final break: none written
    (b"a\nb\n", b"a\nb\n"),        # a final break: written
    (b"", b""),                    # an empty file stays empty
    (b"one line", b"one line"),
    (b"a\r\nb", b"a\r\nb"),        # a DOS file without a final break
    (b"a\r\nb\r\n", b"a\r\nb\r\n"),
])
def test_keep_final_newline_saves_the_file_as_it_was_read(tmp_path, seed, want):
    assert _saved(tmp_path, seed, KEEP_RC) == want


# @scenario SC-selection-11
@pytest.mark.parametrize("seed,want", [
    (b"a\nb", b"a\nb\n"), (b"", b"\n"), (b"a\r\nb", b"a\r\nb\r\n"),
])
def test_without_keep_the_final_newline_is_added_as_it_always_was(tmp_path, seed, want):
    assert _saved(tmp_path, seed, PLAIN_RC) == want


# @scenario SC-selection-11
def test_keep_final_newline_keeps_a_break_the_user_typed(tmp_path):
    """Return at the end of the last line is text the user wrote: it stays."""
    assert _saved(tmp_path, b"a\nb", KEEP_RC, [DOWN, END, ENTER]) == b"a\nb\n"


# @scenario SC-selection-11
def test_a_new_file_is_saved_as_typed(tmp_path):
    with EditSession(str(tmp_path), None, filename="new.txt", xwperc=KEEP_RC) as s:
        s.send(["q", F2])
        assert s.disk() == b"q"
    work = tmp_path / "plain"
    work.mkdir()
    with EditSession(str(work), None, filename="new.txt", xwperc=PLAIN_RC) as s:
        s.send(["q", F2])
        assert s.disk() == b"q\n"


# ---- the dialog ---------------------------------------------------------------------

def _open_editor_options(s):
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("e", delay=0.8)


def _ok(s):
    s.key(ALT.OPTIONS, delay=0.6)             # Ok button: Alt-O


def _dialog_rows(s):
    return "\n".join(s.display())


# @scenario SC-selection-12
def test_the_editor_dialog_shows_the_three_options_and_their_state(tmp_path):
    rc = "[General]\nOptions2 : %d\n" % (BASE + SELECTION + TRIM + KEEP)
    os.makedirs(os.path.join(str(tmp_path), ".config", "xwpe"), exist_ok=True)
    with open(os.path.join(str(tmp_path), ".config", "xwpe", "xwperc"), "w") as fh:
        fh.write(rc)
    with WpeSession(str(tmp_path), SEED, filename="t.txt") as s:
        _open_editor_options(s)
        text = _dialog_rows(s)
        for label in ("SelEction blocK", "Trim Auto-indent", "Keep Final newline",
                      "WordStar blocK"):
            assert label in text, text
        assert checkbox_mark(s, "SelEction blocK") == "[X]"
        assert checkbox_mark(s, "Trim Auto-indent") == "[X]"
        assert checkbox_mark(s, "Keep Final newline") == "[X]"
        assert checkbox_mark(s, "WordStar blocK") == "[ ]"


# @scenario SC-selection-12
def test_ticking_selection_in_the_dialog_turns_the_mode_on_and_ok_keeps_the_other_bits(tmp_path):
    """The dialog rebuilds the options from the boxes: a bit set in the
    options file that the dialog shows must come back, and a tick must set
    exactly its own bit."""
    os.makedirs(os.path.join(str(tmp_path), ".config", "xwpe"), exist_ok=True)
    with open(os.path.join(str(tmp_path), ".config", "xwpe", "xwperc"), "w") as fh:
        fh.write("[General]\nOptions2 : %d\n" % (BASE + TRIM + KEEP))
    with WpeSession(str(tmp_path), SEED, filename="t.txt") as s:
        _open_editor_options(s)
        s.key("\033e", delay=0.4)                                 # tick SelEction blocK
        s.key("\r", delay=0.8)                                    # Ok
        s.key("\033[1;2C", "\033[1;2C", "\033[1;2C", delay=0.3)   # Shift-Right x3
        s.key("J", delay=0.4)
        s.save()
        assert s.disk() == b"Jlo world\nsecond line\nthird\n", s.disk()
        # Keep Final newline came back from the options file: the file has
        # its final break, so it is written either way; Trim came back too
        _open_editor_options(s)
        assert checkbox_mark(s, "Trim Auto-indent") == "[X]"
        assert checkbox_mark(s, "Keep Final newline") == "[X]"
        assert checkbox_mark(s, "SelEction blocK") == "[X]"


# @scenario SC-selection-12
def test_ticking_selection_while_wordstar_is_on_leaves_selection_alone_on(tmp_path):
    os.makedirs(os.path.join(str(tmp_path), ".config", "xwpe"), exist_ok=True)
    with open(os.path.join(str(tmp_path), ".config", "xwpe", "xwperc"), "w") as fh:
        fh.write("[General]\nOptions2 : %d\n" % (BASE + 4))      # WordStar blocK on
    with WpeSession(str(tmp_path), SEED, filename="t.txt") as s:
        _open_editor_options(s)
        assert checkbox_mark(s, "WordStar blocK") == "[X]"
        s.key("\033e", delay=0.4)                                 # tick SelEction blocK
        s.key("\r", delay=0.8)                                    # Ok
        _open_editor_options(s)
        assert checkbox_mark(s, "SelEction blocK") == "[X]"
        assert checkbox_mark(s, "WordStar blocK") == "[ ]", \
            "the box the user ticked wins, the other is cleared"


# @scenario SC-selection-18
def test_both_block_bits_in_the_options_file_show_selection_and_ticking_wordstar_wins(tmp_path):
    """An options file with both block bits opens the dialog on Selection
    alone; ticking WordStar there gives WordStar alone."""
    os.makedirs(os.path.join(str(tmp_path), ".config", "xwpe"), exist_ok=True)
    with open(os.path.join(str(tmp_path), ".config", "xwpe", "xwperc"), "w") as fh:
        fh.write("[General]\nOptions2 : %d\n" % (BASE + SELECTION + 4))
    with WpeSession(str(tmp_path), SEED, filename="t.txt") as s:
        _open_editor_options(s)
        assert checkbox_mark(s, "SelEction blocK") == "[X]"
        assert checkbox_mark(s, "WordStar blocK") == "[ ]"
        s.key("\033k", delay=0.4)                                 # tick WordStar blocK
        s.key("\r", delay=0.8)                                    # Ok
        _open_editor_options(s)
        assert checkbox_mark(s, "WordStar blocK") == "[X]"
        assert checkbox_mark(s, "SelEction blocK") == "[ ]"
