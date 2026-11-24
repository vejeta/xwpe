"""X11: the screen grid has one owner that keeps the window usable.

Every row runs on an X server with no window manager, so nothing but the
editor decides the window's size:

  * -geometry larger than the screen starts the editor at the screen's size
    and says so.  Before the owner existed the editor asked the server for a
    back buffer of the requested size, the server refused it (BadAlloc on
    X_CreatePixmap) and the program ended before it painted anything.
  * A window made smaller than one cell keeps the grid's floor (30 x 6).
    Before, the grid was zero cells wide and the layout was run for 0 x 0.
  * A grow whose memory is refused leaves the editor on the grid it has, with
    its text, says so once and without waiting for a key, and takes a later
    grow.  The refusal comes from the editor's own seam for it
    (XWPE_TEST_FAULT=grid:N refuses the Nth take that asks for memory), so the
    row runs on any build.  A plain move of the window answers with no second
    box and no second layout walk, which is what a window whose pixels no
    longer match its grid used to do on every ConfigureNotify.
  * A start whose memory is refused comes up on the 80 x 24 the owner falls
    back to and says what it asked for and what it uses.
  * A click and a drag that end in the part of the window outside a kept grid
    change nothing.

There is no ceiling row here: no X window is wider than 32767 pixels, which
with a font eight pixels wide or more is 4095 cells, inside the grid's
ceiling of 4096.  The ceiling is the grid owner's own unit row
(tests/test_grid.c), and the clamp an X11 start does get, a window larger
than the screen, is the -geometry row above.

The editor's trace (XWPE_UI_TRACE) names the grid it took, and the notice
box is read from it and from the screen.
"""
import os
import re

import pytest

import gui_owner as owner_api
from conftest import (SCREEN, SCREEN_W, SCREEN_H, XwpeSession,
                      _find_xwpe_window, _window_size, _xdo, launch_editor,
                      serve_display, wait_for)

TRACE = "ui.trace"
# Deadlines, in seconds before XWPE_TEST_WAIT_SCALE: how long a row waits for
# a line of the trace, and how long it waits out what must not happen (an
# editor that ends, a layout walked again).
TRACE_S = 10.0
ALIVE_S = 2.0
FLOOR_GRID = "grid 30x6 taken"
CLAMP_NOTICE = "grid notice Screen cut to"
HUGE_GEOMETRY = "30000x30000"
HUGE_ASKED = "(asked 30000 x 30000)"
TINY_PIXELS = "5"
# What the layout owner writes when the grid has no cell.
ZERO_LAYOUT = "to 0x"
SOURCE = "int main(void){return 0;}\n"
# The window the editor starts on without a -geometry (80 columns and
# three quarters of the screen), the size a -geometry asks for in cells, and
# the pixel sizes the window is then asked for: a grow the seam refuses, a
# plain move that changes nothing, and a later grow the seam leaves alone.
START_GEOMETRY = "100x30"
GROW_WIDTH = "1000"
GROW_HEIGHT = "700"
MOVE_X = "10"
MOVE_Y = "10"
REGROW_WIDTH = "900"
REGROW_HEIGHT = "650"
# The seam's grammar: refuse the Nth take that asks for memory.  A start is
# the first of them, the grow after it the second.
FAULT_FIRST_TAKE = "grid:1"
FAULT_SECOND_TAKE = "grid:2"
# The layout owner's line for one walk of the desk, and the grid owner's for
# a size it kept and for what the user was told.
LAYOUT_WALK = "layout resized"
TAKEN_GRID = re.compile(r"grid (\d+)x(\d+) taken")
KEPT_GRID = re.compile(r"grid kept (\d+)x(\d+), refused (\d+)x(\d+)")
NOTICE_REFUSED = re.compile(
    r"grid notice Not Enough Memory for (\d+) x (\d+), keeping (\d+) x (\d+)")
