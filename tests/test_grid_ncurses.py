"""ncurses: the screen grid has one owner, and it keeps the editor alive.

What the user gets on a terminal the editor's own memory or size limits
cannot hold:

  * A resize the machine has no memory for leaves the editor as it was, with
    its text, and says so once.  Before the owner existed the editor asked
    for the new planes with a realloc that overwrote its only pointer, wrote
    through the NULL it got back and died of a segmentation fault, losing the
    unsaved text.  Only the sanitizer build can refuse exactly the editor's
    large block (max_allocation_size_mb, a sanitizer flag) and nothing of
    curses', so the row runs when the editor under test is the wpe-asan build
    and skips otherwise.
  * The same refusal on any build, through the seam the editor gives a test
    for it: XWPE_TEST_FAULT=grid:N refuses the Nth grid take that asks for
    memory.  With it a refused grow, a refused start and a click past the
    grid the editor kept are rows of the plain binary.
  * A terminal larger than the grid's ceiling (4096 columns or rows) starts
    on the ceiling's grid and says so.
  * A terminal too large for curses to hold in the memory the machine gives
    the editor starts on the largest screen curses accepts, as vim does, and
    says so.  The plain binary runs under an address-space limit for this
    (the sanitizer build cannot take one).
  * A terminal smaller than the editor's floor keeps the floor as its grid,
    and curses runs at the terminal's own size.  Told the floor instead,
    curses writes every line wider than the terminal, which wraps it and
    scrolls the editor off the screen.  Nothing is said: the floor is not a
    size the editor cut down, it is the smallest screen its own frames fit
    on.

What each row reads is on the disk (the saved file), in the editor's trace
(XWPE_UI_TRACE) and on the screen; none asserts only that the editor is up.
"""
import os
import re
import stat
import sys

import pytest

import wpe_driver
from wpe_driver import WpeSession

SEED = "int main(void){return 0;}\n"
ESC = "\033"
TYPED = "x"
TRACE_NAME = "ui.trace"
# The driver's terminal is 80 x 30 until a row says otherwise.
NOTICE_REFUSED = "Not Enough Memory for 1000 x 500, keeping 80 x 30"
NOTICE_CLAMP_TEXT = "Screen cut to"
# The clamp of a start too large for curses is read from the trace, not from
# the screen: the editor paints a 750 x 750 desk there, and pyte has not read
# the bytes of the box, which come last, by the time the row looks.
NOTICE_CLAMP_TRACE = "grid notice Screen cut to"
BOX_PLACED = "layout placed an error box at"

# The sanitizer's refusal of one block above this many megabytes: the editor's
# planes for 1000 x 500 are 6 MB, and nothing at start is above 4 MB.
ASAN_REFUSE_OPTIONS = ("detect_leaks=0:allocator_may_return_null=1:"
                       "max_allocation_size_mb=4")
ASAN_BINARY_PREFIX = "wpe-asan"

CEILING_COLS = 4096
CEILING_ROWS = 10
OVER_CEILING_COLS = 5000

# The address space the editor may use in the start-clamp row: curses needs
# three screens of 28 bytes a cell, so a terminal of 3000 x 3000 does not fit
# and one of a quarter of it does.
START_ADDRESS_SPACE = 600 * 1024 * 1024
START_COLS = 3000
START_ROWS = 3000

GRID_TAKEN = re.compile(r"grid (\d+)x(\d+) taken")
# The size curses runs at, as the editor writes it to its trace when it
# starts curses.  The grid's floor and the terminal's own size are two
# different sizes on a terminal below the floor, and only one of them may be
# handed to curses.
CURSES_RUNS_AT = re.compile(r"curses runs at (\d+)x(\d+)")

# The refusal a row shows without the sanitizer: the seam refuses the Nth take
# that asks for memory, and the start of the editor is the first of them.
FAULT_FIRST_TAKE = "grid:1"
FAULT_SECOND_TAKE = "grid:2"
# The driver's terminal, a grow the seam refuses, and a later grow it does not.
DRIVER_COLS = 80
DRIVER_ROWS = 30
GROW_COLS = 200
GROW_ROWS = 60
REGROW_COLS = 120
REGROW_ROWS = 40
KEPT_GROW = "grid kept %dx%d, refused %dx%d" % (DRIVER_COLS, DRIVER_ROWS,
                                               GROW_COLS, GROW_ROWS)
