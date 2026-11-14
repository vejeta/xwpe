"""Boxes have one layout path (terminal rows; the owner is we_layout.c).

What the user sees when the screen changes size while something holds the
screen: the editor comes back whole.  A command that takes the screen
(Ctrl-F9 on a shell script, which waits for Return) saves what was behind
it; if the terminal is resized meanwhile, that saved picture describes a
screen that is gone, and putting it back drew the old status row in the
middle of the new screen (grown) or an old text row on the last row
(shrunk).  The owner stamps every saved view with the screen's size and
puts back only a current one.

The dialogs, the error box, the About and System Info boxes and the box of
a long file operation are the owner's too (second half of the rows): each
is placed by a rule for the screen as it is now, so a resize centres it
again, the About and System Info boxes are centred and never reach the
status row, a dialog the user moved keeps its place until the next resize,
and a question that is longer than the screen still shows its buttons.

Rows: tests/x11/test_box_layout.py and tests/wayland/test_box_layout.py
are the graphical twins; the placement rules are in test_layout (make
check).
"""
import os
import re

import pytest

from wpe_driver import WpeSession, ALT
from test_path_cuts import _folder_of_length
from test_opt_dialog_move import _ai_build, ENV as AI_ENV

RUN_CTRL_F9 = "\x1b[20;5~"
STRIKE = "Strike <Return> to continue"
GROWN = (100, 40)
SHRUNK = (60, 20)

ESC = "\033"
ENTER = "\r"
ALT_F2 = "\033\033OQ"
ALT_V = "\033v"
LEFT, RIGHT = "\033[D", "\033[C"
SEED = "int main(void){return 0;}\n"
STATUS_START = "F1 Help"
# pyte shows the DEC line-drawing set as its ASCII code points.
CORNER_TL, CORNER_TR, CORNER_BL = "l", "k", "m"
ABOUT_W, ABOUT_H = 55, 18
ABOUT_TEXT = "Created by Fred Kruse"
ABOUT_TEXT_ROW, ABOUT_TEXT_COL = 5, 2     # where the line sits from the frame's corner
ERROR_TEXT = "Can't open directory"
ERROR_MIN_TEXT = 20
OUTSIDE = "layout frame outside the path"
FIRST_ROW = 1                              # under the menu bar


def _screen_after_strike_return(tmp_path, start, resized):
    """The display after Ctrl-F9 on a script, a resize to `resized` while the
    wait is up (None for none), and Return.  The session starts at `start`."""
    with WpeSession(str(tmp_path), "echo HITMARK\n", filename="h.sh",
                    cols=start[0], rows=start[1]) as w:
        w.key(RUN_CTRL_F9, delay=1.5)
        assert STRIKE in "\n".join(w.display()), "the wait did not appear"
        if resized is not None:
            w.resize(resized[0], resized[1])
        w.key("\r", delay=1.0)
        return list(w.display())


def _fresh(tmp_path, size):
    return _screen_after_strike_return(tmp_path / "fresh", size, None)


# @scenario SC-box-06
@pytest.mark.parametrize("name,size", [("grow", GROWN), ("shrink", SHRUNK)])
def test_strike_return_ends_with_the_status_line_on_the_last_row(
        tmp_path, name, size):
    """Resizing during the wait must not bring the old screen back: the last
    row, the status line, is what a session started at that size shows, and
    no row of the old size's status line is left in the middle."""
    (tmp_path / "fresh").mkdir()
    fresh = _fresh(tmp_path, size)
    after = _screen_after_strike_return(tmp_path, (80, 30), size)
    assert after[-1].rstrip() == fresh[-1].rstrip(), \
        "the status row after %s is %r, a fresh session at that size shows %r" % (
            name, after[-1].rstrip(), fresh[-1].rstrip())
    status = fresh[-1].strip()
    stray = [i for i, row in enumerate(after[:-1]) if status and status in row]
    assert not stray, \
        "the old status line was put back on row(s) %s:\n%s" % (
            stray, "\n".join(after))


# ---- the About and System Info boxes ------------------------------------

def _screen(rows):
    return "\n".join(rows)


def _about_corner(rows):
    """(top, left) of the About frame, from its line of credits."""
    for i, row in enumerate(rows):
        col = row.find(ABOUT_TEXT)
        if col >= 0:
            return i - ABOUT_TEXT_ROW, col - ABOUT_TEXT_COL
    raise AssertionError("no About box on the screen:\n" + _screen(rows))