FALLBACK_GRID = "grid 80x24 taken"
FALLBACK_ROWS = 24
# A pixel inside the window the editor starts on, and one of the remainder a
# grown window has past a grid that was kept.
INSIDE_PIXEL = (100, 100)
REMAINDER_PIXEL = (900, 650)
# Escape answers the box the reader shows for a size the grid kept.  No key
# of the row's own brings that box up: a resize of the window is not a key, so
# a row that had to press one for it would pass with a reader that owes the
# user the sentence until he touches the keyboard.
DISMISS_KEY = "Escape"
TYPED = "x"
KEY_S = 0.6
# How long a row waits out what a plain move of the window must not do.
SETTLE_S = 1.5


@pytest.fixture
def bare_display(gui_owner, tmp_path, request):
    """An X server with no window manager, for this row only.  The rows after
    this one ask the suite's display again, not this one."""
    display = owner_api.start_x_server(gui_owner, request, str(tmp_path),
                                       SCREEN)
    with serve_display(display):
        yield display


def _trace(tmp_path):
    try:
        with open(os.path.join(str(tmp_path), TRACE)) as fh:
            return fh.read()
    except OSError:
        return ""


def _start(gui_owner, tmp_path, request, args, extra_env=None):
    """Start xwpe on the bare display with the arguments; return the owner's
    record of it."""
    src = tmp_path / "t.c"
    src.write_text(SOURCE)
    (tmp_path / ".Xdefaults").write_text("xwpe.altMask: mod1\n")
    env = {"XWPE_UI_TRACE": TRACE, "XWPE_LSP_NO_EAGER": "1"}
    env.update(extra_env or {})
    return launch_editor(gui_owner, tmp_path, request, list(args) + [str(src)],
                         env)


def _session(owned, tmp_path):
    """The started editor as a session the row can type into and save from."""
    win = _find_xwpe_window()
    assert win, "the editor's window did not appear (exit %s)" % (
        owned.proc.poll(),)
    session = XwpeSession(owned.proc, win, owned)
    session.srcfile = str(tmp_path / "t.c")
    session.focus()
    return session


def _size(win, width, height):
    """Ask the server for a window size, as a drag of the window's edge does."""
    _xdo("windowsize", win, width, height)


def _wait_out(event_fn, what, seconds):
    """Wait out a stretch in which @event_fn must not come true.

    A row that judges what did NOT happen (an editor that ends, a layout
    walked a second time) has no event of its own to wait for, so it waits
    for the thing it forbids and goes on the moment it happens, leaving the
    sentence to the assertion that follows.  The owner's wait holds until
    that event or its deadline and stretches both by XWPE_TEST_WAIT_SCALE,
    so a loaded host waits the whole stretch out; a sleep of the row's own
    was stretched by nothing, and the row judged the editor before it had
    answered.  A wait that runs out is the answer the row wants, so its
    timeout is caught."""
    try:
        wait_for(event_fn, what, seconds)
    except owner_api.OwnerTimeout:
        pass


def _wait_refused(tmp_path):
    """Wait for the grid owner to keep its grid against a size it was asked
    for, and show the trace when it does not."""
    wait_for(lambda: KEPT_GRID.search(_trace(tmp_path)),
             "the editor to refuse the grown grid", TRACE_S,
             state_fn=lambda: _trace(tmp_path))


def _said_without_a_key(tmp_path):
    """Wait for the reader to say the size it kept, with no key pressed.

    A resize of the window hands the editor no key of its own, so the reader
    says the refusal at the pass that took the size, while nothing is being
    drawn: the box is up before the user touches the keyboard.  A reader that
    said it only at the top of its next pass left the box waiting for a key
    that the resize never delivers, and this wait timed out."""
    wait_for(lambda: NOTICE_REFUSED.search(_trace(tmp_path)),
             "the reader to say the refusal without a key press", TRACE_S,
             state_fn=lambda: _trace(tmp_path))


def _kept_and_notice(tmp_path):
    """The kept grid and the sentence that said so, as the trace holds them.

    Both must name the same two sizes: the sentence is what the user reads,
    and one that names another size than the grid in use tells him the wrong
    thing."""
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


