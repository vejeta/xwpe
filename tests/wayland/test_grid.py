"""Wayland: the screen grid has one owner, and the frame follows it.

The compositor decides the window's size and the frame owner turns it into
cells.  What the user gets when the memory for a size is not there:

  * A grow the machine has no memory for leaves the editor on the grid it
    had, with the text in it, and says so once, without waiting for a key.
    The window stays the compositor's, so the strip past that grid is the
    frame's own black remainder, a later size is taken, and the compositor is
    answered with a buffer that fits what it asked.  Before the owner existed
    the frame kept the size it had fitted, so the next configure was compared
    with a grid the editor did not have and the remainder covered cells the
    editor was still drawing.
  * The sentence about a kept grid rides in the frame that answers the
    configure: the reader says it before that frame is committed, so no frame
    shows the remainder strip with nothing said about it, and the sentence
    never waits for an event the compositor does not owe the editor.
  * A start the machine has no memory for comes up on the 80 x 24 the owner
    falls back to, instead of a desk with no screen behind it.
  * A window smaller than one cell of the editor's frames keeps the floor of
    30 x 6, cut by the frame.
  * A click and a drag that end in the strip past a kept grid change nothing.

The refusals come from the editor's own seam for them (XWPE_TEST_FAULT=grid:N
refuses the Nth take that asks for memory), so the rows run on any build and
say nothing about how much memory the machine has.  What each row reads is on
the disk (the saved file) and in the editor's trace (XWPE_UI_TRACE); none
asserts only that the editor is up.

The editor's first grid is the size of the compositor's first configure, so a
row that wants a refused grow shrinks the output first and grows it back: the
take it then refuses is a grow.

There is no live ceiling row here.  This suite's compositor runs its kiosk
shell on the X11 backend, so its output is an X window on a 1024 x 768
screen: a surface above the grid's ceiling of 4096 cells (32768 pixels at an
eight-pixel cell) is larger than the output, and a fullscreen surface larger
than its output is a protocol error the compositor answers by disconnecting
the editor.  The ceiling the frame owner applies to a buffer of any size is a
unit row of its own (tests/test_wlframe.c, which needs no compositor), as is
the grid owner's ceiling (tests/test_grid.c).
"""
import os
import re

import pytest

from conftest import WL_HEIGHT, WL_WIDTH, _xdo, wait_for

TRACE = "ui.trace"
SOURCE = "int main(void){return 0;}\n"
# Deadlines, in seconds, for the events a row waits for.
TRACE_S = 20.0
KEY_S = 0.6
# An output smaller than the one the editor starts on: the grow back to the
# compositor's own size is what a row refuses.  The floor row's descent ends
# at the smallest output this suite's compositor answers (a smaller one
# produced no configure at all), and runs at a point size whose cell is wide
# enough that even that output holds fewer columns and rows than the floor.
SHRINK_WIDTH = "800"
SHRINK_HEIGHT = "600"
FLOOR_FONT_SIZE = "30"
FLOOR_DESCENT = (("600", "400"), ("450", "250"), ("300", "150"))
GROW_WIDTH = str(WL_WIDTH)
GROW_HEIGHT = str(WL_HEIGHT)
# A size asked for after a refusal: the seam refuses one take only.
REGROW_WIDTH = "900"
REGROW_HEIGHT = "700"
# The seam's grammar: refuse the Nth take that asks for memory.  A start is
# the first of them; the shrink a refused-grow row makes is the second, so the
# grow back is the third.
FAULT_FIRST_TAKE = "grid:1"
FAULT_THIRD_TAKE = "grid:3"
TAKEN_GRID = re.compile(r"grid (\d+)x(\d+) taken")
KEPT_GRID = re.compile(r"grid kept (\d+)x(\d+), refused (\d+)x(\d+)")
NOTICE_REFUSED = re.compile(
    r"grid notice Not Enough Memory for (\d+) x (\d+), keeping (\d+) x (\d+)")
