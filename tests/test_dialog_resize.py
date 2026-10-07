"""Modal dialogs across a terminal resize (ncurses).

A resize while a dialog is up goes SIGWINCH -> KEY_RESIZE -> the editor
re-lays-out the desk and returns WPE_RESIZE to the dialog engine, which places
its box again for the new screen.  Every test here drives a real resize
through WpeSession.resize() and asserts what only that relayout produces: the
status line on the new last row, the box placed again inside the new screen
(re-centred, or clamped into the corner when it no longer fits), and the editor
taking input afterwards.  Liveness alone is not asserted anywhere: a build
whose KEY_RESIZE handling does nothing survives every resize here.

Run: tests/.venv/bin/python -m pytest -v tests/test_dialog_resize.py

The original crash this file was written for: e_pr_char (macro) wrote to
schirm[] without bounds checking, so after a REALLOC to smaller dimensions the
old coordinates overwrote heap metadata.
"""
from wpe_driver import WpeSession, ALT

ESC = "\033"
F2_SAVE = "\033OQ"
SEED = "int main(void){return 0;}\n"
# The engine shortens a title that no longer fits ("Goto Line Nu..." on a
# 30-column box), so the box is found by the start of its title.
GOTO_TITLE = "Goto Line"
FIND_TITLE = "Find"
NOT_FOUND_TITLE = "Message"
NOT_FOUND_TEXT = "String NOT found"
STATUS_TEXT = "F1 Help"
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as l (top-left), k (top-right), m (bottom-left).
CORNER_TL, CORNER_TR, CORNER_BL = "l", "k", "m"
MENU_BAR_ROW = 0
# A dialog that does not fit is clamped to the top-left of the desk: column 0,
# the row under the menu bar.
CORNER_CLAMP = (1, 0)
# A box centred on the full-width editor window has its middle column within
# this many cells of the screen's middle column (rounding of odd widths).
CENTRE_SLACK = 2
# The dialog engine clamps the desk to at least 30 columns by 6 rows; a
# terminal below that still gets its status line on the last row.
TINY = (20, 8)


def _open_goto_line(s):
    s.key(ALT.SEARCH, delay=0.4)
    s.key("g", delay=0.6)


def _open_find(s):
    s.key(ALT.SEARCH, delay=0.4)
    s.key("f", delay=0.6)


def _open_not_found_popup(s):
    """Search for text the buffer does not contain: the "String NOT found"
    message box with its single Ok button."""
    _open_find(s)
    s.key("ZZQQ", delay=0.3)
    s.key("\r", delay=0.8)


def _dismiss(s):
    s.key(ESC, delay=0.6)


def _row_of(rows, needle):
    """Index of the first row containing ``needle``, or None."""
    return next((i for i, r in enumerate(rows) if needle in r), None)


def _frame(rows, title):
    """(top, bottom, left, right) of the dialog frame whose top border carries
    ``title``: the corners around the title and the bottom-left corner below."""
    top = _row_of(rows, title)
    assert top is not None, "no %r dialog on screen:\n%s" % (title, "\n".join(rows))
    at = rows[top].index(title)
    left = rows[top].rfind(CORNER_TL, 0, at)
    right = rows[top].find(CORNER_TR, at)
    bottom = next((i for i in range(top + 1, len(rows)) if rows[i][left] == CORNER_BL), -1)
    assert left >= 0 and right > left and bottom > top, \
        "%r frame not intact (%d,%d,%d,%d):\n%s" % (title, top, bottom, left, right, "\n".join(rows))
    return top, bottom, left, right


def _assert_status_on_last_row(rows, cols, nrows):
    """The status line moves with every resize; at the old size it is either
    off the screen (shrink) or stranded above the new last row (grow)."""
    assert _row_of(rows, STATUS_TEXT) == nrows - 1, \
        "status line is not on row %d after the resize to %dx%d:\n%s" % (
            nrows - 1, cols, nrows, "\n".join(rows))


def _assert_box_inside(frame, cols, nrows, title, rows):
    top, bottom, left, right = frame
    assert top > MENU_BAR_ROW and bottom < nrows - 1 and left >= 0 and right < cols, \
        "%r box (%d,%d,%d,%d) is not inside the %dx%d screen:\n%s" % (
            title, top, bottom, left, right, cols, nrows, "\n".join(rows))