def _a_larger_grid_was_taken(tmp_path, kept_cells):
    """True once the trace holds a grid of more cells than the one kept: a
    take after a refusal still works."""
    taken = TAKEN_GRID.findall(_trace(tmp_path))
    return any(int(cols) * int(rows) > kept_cells for cols, rows in taken)


def _a_move_changed_something(tmp_path, walks):
    """True once a move of the window has walked the layout again or said the
    refusal a second time: the two things a plain move must not do."""
    trace = _trace(tmp_path)
    return (trace.count(LAYOUT_WALK) > walks
            or len(NOTICE_REFUSED.findall(trace)) > 1)


def _dismiss_box(session):
    """Answer the box the reader showed, as the user does."""
    session.key(DISMISS_KEY, delay=KEY_S)


def _type_and_save(session):
    """Answer a box, type a character and save: what the editor holds must
    reach the disk."""
    _dismiss_box(session)
    session.key(TYPED, delay=KEY_S)
    session.save()
    assert TYPED in session.saved_text(), \
        "a typed character did not reach the saved file"


def test_geometry_larger_than_the_screen_starts_at_the_screen(
        bare_display, gui_owner, tmp_path, request):
    """-geometry 30000x30000 on a 1280x1024 screen: the window is no larger
    than the screen, the editor is alive and says what it asked for."""
    owned = _start(gui_owner, tmp_path, request, ["-geometry", HUGE_GEOMETRY])
    win = _find_xwpe_window()
    assert win, "no window: the editor ended on a geometry larger than the " \
        "screen (exit %s)" % owned.proc.poll()
    wait_for(lambda: CLAMP_NOTICE in _trace(tmp_path),
             "the notice of the clamped geometry", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    assert HUGE_ASKED in _trace(tmp_path)
    width, height = _window_size(win)
    assert width <= SCREEN_W and height <= SCREEN_H, \
        "the window is %dx%d on a %dx%d screen" % (width, height, SCREEN_W,
                                                    SCREEN_H)
    _wait_out(lambda: not owned.running(),
              "the editor to end after its window was made", ALIVE_S)
    assert owned.proc.poll() is None, "the editor ended after its window " \
        "was made"


def test_a_window_below_one_cell_keeps_the_floor(bare_display, gui_owner,
                                                 tmp_path, request):
    """The window is made 5 x 5 pixels: the grid is the floor, 30 x 6, and the
    layout is never run for a grid of no cell."""
    owned = _start(gui_owner, tmp_path, request, [])
    win = _find_xwpe_window()
    assert win, "the editor's window did not appear"
    _xdo("windowsize", win, TINY_PIXELS, TINY_PIXELS)
    wait_for(lambda: FLOOR_GRID in _trace(tmp_path) or
             ZERO_LAYOUT in _trace(tmp_path),
             "the editor to take the tiny window's size", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    assert ZERO_LAYOUT not in _trace(tmp_path), \
        "the layout ran for a grid of no cell:\n" + _trace(tmp_path)
    assert FLOOR_GRID in _trace(tmp_path)
    _wait_out(lambda: not owned.running(),
              "the editor to end on a tiny window", ALIVE_S)
    assert owned.proc.poll() is None, "the editor ended on a tiny window"


def test_a_refused_grow_keeps_the_editor_and_its_text(
        bare_display, gui_owner, tmp_path, request):
    """The window grows to 1000 x 700 pixels and the memory for that grid is
    refused: the editor keeps the grid it had with the text in it, says so
    once and before the row presses any key, and takes a later grow.  A move
    of the window, which is a ConfigureNotify like any other, walks the layout
    no more than once and says nothing again.  Before the owner the size was
    set at the top of the key reader and the planes were replaced later,
    inside the repaint, so a refusal left the new stride over the old block.
    """
    owned = _start(gui_owner, tmp_path, request, [],
                   {"XWPE_TEST_FAULT": FAULT_SECOND_TAKE})
    session = _session(owned, tmp_path)
    _size(session.win, GROW_WIDTH, GROW_HEIGHT)
    _wait_refused(tmp_path)
    _said_without_a_key(tmp_path)
    kept_cells = _kept_and_notice(tmp_path)
    trace = _trace(tmp_path)
    assert len(NOTICE_REFUSED.findall(trace)) == 1, \
        "the refusal was not said exactly once:\n" + trace
    walks = trace.count(LAYOUT_WALK)
    _xdo("windowmove", session.win, MOVE_X, MOVE_Y)
    _wait_out(lambda: _a_move_changed_something(tmp_path, walks),
              "a move of the window to walk the layout or say the refusal "
              "again", SETTLE_S)
    moved = _trace(tmp_path)
    assert moved.count(LAYOUT_WALK) == walks, \
        "a move of the window walked the layout again:\n" + moved
    assert len(NOTICE_REFUSED.findall(moved)) == 1, \
        "a move of the window said the refusal again:\n" + moved
    _type_and_save(session)
    _size(session.win, REGROW_WIDTH, REGROW_HEIGHT)
    wait_for(lambda: _a_larger_grid_was_taken(tmp_path, kept_cells),
             "a later grow to be taken", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    assert owned.proc.poll() is None, "the editor ended after a refused grow"


def test_a_start_without_memory_for_its_screen_says_so(
        bare_display, gui_owner, tmp_path, request):
    """The memory for the window's own grid is refused at start: the editor
    comes up on the 80 x 24 the owner falls back to, says what it asked for
    and what it uses, and works.  Before the owner the window was made, the
    planes were asked for and the answer was dropped, so the editor went on
    with no screen at all."""
    owned = _start(gui_owner, tmp_path, request, ["-geometry", START_GEOMETRY],
                   {"XWPE_TEST_FAULT": FAULT_FIRST_TAKE})
    session = _session(owned, tmp_path)
    wait_for(lambda: NOTICE_REFUSED.search(_trace(tmp_path)),
             "the refused start to be said", TRACE_S,
             state_fn=lambda: _trace(tmp_path))
    trace = _trace(tmp_path)
    assert FALLBACK_GRID in trace, \
        "the fallback grid is not in the trace:\n" + trace
    notice = NOTICE_REFUSED.search(trace)
    assert int(notice.group(4)) == FALLBACK_ROWS, \
        "the notice does not name the grid in use:\n" + trace
    _type_and_save(session)
    _wait_out(lambda: not owned.running(),
              "the editor to end after a refused start", ALIVE_S)
    assert owned.proc.poll() is None, "the editor ended after a refused start"


def test_a_click_and_a_drag_past_a_kept_grid_change_nothing(
        bare_display, gui_owner, tmp_path, request):
    """The window is 1000 x 700 pixels and the grid the editor kept is the
    small one it started on, so most of the window is remainder: the server's
    own background past the back buffer.  A click there, and a drag that
    starts inside the grid and ends in the remainder, change nothing.  The
    editor answers the next key, and the text it saves is the text it held
    with the one typed character in it."""
    owned = _start(gui_owner, tmp_path, request, [],
                   {"XWPE_TEST_FAULT": FAULT_SECOND_TAKE})
    session = _session(owned, tmp_path)
    _size(session.win, GROW_WIDTH, GROW_HEIGHT)
    _wait_refused(tmp_path)
    _said_without_a_key(tmp_path)
    _dismiss_box(session)
    session.click(*REMAINDER_PIXEL)
    session.drag(INSIDE_PIXEL[0], INSIDE_PIXEL[1],
                 REMAINDER_PIXEL[0], REMAINDER_PIXEL[1])
    assert owned.proc.poll() is None, \
        "a click past the kept grid ended the editor"
    session.key(TYPED, delay=KEY_S)
    session.save()
    assert session.saved_text().replace(TYPED, "", 1) == SOURCE, \
        "a click and a drag past the kept grid changed the text: %r" % (
            session.saved_text(),)