# The grid the owner falls back to when the first take is refused, and the
# floor a window below one cell keeps.
FALLBACK_GRID = "grid 80x24 taken"
FALLBACK_COLS = 80
FALLBACK_ROWS = 24
FLOOR_GRID = "grid 30x6 taken"
FLOOR_CELLS = 30 * 6
# Escape answers the box the reader shows for a size the grid kept.  No key of
# the row's own brings that box up: a configure from the compositor is not a
# key, so a row that had to press one for it would pass with a reader that
# owes the user the sentence until he touches the keyboard.
DISMISS_KEY = "Escape"
TYPED = "x"
# A pixel inside the shrunk grid a refused grow keeps, and one of the strip
# past it in the window the compositor went on holding.
INSIDE_PIXEL = (100, 100)
REMAINDER_PIXEL = (950, 700)
# What a row arms, with the trace every row reads and the program the fixture
# puts in t.c.
KEPT_ENV = {"XWPE_TEST_FAULT": FAULT_THIRD_TAKE, "XWPE_UI_TRACE": TRACE,
            "XWPE_TEST_SOURCE": SOURCE}
# The frame owner's trace and the editor's own in one file: both writers
# append a line at a time, so the order of the lines is the order of the
# calls, and a row can read that the sentence was said before the frame that
# answers the configure was committed.
KEPT_SAY_ORDER_ENV = {"XWPE_TEST_FAULT": FAULT_THIRD_TAKE,
                      "XWPE_UI_TRACE": TRACE, "XWPE_WL_FRAME_TRACE": TRACE,
                      "XWPE_TEST_SOURCE": SOURCE}
# The three lines of that order: the frame owner takes the kept grid for the
# buffer, the reader says the sentence, the frame owner commits the frame
# that carries both.
KEEP_GRID_LINE = "keep grid="
NOTICE_LINE = "grid notice "
COMMIT_LINE = "commit "
START_ENV = {"XWPE_TEST_FAULT": FAULT_FIRST_TAKE, "XWPE_UI_TRACE": TRACE,
             "XWPE_TEST_SOURCE": SOURCE}
FLOOR_ENV = {"XWPE_UI_TRACE": TRACE, "XWPE_TEST_SOURCE": SOURCE,
             "XWPE_FONT_SIZE": FLOOR_FONT_SIZE}


def _trace(tmp_path):
    try:
        with open(os.path.join(str(tmp_path), TRACE)) as fh:
            return fh.read()
    except OSError:
        return ""


def _trace_lines(tmp_path):
    """The lines of the trace, in the order they were written."""
    return _trace(tmp_path).splitlines()


def _first_after(lines, position, opening):
    """The position of the first line after `position` that opens with
    `opening`, or None when there is none."""
    for later, line in enumerate(lines):
        if later > position and line.startswith(opening):
            return later
    return None


def _positions(lines, opening):
    """The positions of the lines that open with `opening`."""
    return [i for i, line in enumerate(lines) if line.startswith(opening)]


def _kept_frame_committed(tmp_path):
    """True once the frame that answers a kept configure is committed: the
    frame owner took the kept grid for the buffer and a commit followed."""
    lines = _trace_lines(tmp_path)
    kept = _positions(lines, KEEP_GRID_LINE)
    return bool(kept) and \
        _first_after(lines, kept[0], COMMIT_LINE) is not None


def _taken_grids(tmp_path):
    """The grids the editor took, in the order it took them."""
    return [(int(cols), int(rows)) for cols, rows
            in TAKEN_GRID.findall(_trace(tmp_path))]


def _wait_takes(tmp_path, count):
    """Wait until the editor has taken `count` grids, and show the trace when
    it does not: a row that refuses a later take must know the earlier ones
    happened."""
    wait_for(lambda: len(_taken_grids(tmp_path)) >= count,
             "the editor to take %d grids" % count, TRACE_S,
             state_fn=lambda: _trace(tmp_path))


def _wait_refused(tmp_path):
    """Wait for the grid owner to keep its grid against the size the
    compositor asked for, and show the trace when it does not."""
    wait_for(lambda: KEPT_GRID.search(_trace(tmp_path)),
             "the editor to refuse the grown grid", TRACE_S,
             state_fn=lambda: _trace(tmp_path))


def _said_without_a_key(tmp_path):
    """Wait for the reader to say the size it kept, with no key pressed.

    A configure from the compositor hands the editor no key of its own, so
    the reader says the refusal at the pass that took the size: the box is up
    before the user touches the keyboard.  On X11, where the reader said it
    only at the top of its next pass, the box waited for a key that a resize
    never delivers and this same wait timed out."""
    wait_for(lambda: NOTICE_REFUSED.search(_trace(tmp_path)),
             "the reader to say the refusal without a key press", TRACE_S,
             state_fn=lambda: _trace(tmp_path))