NOTICE_REFUSED_GROW = "Not Enough Memory for %d x %d, keeping %d x %d" % (
    GROW_COLS, GROW_ROWS, DRIVER_COLS, DRIVER_ROWS)
REGROW_TAKEN = "grid %dx%d taken" % (REGROW_COLS, REGROW_ROWS)
# A start whose memory is refused: the grid the owner falls back to.
FALLBACK_COLS = 80
FALLBACK_ROWS = 24
FALLBACK_TAKEN = "grid %dx%d taken" % (FALLBACK_COLS, FALLBACK_ROWS)
NOTICE_REFUSED_START = "Not Enough Memory for %d x %d, keeping %d x %d" % (
    DRIVER_COLS, DRIVER_ROWS, FALLBACK_COLS, FALLBACK_ROWS)
# A terminal too small for the editor's own frames keeps the floor.
FLOOR_COLS = 10
FLOOR_ROWS = 3
FLOOR_GRID = "grid 30x6 taken"
NOTICE_TRACE = "grid notice"
# The part of a 200 x 60 terminal outside a kept 80 x 30 grid, and a cell
# inside that grid: a click and a drag that end in the remainder.
REMAINDER_COL = 150
REMAINDER_ROW = 45
INSIDE_COL = 10
INSIDE_ROW = 5
# An SGR (1006) mouse report's button field: the first button, and a move
# with it held.
SGR_BUTTON_ONE = 0
SGR_BUTTON_DRAG = 32


def _fault_env(tmp_path, fault):
    """The editor's trace, plus the seam that refuses one grid take: the
    refusal a row shows then does not depend on the machine's memory."""
    return {"XWPE_TEST_FAULT": fault,
            "XWPE_UI_TRACE": os.path.join(str(tmp_path), TRACE_NAME)}


def _sgr(button, col, row, released):
    """One SGR (1006) mouse report, in the terminal's own encoding: 1-based
    cells, an upper-case final byte for a press or a move, a lower-case one
    for a release."""
    return "\033[<%d;%d;%d%s" % (button, col, row, "m" if released else "M")


def _trace(workdir):
    try:
        with open(os.path.join(workdir, TRACE_NAME)) as fh:
            return fh.read()
    except OSError:
        return ""


def _screen_text(session):
    return "\n".join(session.display())


def _type_and_save(session):
    """Type a character and save: what the editor holds must reach the disk."""
    session.key(TYPED)
    session.save()
    assert TYPED in session.text(), \
        "a typed character did not reach the saved file"


def _dismiss_notice_and_save(session):
    """Answer the notice box (Esc), type a character and save: the text the
    editor still holds must reach the disk."""
    session.key(ESC)
    _type_and_save(session)


def _under_asan():
    return os.path.basename(wpe_driver.WPE_BIN).startswith(ASAN_BINARY_PREFIX)


def test_refused_resize_keeps_the_editor_and_its_text(tmp_path):
    """The grid grows to 1000 x 500 and the memory for it is refused: the
    editor keeps its 80 x 30, shows one box that says so, and the text it
    holds is saved when asked.  On the tree before the owner it ended by
    signal 11 and left a crash report that names MAXSCOL=1000."""
    if not _under_asan():
        pytest.skip("needs the sanitizer build (max_allocation_size_mb): "
                    "run it as wpe-asan")
    env = {"ASAN_OPTIONS": ASAN_REFUSE_OPTIONS,
           "XWPE_UI_TRACE": os.path.join(str(tmp_path), TRACE_NAME)}
    with WpeSession(str(tmp_path), SEED, env_extra=env, wait=2.0) as w:
        w.resize(1000, 500, delay=4.0)
        assert w.alive(), "the editor ended on a refused resize"
        assert NOTICE_REFUSED in _screen_text(w), \
            "the refused resize was not said on the screen"
        _dismiss_notice_and_save(w)
        assert "grid kept 80x30, refused 1000x500" in _trace(str(tmp_path))


