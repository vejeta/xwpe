"""A number the user gives the editor has one owner (we_number.c), terminal layer.

Options > Editor holds five number fields and Search > Go to Line one more.  The
editor divides by Tabstops, sizes every row with Max. Columns and adds one to
Max. Changes, so a number outside what it can work with is a crash or lost
text, never a preference.  Each field knows its range: a number outside it is
refused where the user typed it, the range is said on the bottom row, and the
box stays open with the focus on the field until the number is a usable one or
the user cancels.  Go to Line is the exception that takes any run of digits: a
number past what an int holds counts as "the last line".

Every row drives the keys the user presses and reads the screen, and the tab
width from where a tab in the file is drawn, which is the number the editor
divides by.  A file with a tab is used so that Tabstops 0 is a death, not a
number nobody reads.
"""
import os

from wpe_driver import WpeSession, ALT

ESC = "\033"
ENTER = "\r"
TAB = "\t"
DELETE = "\033[3~"
ALT_M = "\033m"          # Max. Columns
ALT_T = "\033t"          # Tabstops
ALT_X = "\033x"          # MaX. Changes
ALT_N = "\033n"          # Num. Undo
CTRL_U = "\x15"          # Undo
BOX_TITLE = "Editor-Options"
SEED = "int main(void)\n{\n\treturn 0;\n}\n"
GOTO_SEED = "L1\nL2\nL3\nL4\nL5\n"
TAB_ROW = "return 0;"
TABSTOPS_WORDS = "Tabstops: 1 to 99"
MAXCOL_WORDS = "Max. Columns: 4 to 9999"
LINE_MAX = "2147483647"
SGR_PRESS = "\033[<0;%d;%dM"
SGR_RELEASE = "\033[<0;%d;%dm"


def _open_options(w):
    w.key(ALT.OPTIONS, "e")


def _box_is_open(w):
    return any(BOX_TITLE in row for row in w.display())


def _status_row(w):
    return w.display()[-1]


def _field(w, label):
    """What the box shows in the field after @label (a label with its colon)."""
    for row in w.display():
        if label in row:
            rest = row.split(label, 1)[1].split()
            return rest[0] if rest else ""
    return None


def _tab_width(w):
    """The columns a tab is drawn over, from the row the seed's tab is on.
    The box must be closed: it covers the row."""
    for row in w.display():
        if TAB_ROW in row:
            return row.index(TAB_ROW) - 1      # 1 is the window's frame
    return None


def _button_cell(w, label):
    """The 0-based (col, row) of the first cell of a button's label."""
    for y, row in enumerate(w.display()):
        if BOX_TITLE in row:
            continue
        if label in row and y > 15:
            return row.index(label), y
    raise AssertionError("no %r button on screen:\n%s" % (label, "\n".join(w.display())))


def _click(w, col, row):
    w.key(SGR_PRESS % (col + 1, row + 1), delay=0.2)
    w.key(SGR_RELEASE % (col + 1, row + 1), delay=0.5)


def _settle(w, *keys):
    """Open Options > Editor, then send @keys."""
    _open_options(w)
    w.key(*keys)


def _typed_tabstops(w, digits):
    """Options > Editor, Alt-T and @digits typed, the box left as it is."""
    _settle(w, ALT_T, *digits)


def _screen(w):
    return "\n".join(w.display())


# @scenario SC-number-fields-01
def test_tabstops_zero_is_refused_and_the_box_stays_open(tmp_path):
    """Tabstops 0 divides the editor by zero: Enter on it is refused with the
    range on the bottom row, the box stays open, and after Esc a tab is still
    drawn over eight columns."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        w.key(ENTER)
        assert w.alive(), "wpe died on Tabstops 0 (exit %s)" % w.exit_code()
        assert _box_is_open(w), "the box closed on Tabstops 0:\n" + _screen(w)
        assert TABSTOPS_WORDS in _status_row(w), \
            "the range is not on the bottom row: %r" % _status_row(w)
        w.key(ESC)
        assert w.alive(), "wpe died after Esc"
        assert not _box_is_open(w), "Esc did not close the box"
        assert _tab_width(w) == 8, "a tab is not 8 columns:\n" + _screen(w)


# @scenario SC-number-fields-01b
def test_tabstops_emptied_with_delete_is_refused(tmp_path):
    """Delete on the last digit writes a 0 into the field: that is a second way
    to type the number the divisions cannot take, and it is refused too."""
    with WpeSession(str(tmp_path), SEED) as w:
        _settle(w, ALT_T, DELETE)
        w.key(ENTER)
        assert w.alive(), "wpe died on Tabstops emptied with Delete (exit %s)" % w.exit_code()
        assert _box_is_open(w), "the box closed:\n" + _screen(w)
        assert TABSTOPS_WORDS in _status_row(w), \
            "the range is not on the bottom row: %r" % _status_row(w)


# @scenario SC-number-fields-01c
def test_a_refused_tabstops_keeps_the_focus_for_the_next_digits(tmp_path):
    """After the refusal the field still has the focus: the digit typed next
    goes into Tabstops, and Enter closes the box with that number."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        w.key(TAB)
        assert _box_is_open(w), "Tab left a refused field:\n" + _screen(w)
        assert TABSTOPS_WORDS in _status_row(w), \
            "the range is not on the bottom row: %r" % _status_row(w)
        w.key("6", ENTER)
        assert w.alive(), "wpe died"
        assert not _box_is_open(w), "the box stayed open on 6:\n" + _screen(w)
        assert _tab_width(w) == 6, "a tab is not 6 columns:\n" + _screen(w)