def _kept_cells(tmp_path):
    """The cells of the kept grid, after checking that the sentence the user
    reads names the size asked for and the size in use: a sentence that names
    another grid than the one kept tells him the wrong thing."""
    trace = _trace(tmp_path)
    kept = KEPT_GRID.search(trace)
    notice = NOTICE_REFUSED.search(trace)
    assert kept, "no kept grid in the trace:\n" + trace
    assert notice, "the refusal was not said:\n" + trace
    assert (int(notice.group(3)), int(notice.group(4))) == \
        (int(kept.group(1)), int(kept.group(2))), \
        "the notice names another grid than the one kept:\n" + trace
    assert (int(notice.group(1)), int(notice.group(2))) == \
        (int(kept.group(3)), int(kept.group(4))), \
        "the notice names another size than the one refused:\n" + trace
    return int(kept.group(1)) * int(kept.group(2))


def _a_larger_grid_was_taken(tmp_path, cells):
    """True once the trace holds a grid of more cells than `cells`: a take
    after a refusal still works."""
    return any(cols * rows > cells for cols, rows in _taken_grids(tmp_path))


def _answer_the_box_type_and_save(session):
    """Answer the box the reader showed, type a character and save: what the
    editor holds must reach the disk."""
    session.key(DISMISS_KEY, delay=KEY_S)
    session.key(TYPED, delay=KEY_S)
    session.save()
    assert TYPED in session.saved_text(), \
        "a typed character did not reach the saved file"


def _kept_grow(xwpe, tmp_path):
    """Shrink the output, grow it back, and wait for the grow to be refused.

    The seam refuses the third take that asks for memory: the compositor's
    first configure, the shrink, and then the grow, which is the one that
    leaves the editor with the grid it had."""
    _wait_takes(tmp_path, 1)
    _xdo("windowsize", xwpe.wid, SHRINK_WIDTH, SHRINK_HEIGHT)
    _wait_takes(tmp_path, 2)
    _xdo("windowsize", xwpe.wid, GROW_WIDTH, GROW_HEIGHT)
    _wait_refused(tmp_path)
    _said_without_a_key(tmp_path)
    return _kept_cells(tmp_path)