def test_a_terminal_wider_than_the_ceiling_starts_on_the_ceiling(tmp_path):
    """A 5000 x 10 terminal fits in curses, but not in the editor's counts:
    the editor works on the top left 4096 x 10 cells and says so."""
    env = {"XWPE_UI_TRACE": os.path.join(str(tmp_path), TRACE_NAME)}
    with WpeSession(str(tmp_path), SEED, cols=OVER_CEILING_COLS,
                    rows=CEILING_ROWS, env_extra=env, wait=3.0) as w:
        assert w.alive()
        assert "%s %d x %d (asked %d x %d)" % (
            NOTICE_CLAMP_TEXT, CEILING_COLS, CEILING_ROWS,
            OVER_CEILING_COLS, CEILING_ROWS) in _screen_text(w)
        _dismiss_notice_and_save(w)
        assert "grid %dx%d taken" % (CEILING_COLS, CEILING_ROWS) in \
            _trace(str(tmp_path))


def _limited_binary(workdir):
    """A launcher that runs the editor under an address-space limit: the
    plain binary needs the limit from its first instruction, and the row's
    own process must not be limited."""
    path = os.path.join(workdir, "limited-wpe")
    with open(path, "w") as fh:
        fh.write("#!/bin/sh\nulimit -v %d\nexec \"$XWPE_REAL_BIN\" \"$@\"\n"
                 % (START_ADDRESS_SPACE // 1024))
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)
    return path


def test_a_terminal_too_large_for_curses_starts_clamped(tmp_path, monkeypatch):
    """curses cannot hold a 3000 x 3000 terminal in the memory the editor is
    given and used to end with its own sentence (cannot initialize terminal
    type).  Now the editor starts on the largest screen curses accepts, says
    what it asked for and what it uses, and works."""
    if _under_asan():
        pytest.skip("the sanitizer build cannot run under an address-space "
                    "limit; this row runs the plain binary")
    real_bin = wpe_driver.WPE_BIN
    monkeypatch.setattr(wpe_driver, "WPE_BIN", _limited_binary(str(tmp_path)))
    env = {"XWPE_REAL_BIN": os.path.abspath(real_bin),
           "XWPE_UI_TRACE": os.path.join(str(tmp_path), TRACE_NAME)}
    with WpeSession(str(tmp_path), SEED, cols=START_COLS, rows=START_ROWS,
                    env_extra=env, wait=8.0) as w:
        assert w.alive(), "the editor did not start on a terminal too large " \
            "for curses"
        trace = _trace(str(tmp_path))
        taken = GRID_TAKEN.findall(trace)
        assert taken, "no grid in the trace:\n" + trace
        cols, rows = (int(v) for v in taken[0])
        assert 80 <= cols < START_COLS and 24 <= rows < START_ROWS, \
            "the grid %dx%d is not a clamp of %dx%d" % (
                cols, rows, START_COLS, START_ROWS)
        assert "%s %d x %d (asked %d x %d)" % (
            NOTICE_CLAMP_TRACE, cols, rows, START_COLS, START_ROWS) in trace, \
            "the clamp was not said:\n" + trace
        assert BOX_PLACED in trace, "no box carried the clamp:\n" + trace
        _dismiss_notice_and_save(w)


def test_a_refused_grow_keeps_the_editor_and_its_text(tmp_path):
    """The terminal grows to 200 x 60 and the memory for that grid is
    refused: the editor keeps its 80 x 30 with the text it holds, says so
    once, and takes the next grow.  Before the owner, the size was set and
    the planes reallocated over the editor's only pointer, so a refusal left
    it writing through NULL.  This is the seam's twin of the sanitizer row
    above: it runs on any build."""
    env = _fault_env(tmp_path, FAULT_SECOND_TAKE)
    with WpeSession(str(tmp_path), SEED, env_extra=env, wait=2.0) as w:
        w.resize(GROW_COLS, GROW_ROWS, delay=2.0)
        assert w.alive(), "the editor ended on a refused grow"
        assert NOTICE_REFUSED_GROW in _screen_text(w), \
            "the refused grow was not said on the screen"
        trace = _trace(str(tmp_path))
        assert KEPT_GROW in trace, "the kept grid is not in the trace:\n" + trace
        assert trace.count(NOTICE_REFUSED_GROW) == 1, \
            "the refusal was not said exactly once:\n" + trace
        _dismiss_notice_and_save(w)
        w.resize(REGROW_COLS, REGROW_ROWS, delay=2.0)
        assert REGROW_TAKEN in _trace(str(tmp_path)), \
            "a later grow was not taken:\n" + _trace(str(tmp_path))


def test_a_start_without_memory_for_its_screen_says_so(tmp_path):
    """The memory for the terminal's own 80 x 30 is refused at start: the
    editor comes up on the 80 x 24 it always has room for, says what it asked
    for and what it uses, and works.  Before the owner there was no first
    grid to fall back on."""
    env = _fault_env(tmp_path, FAULT_FIRST_TAKE)
    with WpeSession(str(tmp_path), SEED, env_extra=env, wait=2.0) as w:
        assert w.alive(), "the editor did not start on a refused first grid"
        assert NOTICE_REFUSED_START in _screen_text(w), \
            "the refused start was not said on the screen"
        assert FALLBACK_TAKEN in _trace(str(tmp_path)), \
            "the fallback grid is not in the trace:\n" + _trace(str(tmp_path))
        _dismiss_notice_and_save(w)


def test_a_terminal_smaller_than_the_floor_keeps_it(tmp_path):
    """A 10 x 3 terminal is too small for the editor's own frames: the grid
    keeps its floor of 30 x 6, curses runs at the terminal's own 10 x 3 and
    cuts what does not fit, and nothing is said.  A curses told the floor
    writes 30 columns into a 10 column terminal, which wraps them and scrolls
    the editor off the screen.  A grow back to the usual size is taken and a
    typed character reaches the file, so the floor left a working editor
    behind."""
    env = {"XWPE_UI_TRACE": os.path.join(str(tmp_path), TRACE_NAME)}
    with WpeSession(str(tmp_path), SEED, cols=FLOOR_COLS, rows=FLOOR_ROWS,
                    env_extra=env, wait=2.0) as w:
        assert w.alive(), "the editor ended on a terminal below the floor"
        trace = _trace(str(tmp_path))
        assert FLOOR_GRID in trace, "the floor is not in the trace:\n" + trace
        assert NOTICE_TRACE not in trace, \
            "the floor was said as a clamp:\n" + trace
        ran_at = CURSES_RUNS_AT.search(trace)
        assert ran_at, "the size curses runs at is not in the trace:\n" + trace
        assert (int(ran_at.group(1)), int(ran_at.group(2))) == \
            (FLOOR_COLS, FLOOR_ROWS), \
            "curses runs at %sx%s on a %dx%d terminal, so it was told the " \
            "grid's floor: every line it writes is wider than the terminal, " \
            "which wraps it and scrolls the editor off the screen:\n%s" % (
                ran_at.group(1), ran_at.group(2), FLOOR_COLS, FLOOR_ROWS,
                trace)
        w.resize(DRIVER_COLS, DRIVER_ROWS, delay=2.0)
        _type_and_save(w)


def test_a_click_and_a_drag_past_a_kept_grid_change_nothing(tmp_path):
    """The terminal is 200 x 60 and the grid the editor kept is 80 x 30, so
    most of the window is remainder.  A click there and a drag that starts
    inside the grid and ends in the remainder change nothing: the editor
    answers the next key, and the text it saves is the text it held with the
    one typed character in it."""
    env = _fault_env(tmp_path, FAULT_SECOND_TAKE)
    with WpeSession(str(tmp_path), SEED, env_extra=env, wait=2.0) as w:
        w.resize(GROW_COLS, GROW_ROWS, delay=2.0)
        assert KEPT_GROW in _trace(str(tmp_path)), \
            "the grid was not kept:\n" + _trace(str(tmp_path))
        w.key(ESC)
        w.key(_sgr(SGR_BUTTON_ONE, REMAINDER_COL, REMAINDER_ROW, False),
              delay=0.2)
        w.key(_sgr(SGR_BUTTON_ONE, REMAINDER_COL, REMAINDER_ROW, True),
              delay=0.4)
        w.key(_sgr(SGR_BUTTON_ONE, INSIDE_COL, INSIDE_ROW, False), delay=0.2)
        w.key(_sgr(SGR_BUTTON_DRAG, REMAINDER_COL, REMAINDER_ROW, False),
              delay=0.2)
        w.key(_sgr(SGR_BUTTON_ONE, REMAINDER_COL, REMAINDER_ROW, True),
              delay=0.4)
        assert w.alive(), "a click past the kept grid ended the editor"
        w.key(ESC)
        _type_and_save(w)
        assert w.text().replace(TYPED, "", 1) == SEED, \
            "a click and a drag past the kept grid changed the text: %r" % (
                w.text(),)