def _titled_frame(rows, title):
    """(top, bottom, left, right) of the frame whose top border carries
    `title`: the corners either side of it and the bottom-left corner below."""
    top = next((i for i, r in enumerate(rows) if title in r), None)
    assert top is not None, "no %r box on the screen:\n%s" % (title, _screen(rows))
    at = rows[top].index(title)
    left = rows[top].rfind(CORNER_TL, 0, at)
    right = rows[top].find(CORNER_TR, at)
    bottom = next((i for i in range(top + 1, len(rows)) if rows[i][left] == CORNER_BL), -1)
    assert left >= 0 and right > left and bottom > top, \
        "the frame of %r is not whole (%d,%d,%d,%d):\n%s" % (
            title, top, bottom, left, right, _screen(rows))
    return top, bottom, left, right


# @scenario SC-box-04
@pytest.mark.parametrize("size", [(100, 40), (80, 21), (60, 20)])
def test_the_about_box_is_centred_and_stays_above_the_status_row(tmp_path, size):
    """About is centred between the menu bar and the status row, also after
    the terminal changed size.  It used to be drawn at column 8, row 3 for
    ever, which put its lower rows over the status line below 22 rows."""
    cols, rows = size
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30) as w:
        w.menu(ALT.SYSTEM, "a", delay=0.8)
        w.resize(cols, rows, delay=1.0)
        shown = w.display()
        w.key(ESC, delay=0.8)
        closed = w.display()
    top, left = _about_corner(shown)
    assert left == (cols - ABOUT_W) // 2, \
        "About starts at column %d on %d columns:\n%s" % (left, cols, _screen(shown))
    assert top == FIRST_ROW + (rows - 2 - ABOUT_H) // 2, \
        "About starts at row %d on %d rows:\n%s" % (top, rows, _screen(shown))
    assert top + ABOUT_H - 1 <= rows - 2, "About reaches the status row:\n" + _screen(shown)
    assert shown[-1].rstrip() == closed[-1].rstrip(), \
        "the status row changed under About: %r, then %r" % (
            shown[-1].rstrip(), closed[-1].rstrip())


# @scenario SC-box-04
@pytest.mark.parametrize("rows", [21, 20])
def test_the_about_box_leaves_the_status_row_alone(tmp_path, rows):
    """On 21 and 20 rows the About box is 18 tall and fits between the menu
    bar and the status row.  Its fixed place, row 3, put its last rows on the
    status line (the last row read "F1 Helpmqqq...")."""
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30) as w:
        w.menu(ALT.SYSTEM, "a", delay=0.8)
        w.resize(80, rows, delay=1.0)
        shown = w.display()
        w.key(ESC, delay=0.8)
        closed = w.display()
    assert shown[-1].rstrip() == closed[-1].rstrip(), \
        "About wrote on the status row: %r, the status row is %r" % (
            shown[-1].rstrip(), closed[-1].rstrip())
    assert closed[-1].lstrip().startswith(STATUS_START), closed[-1]


# @scenario SC-box-04
def test_the_about_box_on_a_screen_too_small_is_cut_at_the_top_left(tmp_path):
    """50 by 14 cannot hold the 55 by 18 About box: it is drawn cut at the
    top left, where its first lines are, leaves the status row alone, and
    any key takes it away."""
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30) as w:
        w.menu(ALT.SYSTEM, "a", delay=0.8)
        w.resize(50, 14, delay=1.0)
        shown = w.display()
        w.key("x", delay=0.8)
        closed = w.display()
    top, left = _about_corner(shown)
    assert (top, left) == (FIRST_ROW, 0), \
        "the cut About box starts at row %d, column %d:\n%s" % (top, left, _screen(shown))
    assert shown[-1].rstrip() == closed[-1].rstrip(), \
        "the status row changed under About:\n" + _screen(shown)
    assert not any(ABOUT_TEXT in r for r in closed), \
        "a key should close About:\n" + _screen(closed)