def _assert_box_centred(frame, cols, title, rows):
    _, _, left, right = frame
    assert abs((left + right) / 2.0 - (cols - 1) / 2.0) <= CENTRE_SLACK, \
        "%r box columns %d..%d are not centred on %d columns:\n%s" % (
            title, left, right, cols, "\n".join(rows))


def _assert_box_clamped_to_corner(frame, title, rows):
    top, _, left, _ = frame
    assert (top, left) == CORNER_CLAMP, \
        "%r box top-left is at (%d,%d), not clamped to %r:\n%s" % (
            title, top, left, CORNER_CLAMP, "\n".join(rows))


def _assert_dialog_placed_for(s, cols, nrows, title):
    """After a resize the box has been placed again for the new screen: it
    lies inside it, centred when it fits and clamped into the corner when it
    does not, with the status line on the new last row."""
    rows = s.display()
    _assert_status_on_last_row(rows, cols, nrows)
    frame = _frame(rows, title)
    _assert_box_inside(frame, cols, nrows, title, rows)
    top, bottom, left, right = frame
    if right - left + 1 < cols and bottom - top + 1 < nrows - 2:
        _assert_box_centred(frame, cols, title, rows)
    else:
        _assert_box_clamped_to_corner(frame, title, rows)
    return frame


def _assert_dialog_closed(s, title):
    rows = s.display()
    assert _row_of(rows, title) is None, \
        "Esc did not close the %r dialog:\n%s" % (title, "\n".join(rows))


class TestDialogResizeSurvival:
    """A modal dialog follows the screen through a resize."""

    # @scenario SC-dialog-engine-21
    def test_goto_line_resize_shrink(self, tmp_path):
        with WpeSession(str(tmp_path), SEED) as s:
            _open_goto_line(s)
            before = _frame(s.display(), GOTO_TITLE)
            s.resize(40, 15, delay=0.8)
            assert s.alive(), "wpe died on the shrink with Goto Line open"
            after = _assert_dialog_placed_for(s, 40, 15, GOTO_TITLE)
            _dismiss(s)
            _assert_dialog_closed(s, GOTO_TITLE)
        assert after != before, "the box did not move on the shrink: %r" % (after,)

    def test_goto_line_resize_grow(self, tmp_path):
        with WpeSession(str(tmp_path), SEED, cols=40, rows=15) as s:
            _open_goto_line(s)
            before = _frame(s.display(), GOTO_TITLE)
            s.resize(120, 40, delay=0.8)
            assert s.alive(), "wpe died on the grow with Goto Line open"
            after = _assert_dialog_placed_for(s, 120, 40, GOTO_TITLE)
            _dismiss(s)
            _assert_dialog_closed(s, GOTO_TITLE)
        assert after != before, "the box did not move on the grow: %r" % (after,)

    # @scenario SC-dialog-engine-28
    def test_goto_line_resize_multiple(self, tmp_path):
        with WpeSession(str(tmp_path), SEED) as s:
            _open_goto_line(s)
            for cols, rows in [(40, 15), (100, 40), (30, 10), (80, 30)]:
                s.resize(cols, rows, delay=0.8)
                assert s.alive(), "wpe died at the resize to %dx%d" % (cols, rows)
                _assert_dialog_placed_for(s, cols, rows, GOTO_TITLE)
            _dismiss(s)
            _assert_dialog_closed(s, GOTO_TITLE)

    def test_search_dialog_resize(self, tmp_path):
        # The 57x16 Find box does not fit a 40x15 screen: it is clamped into
        # the corner and its right border closes on the new last column.
        with WpeSession(str(tmp_path), SEED) as s:
            _open_find(s)
            _frame(s.display(), FIND_TITLE)
            s.resize(40, 15, delay=0.8)
            assert s.alive(), "wpe died on the shrink with Find open"
            _assert_dialog_placed_for(s, 40, 15, FIND_TITLE)
            _dismiss(s)
            _assert_dialog_closed(s, FIND_TITLE)

    def test_error_popup_resize(self, tmp_path):
        # The "String NOT found" message box is the one-button popup.
        with WpeSession(str(tmp_path), SEED) as s:
            _open_not_found_popup(s)
            assert _row_of(s.display(), NOT_FOUND_TEXT) is not None, \
                "the not-found popup did not open:\n" + "\n".join(s.display())
            s.resize(40, 15, delay=0.8)
            assert s.alive(), "wpe died on the shrink with the popup open"
            _assert_dialog_placed_for(s, 40, 15, NOT_FOUND_TITLE)
            _dismiss(s)
            _assert_dialog_closed(s, NOT_FOUND_TITLE)

    def test_resize_then_dialog(self, tmp_path):
        with WpeSession(str(tmp_path), SEED) as s:
            s.resize(40, 15, delay=0.8)
            _assert_status_on_last_row(s.display(), 40, 15)
            _open_goto_line(s)
            assert s.alive(), "wpe died opening Goto Line after the resize"
            _assert_dialog_placed_for(s, 40, 15, GOTO_TITLE)
            _dismiss(s)
            _assert_dialog_closed(s, GOTO_TITLE)

    def test_aggressive_shrink(self, tmp_path):
        # Below the 30x6 minimum the desk is drawn for 30 columns into a
        # narrower terminal, but the rows are real: the status line lands on
        # the last of the 8.
        cols, rows = TINY
        with WpeSession(str(tmp_path), SEED) as s:
            _open_goto_line(s)
            s.resize(cols, rows, delay=0.8)
            assert s.alive(), "wpe did not survive the shrink to %dx%d" % (cols, rows)
            screen = s.display()
            _assert_status_on_last_row(screen, cols, rows)
            assert _row_of(screen, GOTO_TITLE) == 1, \
                "the Goto Line box is not clamped under the menu bar at %dx%d:\n%s" % (
                    cols, rows, "\n".join(screen))
            _dismiss(s)
            assert _row_of(s.display(), GOTO_TITLE) is None, \
                "Esc did not close the box at %dx%d:\n%s" % (cols, rows, "\n".join(s.display()))

    def test_no_crash_editor_after_resize(self, tmp_path):
        with WpeSession(str(tmp_path), SEED) as s:
            _open_goto_line(s)
            s.resize(40, 15, delay=0.8)
            _assert_status_on_last_row(s.display(), 40, 15)
            _dismiss(s)
            _assert_dialog_closed(s, GOTO_TITLE)
            s.resize(80, 30, delay=0.8)
            _assert_status_on_last_row(s.display(), 80, 30)
            s.key("hello", delay=0.3)
            s.key(F2_SAVE, delay=0.6)
            saved = s.text()
        assert saved.startswith("hello"), \
            "the editor did not take input after the resize cycle: %r" % saved

    def test_resize_leaves_no_stale_focused_widget(self, tmp_path):
        with WpeSession(str(tmp_path), SEED, cols=40, rows=15) as s:
            _open_goto_line(s)
            s.key("7", delay=0.3)
            s.resize(120, 40, delay=0.8)
            rows = s.display()
            top, bottom, left, right = _assert_dialog_placed_for(s, 120, 40, GOTO_TITLE)
            _dismiss(s)
        stale = [(y, x) for y, row in enumerate(rows) for x, ch in enumerate(row)
                 if ch == "7" and not (top <= y <= bottom and left <= x <= right)]
        assert not stale, \
            "the field value lingers outside the box at %r:\n%s" % (stale, "\n".join(rows))