# @scenario SC-number-fields-06
def test_max_columns_zero_is_refused_and_the_text_typed_afterwards_is_seen(tmp_path):
    """Max. Columns 0 left no room in a row, so what was typed in a new file
    was nowhere on the screen: it is refused with its range, and after Esc a
    new file shows what is typed."""
    with WpeSession(str(tmp_path), SEED) as w:
        _settle(w, ALT_M, "0", ENTER)
        assert w.alive(), "wpe died"
        assert _box_is_open(w), "the box closed on Max. Columns 0:\n" + _screen(w)
        assert MAXCOL_WORDS in _status_row(w), \
            "the range is not on the bottom row: %r" % _status_row(w)
        w.key(ESC)
        w.key(ALT.FILE, "n")
        w.key(*"abc")
        assert any("abc" in row for row in w.display()), \
            "what was typed in the new file is not on the screen:\n" + _screen(w)


# @scenario SC-number-fields-06b
def test_max_columns_takes_four_places_up_to_9999(tmp_path):
    """The field has room for 9999 and keeps it: reopened, the box shows it,
    and a new file takes what is typed."""
    with WpeSession(str(tmp_path), SEED) as w:
        _settle(w, ALT_M, "9", "9", "9", "9", ENTER)
        assert w.alive(), "wpe died"
        assert not _box_is_open(w), "the box stayed open on 9999:\n" + _screen(w)
        _open_options(w)
        assert _field(w, "Max. Columns:") == "9999", \
            "the field does not read 9999:\n" + _screen(w)
        w.key(ESC)
        w.key(ALT.FILE, "n")
        w.key(*"abc")
        assert any("abc" in row for row in w.display()), \
            "what was typed in the new file is not on the screen:\n" + _screen(w)


# @scenario SC-number-fields-06c
def test_max_columns_three_is_refused_one_below_the_least(tmp_path):
    """A row must hold one character of any width: 3 is refused, 4 is taken."""
    with WpeSession(str(tmp_path), SEED) as w:
        _settle(w, ALT_M, "3", ENTER)
        assert _box_is_open(w), "the box closed on 3:\n" + _screen(w)
        assert MAXCOL_WORDS in _status_row(w), \
            "the range is not on the bottom row: %r" % _status_row(w)
        w.key("4", ENTER)
        assert not _box_is_open(w), "the box stayed open on 4:\n" + _screen(w)
        assert w.alive(), "wpe died"


# @scenario SC-number-fields-07
def test_a_dropped_key_does_not_make_the_next_digit_append(tmp_path):
    """A '-' typed into a number field changes nothing, and the digit after it
    still replaces the number: the field reads 5, not 85."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "-5")
        w.key(TAB)
        assert _field(w, "Tabstops:") == "5", \
            "the field does not read 5:\n" + _screen(w)


# @scenario SC-number-fields-09
def test_a_resize_does_not_store_a_refused_number(tmp_path):
    """Tabstops 0 typed, the terminal resized: the box is drawn again from the
    last number it could use, 8, and Enter closes it with that."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        w.resize(100, 32)
        assert w.alive(), "wpe died on the resize (exit %s)" % w.exit_code()
        assert _field(w, "Tabstops:") == "8", \
            "the field does not read 8 after the resize:\n" + _screen(w)
        w.key(ENTER)
        assert w.alive(), "wpe died on Enter (exit %s)" % w.exit_code()
        _open_options(w)
        assert _field(w, "Tabstops:") == "8", \
            "reopened, the field does not read 8:\n" + _screen(w)