# @scenario SC-box-04
@pytest.mark.parametrize("size", [(100, 40), (80, 40), (80, 24)])
def test_system_info_is_centred_after_a_resize(tmp_path, size):
    """System Info keeps the same margin to the left and the right and sits
    in the middle between the menu bar and the status row, whatever size the
    terminal has been resized to.  It used to start at row 3 for ever."""
    cols, rows = size
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30) as w:
        w.menu(ALT.SYSTEM, "s", delay=0.8)
        w.resize(cols, rows, delay=1.0)
        shown = w.display()
        w.key(ESC, delay=0.6)
    top, bottom, left, right = _titled_frame(shown, "Information")
    height = bottom - top + 1
    assert left == cols - 1 - right, \
        "System Info margins differ: left %d, right %d on %d columns:\n%s" % (
            left, cols - 1 - right, cols, _screen(shown))
    assert top == FIRST_ROW + (rows - 2 - height) // 2, \
        "System Info starts at row %d on %d rows, %d rows tall:\n%s" % (
            top, rows, height, _screen(shown))


# @scenario SC-box-04
def test_system_info_on_a_screen_too_small_is_cut_at_the_top_left(tmp_path):
    """40 by 8 holds neither the width nor the rows System Info asks for: its
    frame is drawn cut at the top left (column 0, under the menu bar), the
    status row is left alone and any key takes the box away."""
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30) as w:
        w.menu(ALT.SYSTEM, "s", delay=0.8)
        w.resize(40, 8, delay=1.0)
        shown = w.display()
        w.key("x", delay=0.8)
        closed = w.display()
    row = next((i for i, r in enumerate(shown) if "Information" in r), None)
    assert row == FIRST_ROW, "the title is on row %r:\n%s" % (row, _screen(shown))
    assert shown[row].startswith(CORNER_TL), "the frame is not at column 0:\n" + _screen(shown)
    assert shown[-1].rstrip() == closed[-1].rstrip(), \
        "the status row changed under System Info:\n" + _screen(shown)
    assert not any("Information" in r for r in closed), \
        "a key should close System Info:\n" + _screen(closed)


# ---- the error box --------------------------------------------------------

def _open_the_error_box(w):
    """File > Save As to a folder that does not exist: "Can't open directory"."""
    w.key(ALT.FILE, delay=0.5)
    w.key("a", delay=0.8)
    w.key("\x15", delay=0.2)
    w.key("/proc/nope/x.c", delay=0.3)
    w.key(ENTER, delay=1.0)


def _error_corner(rows):
    """(top, left) of the error box, from its text: two rows down, two in."""
    for i, row in enumerate(rows):
        col = row.find(ERROR_TEXT)
        if col >= 0:
            return i - 2, col - 2
    raise AssertionError("no error box on the screen:\n" + _screen(rows))


# @scenario SC-box-04
@pytest.mark.parametrize("size", [(80, 30), (100, 40), (60, 20), (80, 25)])
def test_the_error_box_keeps_its_corner_and_follows_a_resize(tmp_path, size):
    """A control: the error box is where it was drawn since 1.5.30a, a text
    wide box centred as a box one column and one row smaller, and a resize
    centres it again at the same corner."""
    cols, rows = size
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30) as w:
        _open_the_error_box(w)
        if size != (80, 30):
            w.resize(cols, rows, delay=1.0)
        shown = w.display()
        w.key(ENTER, delay=0.8)
        closed = w.display()
    top, left = _error_corner(shown)
    assert top == (rows - 6) // 2, \
        "the error box starts at row %d on %d rows:\n%s" % (top, rows, _screen(shown))
    assert left == (cols - ERROR_MIN_TEXT) // 2 - 2, \
        "the error box starts at column %d on %d columns:\n%s" % (left, cols, _screen(shown))
    assert not any(ERROR_TEXT in r for r in closed), \
        "Return should close the error box:\n" + _screen(closed)


# ---- the box of a long file operation -------------------------------------

F3 = "\033[13~"
ALT_M = "\033m"
ALT_T = "\033t"
ALT_R = "\033r"
DOWN = "\033[B"
CTRL_C_HEADER = "Remove"
CTRL_C_NAMES = ("victim/a.txt", "victim/b.txt")
CTRL_C_MIN_TEXT = 14           # the header's width plus eight: what the box is at least wide for
CTRL_C_LINES = 1