class TestEditorResizeNoCrash:
    """The bare desk follows the screen through a resize."""

    def _assert_desk_relaid_out(self, s, cols, nrows):
        rows = s.display()
        _assert_status_on_last_row(rows, cols, nrows)
        assert rows[1][cols - 1] == CORNER_TR, \
            "editor frame is not closed on column %d at %dx%d:\n%s" % (
                cols - 1, cols, nrows, "\n".join(rows))

    def test_plain_resize_shrink(self, tmp_path):
        with WpeSession(str(tmp_path), SEED) as s:
            s.resize(40, 15, delay=0.8)
            assert s.alive()
            self._assert_desk_relaid_out(s, 40, 15)

    def test_plain_resize_grow(self, tmp_path):
        with WpeSession(str(tmp_path), SEED, cols=40, rows=15) as s:
            s.resize(120, 40, delay=0.8)
            assert s.alive()
            self._assert_desk_relaid_out(s, 120, 40)

    def test_rapid_resize_burst(self, tmp_path):
        with WpeSession(str(tmp_path), SEED) as s:
            for cols, rows in [
                (60, 20), (40, 15), (30, 10), (50, 20),
                (80, 30), (100, 40), (40, 12), (80, 30)
            ]:
                s.resize(cols, rows, delay=0.3)
            assert s.alive(), "wpe must survive a resize storm"
            s._drain(0.8)
            self._assert_desk_relaid_out(s, 80, 30)