@pytest.mark.parametrize("xwpe", [KEPT_ENV], indirect=True)
def test_a_refused_grow_keeps_the_editor_and_its_text(xwpe, tmp_path):
    """The output is shrunk and grown back and the memory for the grown grid
    is refused: the editor keeps the smaller grid with the text in it, says so
    exactly once and before the row presses any key, and takes a later size.
    Before the owner the frame went on holding the size it had fitted, so the
    next configure was compared with a grid the editor did not have."""
    kept_cells = _kept_grow(xwpe, tmp_path)
    trace = _trace(tmp_path)
    assert len(NOTICE_REFUSED.findall(trace)) == 1, \
        "the refusal was not said exactly once:\n" + trace
    _answer_the_box_type_and_save(xwpe)
    _xdo("windowsize", xwpe.wid, REGROW_WIDTH, REGROW_HEIGHT)
    wait_for(lambda: _a_larger_grid_was_taken(tmp_path, kept_cells),
             "a later size to be taken", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    assert xwpe.proc.poll() is None, "the editor ended after a refused grow"


@pytest.mark.parametrize("xwpe", [KEPT_SAY_ORDER_ENV], indirect=True)
def test_a_refused_grow_is_said_in_the_frame_that_answers_it(xwpe, tmp_path):
    """The grow is refused and the frame that answers the compositor's
    configure carries the sentence: the reader says it at the pass that took
    the size, before that pass commits.  No frame is shown with the black
    strip past the kept grid and nothing said about it, and the sentence does
    not wait for an event the compositor does not owe the editor.

    A configure hands the editor no key, so a reader that said the sentence
    at a later pass would owe it until something else woke the input loop.
    Both traces go to one file here, so the order of its lines is the order of
    the calls, and the row reads that the sentence was written before the
    commit of the kept frame.  No key of the row's own brings either about."""
    _kept_grow(xwpe, tmp_path)
    wait_for(lambda: _kept_frame_committed(tmp_path),
             "the frame that answers the kept configure to be committed",
             TRACE_S, state_fn=lambda: _trace(tmp_path))
    lines = _trace_lines(tmp_path)
    kept = _positions(lines, KEEP_GRID_LINE)[0]
    commit = _first_after(lines, kept, COMMIT_LINE)
    notice = _positions(lines, NOTICE_LINE)
    assert notice, "the refusal was not said:\n" + "\n".join(lines)
    assert notice[0] < commit, \
        "the sentence came after the frame that answers the configure was " \
        "committed, so that frame showed the strip past the kept grid with " \
        "nothing said about it:\n" + "\n".join(lines)
    assert NOTICE_REFUSED.search(lines[notice[0]]), \
        "the sentence is not the refusal:\n" + lines[notice[0]]
    _answer_the_box_type_and_save(xwpe)
    assert xwpe.proc.poll() is None, \
        "the editor ended after a refusal it said in its own frame"


@pytest.mark.parametrize("xwpe", [KEPT_ENV], indirect=True)
def test_a_click_and_a_drag_past_a_kept_grid_change_nothing(xwpe, tmp_path):
    """The window is the compositor's own size and the grid the editor kept is
    the smaller one, so part of the window is the frame's black remainder.  A
    click there, and a drag that starts inside the grid and ends in the
    remainder, change nothing: the editor answers the next key, and the text
    it saves is the text it held with the one typed character in it."""
    _kept_grow(xwpe, tmp_path)
    xwpe.key(DISMISS_KEY, delay=KEY_S)
    xwpe.click(*REMAINDER_PIXEL)
    xwpe.drag(INSIDE_PIXEL[0], INSIDE_PIXEL[1],
              REMAINDER_PIXEL[0], REMAINDER_PIXEL[1])
    assert xwpe.proc.poll() is None, \
        "a click past the kept grid ended the editor"
    xwpe.key(TYPED, delay=KEY_S)
    xwpe.save()
    assert xwpe.saved_text().replace(TYPED, "", 1) == SOURCE, \
        "a click and a drag past the kept grid changed the text: %r" % (
            xwpe.saved_text(),)


@pytest.mark.parametrize("xwpe", [START_ENV], indirect=True)
def test_a_start_without_memory_for_its_screen_falls_back(xwpe, tmp_path):
    """The memory for the first grid is refused: the owner takes the 80 x 24
    it always has room for, says what it asked for and what it uses, and the
    editor works.  Before the owner a refused start left the display with no
    screen behind it."""
    wait_for(lambda: FALLBACK_GRID in _trace(tmp_path),
             "the owner to fall back to its own 80 x 24", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    assert xwpe.proc.poll() is None, "the editor ended on a refused first grid"
    _said_without_a_key(tmp_path)
    notice = NOTICE_REFUSED.search(_trace(tmp_path))
    assert (int(notice.group(3)), int(notice.group(4))) == \
        (FALLBACK_COLS, FALLBACK_ROWS), \
        "the notice does not name the grid in use:\n" + _trace(tmp_path)
    _answer_the_box_type_and_save(xwpe)
    assert xwpe.proc.poll() is None, "the editor ended after a refused start"


@pytest.mark.parametrize("xwpe", [FLOOR_ENV], indirect=True)
def test_a_window_below_the_floor_keeps_it(xwpe, tmp_path):
    """The output is stepped down until it holds fewer cells than the editor's
    floor: the grid keeps 30 x 6 and the editor still works when the output
    grows again.  The floor is the grid owner's, so a display that had none of
    its own no longer runs the layout for a grid of no cell."""
    _wait_takes(tmp_path, 1)
    for width, height in FLOOR_DESCENT:
        _xdo("windowsize", xwpe.wid, width, height)
    wait_for(lambda: FLOOR_GRID in _trace(tmp_path),
             "the editor to keep the grid's floor", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    assert xwpe.proc.poll() is None, "the editor ended on a tiny window"
    _xdo("windowsize", xwpe.wid, REGROW_WIDTH, REGROW_HEIGHT)
    wait_for(lambda: _a_larger_grid_was_taken(tmp_path, FLOOR_CELLS),
             "the editor to take the grown window", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    _answer_the_box_type_and_save(xwpe)
