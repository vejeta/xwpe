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


# ---- the option file: a number it gives is used, or refused and said --------

ALT_QUIT = "\033x"
WIDE = 240                 # a start-up line with three numbers is long
ONE_ROW_LINE = "A" * 1500
SENTINEL = "ENDMARK"
TAB_WORDS = "Tab : %s is not 1 to 99, 8 used (the file keeps %s)"
LINE_LEAD = "xwperc: "


def _user_rc(tmp_path, text):
    """Write the user's own option file under the session's HOME; return its path."""
    folder = os.path.join(str(tmp_path), ".config", "xwpe")
    os.makedirs(folder)
    path = os.path.join(folder, "xwperc")
    with open(path, "w") as fh:
        fh.write(text)
    return path


def _file_text(path):
    with open(path) as fh:
        return fh.read()


def _save_options(w):
    """Options > Save, answered."""
    w.key(ALT.OPTIONS)
    w.key("s", delay=0.8)
    w.key("\r", delay=1.0)


def _refused_tabstops_start(tmp_path, text):
    """Start with `Tab : <text>` in the user's xwperc; return the session's
    bottom row, the tab width, and the Tabstops the box shows."""
    _user_rc(tmp_path, "[General]\nTab : %s\n" % text)
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert w.alive(), "wpe died at start on Tab : %s (exit %s)" % (text, w.exit_code())
        row = _status_row(w)
        width = _tab_width(w)
        _open_options(w)
        shown = _field(w, "Tabstops:")
        w.key(ESC)
        return row, width, shown


# @scenario SC-number-fields-02
def test_an_option_file_tabstops_of_zero_is_not_used_and_is_said(tmp_path):
    """Tab : 0 in xwperc divided the editor by zero at start.  Now the default
    is used, the editor lives, and the bottom row says which number was not
    used and that the file keeps it."""
    row, width, shown = _refused_tabstops_start(tmp_path, "0")
    assert LINE_LEAD + TAB_WORDS % ("0", "0") in row, "bottom row: %r" % row
    assert width == 8 and shown == "8", "tab %s, box %s" % (width, shown)