def _start_a_removal(w):
    """Options > File-Manager > Remove: No Prompt, then the file manager's
    tree on the folder `victim` and Remove: the box of the operation says
    which file it is on and waits for a key, once per file."""
    w.key(ALT.OPTIONS, delay=0.6)
    w.key("f", delay=0.8)
    w.key(ALT_M, delay=0.6)
    w.key(ENTER, delay=0.8)
    w.key(F3, delay=1.2)
    w.key(ALT_T, delay=0.6)
    w.key(DOWN, delay=0.4)
    w.key(ALT_R, delay=1.0)


def _ctrl_c_corner(rows, name):
    """(top, left) of the Ctrl-C box that shows `name`: the frame's top row
    carries its header, and the text starts three columns in, two rows down."""
    for i, row in enumerate(rows):
        col = row.find(name)
        if col >= 0:
            return i - 2, col - 3
    raise AssertionError("no box names %r:\n%s" % (name, _screen(rows)))


# @scenario SC-box-04
@pytest.mark.parametrize("size", [(80, 30), (100, 40), (60, 20)])
def test_the_box_of_a_file_operation_keeps_its_corner_across_a_resize(tmp_path, size):
    """A control: removing a folder file by file shows a box that names the
    file and waits for a key.  On the terminal a resize ends that wait (the
    operation goes on to the next file), and the box of the next file is
    where it was centred since 1.5.30a, for the new screen.  The files are
    removed and no box is left behind."""
    cols, rows = size
    work = tmp_path / "work"
    (work / "victim").mkdir(parents=True)
    for name in CTRL_C_NAMES:
        (work / name).write_text("x\n")
    with WpeSession(str(work), SEED, filename="t.c", cols=80, rows=30) as w:
        _start_a_removal(w)
        if size != (80, 30):
            w.resize(cols, rows, delay=1.0)
        name = CTRL_C_NAMES[0] if size == (80, 30) else CTRL_C_NAMES[1]
        shown = w.display()
        w.key("x", delay=1.0)
        w.key("x", delay=1.0)
        final = w.display()
    top, left = _ctrl_c_corner(shown, name)
    assert top == (rows - CTRL_C_LINES - 6) // 2, \
        "the box is at row %d on %d rows:\n%s" % (top, rows, _screen(shown))
    assert left == (cols - CTRL_C_MIN_TEXT - 6) // 2, \
        "the box is at column %d on %d columns:\n%s" % (left, cols, _screen(shown))
    assert any(CTRL_C_HEADER in r for r in shown), _screen(shown)
    assert not os.path.exists(str(work / "victim")), "the folder was not removed"
    assert not any(CTRL_C_NAMES[0] in r or CTRL_C_NAMES[1] in r for r in final), \
        "a box was left behind:\n" + _screen(final)


# ---- a dialog the user moved ----------------------------------------------

AI_TITLE = "AI settings"
MOVE_LEFT_BY = 4
CENTRE_SLACK = 1       # an odd width is centred to either neighbour


def _open_ai_settings(tmp_path, cols=80, rows=30):
    w = WpeSession(str(tmp_path), SEED, filename="t.c", cols=cols, rows=rows,
                   env_extra=AI_ENV)
    w.key(ALT.OPTIONS, delay=0.5)
    w.key("i", delay=0.7)
    return w


needs_ai = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