# @scenario SC-number-fields-09b
def test_a_resize_keeps_a_valid_number_typed_before_it(tmp_path):
    """The control of the row above: a number the owner accepts survives the
    resize, as it does today."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "5")
        w.resize(100, 32)
        assert _field(w, "Tabstops:") == "5", \
            "the field does not read 5 after the resize:\n" + _screen(w)
        w.key(ENTER)
        assert w.alive(), "wpe died on Enter (exit %s)" % w.exit_code()
        _open_options(w)
        assert _field(w, "Tabstops:") == "5", \
            "reopened, the field does not read 5:\n" + _screen(w)


# @scenario SC-number-fields-10
def test_a_click_on_ok_does_not_store_a_refused_number(tmp_path):
    """Tabstops 0 typed and Ok clicked: a click leaves the field like a key,
    so it is refused the same way, and the box stays open."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        col, row = _button_cell(w, "Ok")
        _click(w, col, row)
        assert w.alive(), "wpe died on the click (exit %s)" % w.exit_code()
        assert _box_is_open(w), "the box closed on the click:\n" + _screen(w)
        assert TABSTOPS_WORDS in _status_row(w), \
            "the range is not on the bottom row: %r" % _status_row(w)


# @scenario SC-number-fields-10b
def test_a_click_on_cancel_closes_the_box_and_keeps_the_number(tmp_path):
    """The control of the row above, and of the loop that holds a leave: a
    click on Cancel is Esc, so the box closes and nothing is taken."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        col, row = _button_cell(w, "Cancel")
        _click(w, col, row)
        assert w.alive(), "wpe died on the click (exit %s)" % w.exit_code()
        assert not _box_is_open(w), "the box stayed open on Cancel:\n" + _screen(w)
        assert _tab_width(w) == 8, "a tab is not 8 columns:\n" + _screen(w)


# @scenario SC-number-fields-10c
def test_a_box_dragged_while_a_number_is_held_keeps_its_field_inside(tmp_path):
    """Tabstops 0 typed, the box dragged by its title: the field is held, and
    the digits typed next go into the field where the box now is, not over
    what the box shows at the field's old place."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        rows = w.display()
        ty = [i for i, r in enumerate(rows) if BOX_TITLE in r][0]
        tx = rows[ty].index(BOX_TITLE)
        w.key(SGR_PRESS % (tx + 1, ty + 1), delay=0.2)
        w.key("\033[<32;%d;%dM" % (tx + 1 - 8, ty + 1 + 3), delay=0.2)
        w.key(SGR_RELEASE % (tx + 1 - 8, ty + 1 + 3), delay=0.6)
        assert w.alive(), "wpe died on the drag (exit %s)" % w.exit_code()
        assert _box_is_open(w), "the box closed on the drag:\n" + _screen(w)
        w.key("7")
        assert _field(w, "Tabstops:") == "7", \
            "the digit did not go into the moved field:\n" + _screen(w)
        assert any("Keys:" in row for row in w.display()), \
            "the field was drawn over the box's Keys label:\n" + _screen(w)
        w.key(ENTER)
        assert not _box_is_open(w), "the box stayed open on 7:\n" + _screen(w)
        assert _tab_width(w) == 7, "a tab is not 7 columns:\n" + _screen(w)