# @scenario SC-number-fields-02a
def test_an_option_file_tabstops_of_zero_leaves_eight_in_the_box_and_on_a_tab(tmp_path):
    """The default is what the box shows and what a tab is drawn over; moving
    over the tab line with Down and Up (which divides by it too) lives."""
    _user_rc(tmp_path, "[General]\nTab : 0\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        assert _tab_width(w) == 8, "a tab is not 8 columns:\n" + _screen(w)
        w.key("\033[B", "\033[B", "\033[B", "\033[A")
        assert w.alive(), "wpe died on Down/Up over the tab line (exit %s)" % w.exit_code()
        _open_options(w)
        assert _field(w, "Tabstops:") == "8", "the box does not read 8:\n" + _screen(w)


# @scenario SC-number-fields-03
def test_an_option_file_negative_tabstops_is_not_used_and_is_said(tmp_path):
    row, width, shown = _refused_tabstops_start(tmp_path, "-3")
    assert LINE_LEAD + TAB_WORDS % ("-3", "-3") in row, "bottom row: %r" % row
    assert width == 8 and shown == "8", "tab %s, box %s" % (width, shown)


# @scenario SC-number-fields-04
def test_an_option_file_tabstops_past_an_int_is_not_used_and_is_said(tmp_path):
    row, width, shown = _refused_tabstops_start(tmp_path, "99999999999")
    assert LINE_LEAD + TAB_WORDS % ("99999999999", "99999999999") in row, \
        "bottom row: %r" % row
    assert width == 8 and shown == "8", "tab %s, box %s" % (width, shown)


# @scenario SC-number-fields-05
def test_every_refused_number_of_the_option_file_is_named_in_one_line(tmp_path):
    """MaxColumn 0, AutoIndent -1 and NumUndo 100 are three numbers the editor
    cannot use: the one line names each, and the box shows the defaults."""
    _user_rc(tmp_path, "[General]\nMaxColumn : 0\nNumUndo : 100\nAutoIndent : -1\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        row = _status_row(w)
        for words in ("MaxColumn : 0 is not 4 to 9999, 120 used (the file keeps 0)",
                      "NumUndo : 100 is not 0 to 99, 10 used (the file keeps 100)",
                      "AutoIndent : -1 is not 0 to 99, 3 used (the file keeps -1)"):
            assert words in row, "%r missing from %r" % (words, row)
        _open_options(w)
        assert _field(w, "Max. Columns:") == "120", "Max. Columns:\n" + _screen(w)
        assert _field(w, "Num. Undo:") == "10", "Num. Undo:\n" + _screen(w)
        assert _field(w, "Auto Ind. Col.:") == "3", "Auto Ind. Col.:\n" + _screen(w)


# @scenario SC-number-fields-05b
def test_a_refused_number_is_joined_to_the_line_about_keys_a_project_may_not_set(tmp_path):
    """A start-folder xwperc that tries a user-only key and gives Tab : 0 has
    one bottom row to say it: the keys first, then the number."""
    with open(os.path.join(str(tmp_path), "xwperc"), "w") as fh:
        fh.write("[General]\nTab : 0\n[Programming]\nAIBackend : 1\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        row = _status_row(w)
        keys = row.find("may not set")
        number = row.find(LINE_LEAD + TAB_WORDS % ("0", "0"))
        assert 0 <= keys < number, "bottom row: %r" % row


NARROW = 80                # the usual screen: a sentence per number does not fit
HEAD_OF_KEYS_LINE = "./xwperc may not set"


# @scenario SC-number-fields-05d
def test_every_refused_number_is_named_on_an_80_column_row(tmp_path):
    """Three refused numbers do not fit 80 columns as sentences, and the row
    keeps the end of a line too long for it: the compact form names every key
    with its value."""
    _user_rc(tmp_path, "[General]\nMaxColumn : 0\nNumUndo : 100\nAutoIndent : -1\n")
    with WpeSession(str(tmp_path), SEED, cols=NARROW) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        row = _status_row(w)
        for words in ("not used", "MaxColumn 0", "NumUndo 100", "AutoIndent -1"):
            assert words in row, "%r missing from %r" % (words, row)


# @scenario SC-number-fields-05e
def test_a_sixty_digit_option_file_number_still_shows_its_key_at_80_columns(tmp_path):
    """The key name is what tells the user which line of his file it was."""
    digits = "123456789012345678901234567890123456789012345678901234567890"
    _user_rc(tmp_path, "[General]\nTab : %s\n" % digits)
    with WpeSession(str(tmp_path), SEED, cols=NARROW) as w:
        row = _status_row(w)
        assert "Tab 12345678" in row and "not used" in row, "bottom row: %r" % row
        assert digits not in row, "the whole value is on the row: %r" % row


# @scenario SC-number-fields-05f
def test_ignored_keys_and_a_refused_number_at_80_columns_keep_the_head_whole(tmp_path):
    """The line about keys a project's file may not set keeps its head, and the
    refused number follows it, key named."""
    with open(os.path.join(str(tmp_path), "xwperc"), "w") as fh:
        fh.write("[General]\nTab : 0\n[Programming]\nAIBackend : 1\n")
    with WpeSession(str(tmp_path), SEED, cols=NARROW) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        row = _status_row(w)
        assert row.find(HEAD_OF_KEYS_LINE) >= 0, "the head is cut: %r" % row
        assert row.find("Tab 0") > row.find(HEAD_OF_KEYS_LINE), "bottom row: %r" % row


FIVE_KEYS = ("MaxColumn", "Tab", "MaxChanges", "NumUndo", "AutoIndent")
FIVE_SHORT = "MaxColumn : 0\nTab : 0\nMaxChanges : 1000\nNumUndo : 100\nAutoIndent : -1\n"
FORTY_DIGITS = "1234567890123456789012345678901234567890"
FIVE_LONG = "".join("%s : %s\n" % (k, FORTY_DIGITS) for k in FIVE_KEYS)
IGNORED_ONE = "[Programming]\nAIBackend : 1\n"
IGNORED_TWO = "[Programming]\nAIBackend : 1\nAIPolicy : ask\n"


def _bottom_row_at(tmp_path, folder_rc, user_rc, cols):
    """Start in a folder holding ./xwperc @folder_rc (when not None) and the
    user's own file @user_rc (when not None); return the bottom row."""
    if folder_rc is not None:
        with open(os.path.join(str(tmp_path), "xwperc"), "w") as fh:
            fh.write(folder_rc)
    if user_rc is not None:
        _user_rc(tmp_path, user_rc)
    with WpeSession(str(tmp_path), SEED, cols=cols) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        return _status_row(w).strip()


# @scenario SC-number-fields-05g
def test_one_ignored_key_and_three_refused_numbers_at_80_columns(tmp_path):
    """The long sentence of the keys line and three numbers do not fit 80
    columns: the keys line takes its short form and every number key is named
    whole."""
    rc = ("[General]\nMaxColumn : 0\nNumUndo : 100\nAutoIndent : -1\n" + IGNORED_ONE)
    row = _bottom_row_at(tmp_path, rc, None, NARROW)
    assert row == (HEAD_OF_KEYS_LINE.replace("set", "set:") + " AIBackend; "
                   "not used: MaxColumn, NumUndo, AutoIndent"), "row: %r" % row


# @scenario SC-number-fields-05h
def test_two_ignored_keys_and_one_refused_number_at_80_columns(tmp_path):
    """The short keys line, and the refused number whole behind it."""
    rc = "[General]\nTab : 0\n" + IGNORED_TWO
    row = _bottom_row_at(tmp_path, rc, None, NARROW)
    assert row == (HEAD_OF_KEYS_LINE.replace("set", "set:") + " AIBackend AIPolicy; "
                   "not used: Tab 0"), "row: %r" % row


# @scenario SC-number-fields-05m
def test_ignored_keys_alone_on_a_40_column_row_keep_their_head(tmp_path):
    """No refused number, only a key the project may not set: the long
    sentence is wider than the row, so the short form is said and the row does
    not begin in the middle of the sentence."""
    row = _bottom_row_at(tmp_path, "[General]\nTab : 3\n" + IGNORED_ONE, None, 40)
    assert row.startswith("./xwperc may not set: AIBackend"), "row: %r" % row


# @scenario SC-number-fields-05l
def test_many_ignored_keys_wider_than_the_row_keep_their_head(tmp_path):
    """The keys line alone is wider than the row: it is cut from the right, so
    its head is read, and the numbers part is left out."""
    rc = ("[General]\nTab : 0\n[Programming]\nAIBackend : 1\nAIEndpoint : http://x\n"
          "AIProvider : x\nAIProviderName : x\nAICAFile : /x\nAISearchURL : http://y\n"
          "AIEditHook : z\n")
    row = _bottom_row_at(tmp_path, rc, None, NARROW)
    assert row.startswith("./xwperc may not set: AIBackend AIEndpoint"), "row: %r" % row
    assert row.endswith("...") and "not used" not in row, "row: %r" % row


# @scenario SC-number-fields-05i
def test_five_refused_numbers_with_short_values_name_every_key_at_80_columns(tmp_path):
    row = _bottom_row_at(tmp_path, None, "[General]\n" + FIVE_SHORT, NARROW)
    for key in FIVE_KEYS:
        assert key in row, "%s missing from %r" % (key, row)
    assert "not used" in row, "row: %r" % row


# @scenario SC-number-fields-05j
def test_five_refused_numbers_with_forty_digit_values_name_every_key_at_80_columns(tmp_path):
    row = _bottom_row_at(tmp_path, None, "[General]\n" + FIVE_LONG, NARROW)
    for key in FIVE_KEYS:
        assert key in row, "%s missing from %r" % (key, row)
    assert FORTY_DIGITS not in row, "the whole value is on the row: %r" % row


# @scenario SC-number-fields-05k
def test_five_refused_numbers_at_40_columns_show_the_keys_from_the_left(tmp_path):
    row = _bottom_row_at(tmp_path, None, "[General]\n" + FIVE_SHORT, 40)
    assert row == "xwperc not used: MaxColumn, Tab +3", "row: %r" % row


# @scenario SC-number-fields-16
def test_an_option_file_with_dos_line_ends_gives_its_numbers(tmp_path):
    """A file saved with CR LF has a CR behind each number: Tab : 4 is 4, as it
    was, and nothing is said."""
    folder = os.path.join(str(tmp_path), ".config", "xwpe")
    os.makedirs(folder)
    with open(os.path.join(folder, "xwperc"), "wb") as fh:
        fh.write(b"[General]\r\nTab : 4\r\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert "xwperc:" not in _status_row(w), "bottom row: %r" % _status_row(w)
        assert _tab_width(w) == 4, "a tab is not 4 columns:\n" + _screen(w)


# @scenario SC-number-fields-NF4a
def test_the_key_style_upgrade_at_start_keeps_a_refused_number(tmp_path):
    """An xwperc of the CUA profile with no Modern bits, no KeysNotice and no
    XWPE_KEYS is written again at start; NumUndo : 500 is still what it says."""
    rc = _user_rc(tmp_path, "[General]\nOptions2 : 1\nNumUndo : 500\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE, env_extra={"XWPE_KEYS": None}) as w:
        assert w.alive(), "wpe died at start (exit %s)" % w.exit_code()
        w.key(ESC)
    text = _file_text(rc)
    assert "Version" in text, "the start did not write the file:\n" + text
    assert "NumUndo : 500\n" in text, "file:\n" + text


# @scenario SC-number-fields-05c
def test_a_good_option_file_number_says_nothing_and_is_used(tmp_path):
    """The control: Tab : 3 is used and the bottom row says nothing of it."""
    _user_rc(tmp_path, "[General]\nTab : 3\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert "xwperc:" not in _status_row(w), "bottom row: %r" % _status_row(w)
        assert _tab_width(w) == 3, "a tab is not 3 columns:\n" + _screen(w)


# @scenario SC-number-fields-11
def test_a_refused_number_survives_the_save_at_quit(tmp_path):
    """Tab : 0 with Autosave on: the editor lives, the save at quit still
    writes the file (it has a Version line afterwards), and the file reads
    Tab : 0 as the user wrote it, not the default."""
    rc = _user_rc(tmp_path, "[General]\nAutosave : 1\nTab : 0\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        assert w.alive(), "wpe died at start on Tab : 0 (exit %s)" % w.exit_code()
        w.key(ALT_QUIT, delay=1.0)
    text = _file_text(rc)
    assert "Version" in text, "the save at quit did not write the file:\n" + text
    assert "Tab : 0\n" in text and "Tab : 8" not in text, "file:\n" + text


# @scenario SC-number-fields-12
def test_options_save_keeps_a_refused_number_as_the_file_had_it(tmp_path):
    """The control of what works today: NumUndo : 500 is above the dialog's
    99, and Save writes it back unchanged."""
    rc = _user_rc(tmp_path, "[General]\nNumUndo : 500\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        _save_options(w)
        assert w.alive(), "wpe died (exit %s)" % w.exit_code()
    text = _file_text(rc)
    assert "Version" in text and "NumUndo : 500\n" in text, "file:\n" + text


# @scenario SC-number-fields-13
def test_a_number_the_user_stores_replaces_the_refused_one_on_save(tmp_path):
    """NumUndo 500 refused, then Num. Undo 20 stored in the dialog: Save writes
    20, the user's choice."""
    rc = _user_rc(tmp_path, "[General]\nNumUndo : 500\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        _settle(w, ALT_N, "20", ENTER)
        assert not _box_is_open(w), "the box stayed open:\n" + _screen(w)
        _save_options(w)
        assert w.alive(), "wpe died (exit %s)" % w.exit_code()
    text = _file_text(rc)
    assert "NumUndo : 20\n" in text and "NumUndo : 500" not in text, "file:\n" + text


# @scenario SC-number-fields-13b
def test_ok_on_the_box_for_another_option_keeps_the_refused_number(tmp_path):
    """A refused NumUndo is not the user's choice just because Ok was pressed
    on the box for Tabstops: Save still writes the file's own text."""
    rc = _user_rc(tmp_path, "[General]\nNumUndo : 500\n")
    with WpeSession(str(tmp_path), SEED, cols=WIDE) as w:
        _settle(w, ALT_T, "4", ENTER)
        _save_options(w)
        assert w.alive(), "wpe died (exit %s)" % w.exit_code()
    text = _file_text(rc)
    assert "NumUndo : 500\n" in text and "Tab : 4\n" in text, "file:\n" + text


def _row_of(rows, needle):
    """The index of the first screen row holding @needle."""
    return [i for i, r in enumerate(rows) if needle in r][0]


# @scenario SC-number-fields-14a
def test_an_option_file_max_columns_of_2000_keeps_a_long_line_in_one_row(tmp_path):
    """MaxColumn 2000 is a number the editor can use: a 1500 byte line is one
    row, so the next line is on the row below it and not thirteen rows down."""
    _user_rc(tmp_path, "[General]\nMaxColumn : 2000\n")
    seed = ONE_ROW_LINE + "\n" + SENTINEL + "\n"
    with WpeSession(str(tmp_path), seed, cols=WIDE) as w:
        rows = w.display()
        assert _row_of(rows, SENTINEL) == _row_of(rows, "AAAA") + 1, \
            "the line is not one row:\n" + _screen(w)


# @scenario SC-number-fields-14b
def test_an_option_file_max_columns_below_four_is_not_used_and_is_said(tmp_path):
    """The control of 14a from the other side: 3 is below the least, the
    default of 120 cuts the same line into rows."""
    _user_rc(tmp_path, "[General]\nMaxColumn : 3\n")
    seed = ONE_ROW_LINE + "\n" + SENTINEL + "\n"
    with WpeSession(str(tmp_path), seed, cols=WIDE) as w:
        assert "MaxColumn : 3 is not 4 to 9999, 120 used (the file keeps 3)" in _status_row(w)
        rows = w.display()
        assert _row_of(rows, SENTINEL) > _row_of(rows, "AAAA") + 1, \
            "the line is one row at the default width:\n" + _screen(w)