# @scenario SC-box-07
@needs_ai
def test_a_moved_dialog_keeps_its_place_across_a_picker_and_a_resize_centres_it(tmp_path):
    """The dialog is moved four columns left; a picker floats over it and is
    closed, and the dialog is drawn again where the user left it (not
    centred anew); then the terminal is resized and the owner centres it by
    its rule again.  The rows for the second half have always held; the
    first half is the owner's `redraw in place`."""
    with _open_ai_settings(tmp_path) as w:
        _, _, opened_left, opened_right = _titled_frame(w.display(), AI_TITLE)
        w.key(ALT_F2, delay=0.6)
        w.key(LEFT * MOVE_LEFT_BY, delay=1.0)
        w.key(ENTER, delay=0.6)
        moved = _titled_frame(w.display(), AI_TITLE)
        w.key(ALT_V, delay=0.8)
        assert "AI providers" in _screen(w.display()), "the picker did not open"
        w.key(ESC, delay=0.8)
        kept = _titled_frame(w.display(), AI_TITLE)
        w.resize(100, 30, delay=1.0)
        centred = _titled_frame(w.display(), AI_TITLE)
    assert moved[2] == opened_left - MOVE_LEFT_BY, "the keyboard move did not move the dialog"
    assert kept == moved, "the dialog was drawn at %r after the picker, not where it was left, %r" % (
        kept, moved)
    width = centred[3] - centred[2] + 1
    assert abs(centred[2] - (100 - width) // 2) <= CENTRE_SLACK, \
        "a resize left the dialog at column %d, not centred on 100 columns" % centred[2]


# @scenario SC-box-07
@needs_ai
def test_a_resize_during_the_keyboard_move_goes_on_from_the_centred_place(tmp_path):
    """Alt-F2 starts the move, the terminal is resized, an arrow moves the
    dialog one more cell: it moves from where the owner centred it, as a
    frame that is whole, not from the place it had on the old screen."""
    with _open_ai_settings(tmp_path) as w:
        w.key(ALT_F2, delay=0.6)
        w.key(LEFT * MOVE_LEFT_BY, delay=1.0)
        w.resize(100, 30, delay=1.0)
        w.key(RIGHT, delay=0.8)
        after = _titled_frame(w.display(), AI_TITLE)
        w.key(ENTER, delay=0.6)
    width = after[3] - after[2] + 1
    assert abs(after[2] - ((100 - width) // 2 + 1)) <= CENTRE_SLACK, \
        "the dialog is at column %d after the resize and one step right, not one right of the centre" % after[2]


# ---- a dialog cut by a small screen -----------------------------------------

FIND_TITLE = " Find "
CUT_SIZES = [(41, 12), (45, 15), (30, 8), (31, 7)]
CUT_ON_STATUS_ROW = pytest.mark.xfail(
    reason="INCOHERENCE: after a resize from 80x30 to 45x15 a '(' is left on "
           "the last column of the status line under Find; also on 0060461",
    strict=False)


def _bottom_border_whole(rows, title):
    """Is the bottom border of the box titled `title` an unbroken line?"""
    top, bottom, left, right = _titled_frame(rows, title)
    return set(rows[bottom][left + 1:right]) == {"q"}


# @scenario SC-box-04
@pytest.mark.parametrize("size,how", [
    pytest.param(s, h, id="%dx%d-%s" % (s[0], s[1], h),
                 marks=[CUT_ON_STATUS_ROW] if (s, h) == ((45, 15), "resized") else [])
    for s in CUT_SIZES for h in ("fresh", "resized")])
def test_a_dialog_cut_by_the_screen_marks_only_the_widgets_it_shows(tmp_path, size, how):
    """Search > Find on a screen too small for the whole dialog: the box is
    cut and the checkboxes and radio buttons that fall outside it are not
    drawn, and neither are their marks.  A mark of a hidden radio button
    was written on the bottom border, inside the box beside the cut text,
    and on the status line (F1 He*p)."""
    cols, rows = size
    start = size if how == "fresh" else (80, 30)
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=start[0], rows=start[1]) as w:
        if how == "resized":
            w.resize(cols, rows, delay=1.0)
        status = w.display()[-1]
        if how == "resized":
            w.resize(80, 30, delay=1.0)
        w.key(ALT.SEARCH, delay=0.5)
        w.key("f", delay=0.8)
        if how == "resized":
            w.resize(cols, rows, delay=1.2)
        shown = w.display()
    assert shown[-1] == status, "the status line changed under the dialog:\n%r\n%r" % (
        status, shown[-1])
    assert _bottom_border_whole(shown, FIND_TITLE), \
        "the dialog's bottom border is broken:\n" + _screen(shown)
    assert not any("*" in r for r in shown[1:-1] if "(*)" not in r), \
        "a mark stands where no widget is:\n" + _screen(shown)


# ---- a question longer than the screen ------------------------------------

LONG_FOLDER_BYTES = 3500       # a configuration folder far over the screen's rows
QUESTION_BUTTONS = ("Yes", "No", "Cancel")
QUESTION_CUT = "..."


# @scenario SC-box-08
def test_a_question_longer_than_the_screen_still_shows_its_buttons(tmp_path):
    """Save Options in a 3500 byte configuration folder asks "Save the options
    to <the path>?": the path is 55 lines on 80 columns, more than a 40 row
    screen holds.  The text is cut and says so, and Yes, No and Cancel stay
    on the screen, so Y answers.  The buttons used to fall below the screen
    and only Esc was left."""
    xdg = _folder_of_length(tmp_path / "cfg", LONG_FOLDER_BYTES)
    work = tmp_path / "work"
    work.mkdir()
    with WpeSession(str(work), SEED, filename="t.c", cols=80, rows=40,
                    env_extra={"XDG_CONFIG_HOME": xdg}) as w:
        w.key(ALT.OPTIONS, delay=1.0)
        w.key("s", delay=1.5)
        shown = w.display()
        w.key("y", delay=1.5)
    text = _screen(shown)
    assert "Save the options to" in text, text
    for button in QUESTION_BUTTONS:
        assert button in text, "%r is not on the screen:\n%s" % (button, text)
    assert QUESTION_CUT in text, "the cut text does not say so:\n" + text
    assert os.path.exists(os.path.join(xdg, "xwpe", "xwperc")), \
        "Y did not save the options under the folder"


# ---- every frame is opened by the owner -------------------------------------

MODAL_ENTER = re.compile(r"modal enter depth=(\d+) (.*)$")
MODAL_LEAVE = re.compile(r"modal leave depth=(\d+)")
KEY_READ = "a key read by a box"
# The boxes the layout owner opens; a frame written while one of these is the
# innermost box was opened around the owner.
OWNER_BOXES = ("a dialog", "the About box", "the System Info box",
               "an error box", "the Ctrl-C box")
DROPDOWN = "a dropdown"


def _outside_frames_by_box(trace):
    """{name of the innermost box: number of frames opened outside the path
    while it was} from the trace, following its modal enter and leave lines."""
    found = {}
    stack = []
    try:
        with open(trace) as fh:
            lines = fh.read().splitlines()
    except OSError:
        return found
    for line in lines:
        entered = MODAL_ENTER.search(line)
        left = MODAL_LEAVE.search(line)
        if entered:
            del stack[int(entered.group(1)) - 1:]
            stack.append(entered.group(2))
        elif left:
            del stack[int(left.group(1)):]
        elif OUTSIDE in line:
            boxes = [b for b in stack if b != KEY_READ]
            name = boxes[-1] if boxes else "no box"
            found[name] = found.get(name, 0) + 1
    return found


# @scenario SC-box-09
def test_the_boxes_on_the_owner_open_no_frame_outside_its_path(tmp_path):
    """The trace names every frame opened while no box's draw was running,
    and the box that was open then.  The control: the File dropdown is not on
    the owner yet (the next pass moves it, and this control then moves to a
    box that is still outside), and opening it writes the line.  The
    dialogs, the About and System Info boxes and the error box, with a
    resize under each, write none."""
    trace = tmp_path / "ui.trace"
    (tmp_path / "victim").mkdir()
    for name in CTRL_C_NAMES:
        (tmp_path / name).write_text("x\n")
    with WpeSession(str(tmp_path), SEED, filename="t.c", cols=80, rows=30,
                    env_extra={"XWPE_UI_TRACE": str(trace)}) as w:
        w.key(ALT.FILE, delay=0.6)
        w.key(ESC, delay=0.6)
        assert _outside_frames_by_box(trace).get(DROPDOWN, 0) >= 1, \
            "the rule did not fire for a box that goes around the owner"
        w.key(ALT.SEARCH, delay=0.4)
        w.key("g", delay=0.6)                       # Goto Line, a dialog
        w.resize(100, 40, delay=1.0)
        w.key(ESC, delay=0.6)
        w.menu(ALT.SYSTEM, "a", delay=0.8)           # About
        w.resize(60, 20, delay=1.0)
        w.key(ESC, delay=0.6)
        w.menu(ALT.SYSTEM, "s", delay=0.8)           # System Info
        w.resize(80, 30, delay=1.0)
        w.key(ESC, delay=0.6)
        _open_the_error_box(w)                        # the error box over the file manager
        w.resize(100, 40, delay=1.0)
        w.key(ENTER, delay=0.8)
        w.key(ESC, delay=0.6)
        _start_a_removal(w)                           # the box of a long file operation
        w.key("x", delay=1.0)
        w.key("x", delay=1.0)
        assert w.alive()
    around ={b: n for b, n in _outside_frames_by_box(trace).items() if b in OWNER_BOXES}
    assert not around, "a box opened its frame around the owner: %r\n%s" % (
        around, open(trace).read())