# @scenario SC-number-fields-11
def test_esc_on_a_typed_number_takes_nothing(tmp_path):
    """The control: Tabstops 0 typed and Esc pressed closes the box and the
    tab stays eight columns."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "0")
        w.key(ESC)
        assert w.alive(), "wpe died on Esc"
        assert not _box_is_open(w), "Esc did not close the box"
        assert _tab_width(w) == 8, "a tab is not 8 columns:\n" + _screen(w)


# @scenario SC-number-fields-C2
def test_tabstops_four_is_taken(tmp_path):
    """The control: a number in range is taken and a tab is drawn over it."""
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "4")
        w.key(ENTER)
        assert not _box_is_open(w), "the box stayed open:\n" + _screen(w)
        assert _tab_width(w) == 4, "a tab is not 4 columns:\n" + _screen(w)


# @scenario SC-number-fields-C3
def test_zero_changes_and_zero_undo_are_taken(tmp_path):
    """The control: 0 is a usable number for Max. Changes (autosave after every
    change) and for Num. Undo (one step kept), and both are taken."""
    with WpeSession(str(tmp_path), "\n") as w:
        _settle(w, ALT_X, "0", ALT_N, "0", ENTER)
        assert w.alive(), "wpe died"
        assert not _box_is_open(w), "the box stayed open:\n" + _screen(w)
        w.key(*"ab")
        w.key(CTRL_U)
        assert w.alive(), "wpe died on Ctrl-U"
        w.save()
        assert w.text() in ("a\n", "\n"), "Ctrl-U gave %r" % w.text()


# @scenario SC-number-fields-C5
def test_an_option_file_number_past_its_range_does_not_make_ok_an_error(tmp_path):
    """Until the file reader checks the range, Options > Editor can open on a
    number the owner would not take (NumUndo 500 of an xwperc).  Ok on the
    box for another option must not raise an error for it: a value equal to
    the one in force is left as it is."""
    cfg = os.path.join(str(tmp_path), ".config", "xwpe")
    os.makedirs(cfg)
    rc = os.path.join(cfg, "xwperc")
    text = "[General]\nNumUndo : 500\n"
    with open(rc, "w") as fh:
        fh.write(text)
    with WpeSession(str(tmp_path), SEED) as w:
        _typed_tabstops(w, "4")
        w.key(ENTER)
        assert w.alive(), "wpe died"
        assert not _box_is_open(w), \
            "the box did not close (an error box?):\n" + _screen(w)
        assert _tab_width(w) == 4, "a tab is not 4 columns:\n" + _screen(w)
    with open(rc) as fh:
        assert fh.read() == text, "Ok on the box rewrote the option file"


# @scenario SC-number-fields-14
def test_an_option_file_max_columns_of_2000_is_shown_whole(tmp_path):
    """MaxColumn 2000 of an xwperc is a number the editor can use: the box
    shows 2000, with four places for it, and not 999."""
    cfg = os.path.join(str(tmp_path), ".config", "xwpe")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "xwperc"), "w") as fh:
        fh.write("[General]\nMaxColumn : 2000\n")
    with WpeSession(str(tmp_path), SEED) as w:
        _open_options(w)
        assert _field(w, "Max. Columns:") == "2000", \
            "the field does not read 2000:\n" + _screen(w)


def _goto_typed_then_leave(w, digits):
    w.menu(ALT.SEARCH, "g")
    w.key(*digits)
    w.key(TAB)


def _marker_lines(w):
    """Type a marker at the cursor, save, and return the 1-based lines it is on."""
    w.key("X")
    w.save()
    text = w.text()
    assert [l.replace("X", "") for l in text.splitlines()] == \
        ["L1", "L2", "L3", "L4", "L5"], "file damaged: %r" % text
    return [i + 1 for i, l in enumerate(text.splitlines()) if "X" in l]


# @scenario SC-number-fields-08b
def test_goto_line_past_the_int_range_shows_the_largest_number(tmp_path):
    """Twelve digits typed into the ten place field, the focus moved on: the
    field reads 2147483647, the largest number a line can have."""
    with WpeSession(str(tmp_path), GOTO_SEED) as w:
        _goto_typed_then_leave(w, "123456789012")
        row = [r for r in w.display() if "Goto Line Number" in r and "q" not in r]
        assert row and LINE_MAX in row[0], \
            "the field does not read %s:\n%s" % (LINE_MAX, _screen(w))
        w.key(ENTER)
        assert w.alive(), "wpe died"
        assert _marker_lines(w) == [5], "Go to Line did not reach the last line"


# @scenario SC-number-fields-08a
def test_goto_line_2147483648_goes_to_the_last_line(tmp_path):
    """One past what an int holds: the number saturates and the cursor goes to
    the last line.  The row for the sanitizer build: negating the number
    to count its places is undefined for the smallest int."""
    with WpeSession(str(tmp_path), GOTO_SEED) as w:
        _goto_typed_then_leave(w, "2147483648")
        w.key(ENTER)
        assert w.alive(), "wpe died (exit %s)" % w.exit_code()
        assert _marker_lines(w) == [5], "Go to Line 2147483648 missed the last line"


# @scenario SC-number-fields-C1
def test_goto_line_int_max_goes_to_the_last_line(tmp_path):
    """The control: the largest number an int holds goes to the last line."""
    with WpeSession(str(tmp_path), GOTO_SEED) as w:
        _goto_typed_then_leave(w, LINE_MAX)
        w.key(ENTER)
        assert w.alive(), "wpe died"
        assert _marker_lines(w) == [5], "Go to Line %s missed the last line" % LINE_MAX
