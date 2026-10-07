"""ncurses: resizing the terminal re-lays-out the desk, Messages window included.

The terminal resize path -- SIGWINCH -> resizeterm -> KEY_RESIZE ->
e_relayout_windows -- had NO test coverage, and that is exactly where the
Messages-window layout regression hid for weeks (nobody drove a resize).  This
opens the Messages window (Execute Make), sweeps the terminal size down and back
up several times through WpeSession.resize(), and after EVERY step asserts the
layout that only a relayout produces at the new size: the status line on the
new last row, the editor frame closed on the new last column, and the Messages
window still present, docked on the bottom edge and as wide as the screen.

Those assertions are what makes the test a guard: a build whose KEY_RESIZE
handling does nothing stays alive and keeps "Messages" on screen, but leaves
the status line and the borders at the old size.
"""
from wpe_driver import WpeSession

ALT_A = "\033a"   # Execute Make (programming mode) -> opens Messages
MARK = "MSG_RESIZE_MARKER_OK"
STATUS_TEXT = "F1 Help"
MESSAGES_TITLE = "Messages"
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as k (top-right) and m (bottom-left).
CORNER_TR, CORNER_BL = "k", "m"
# The menu bar is row 0 and the status line the last row, so a window docked
# on the bottom edge ends on the row above the status line.
BOTTOM_DOCK_OFFSET = 2
SIZE_SWEEP = [(60, 16), (95, 34), (55, 14), (100, 36), (72, 20)]


def _row_of(rows, needle):
    """Index of the first row containing ``needle``, or None."""
    return next((i for i, r in enumerate(rows) if needle in r), None)


def _assert_desk_relaid_out(rows, cols, nrows):
    """The two desk landmarks that move with every resize: the status line
    sits on the new last row and the editor frame's top-right corner on the
    new last column.  Both stay at the old size when no relayout happened."""
    joined = "\n".join(rows)
    assert _row_of(rows, STATUS_TEXT) == nrows - 1, \
        "status line is not on row %d after the resize to %dx%d:\n%s" % (
            nrows - 1, cols, nrows, joined)
    assert rows[1][cols - 1] == CORNER_TR, \
        "editor frame is not closed on column %d after the resize to %dx%d:\n%s" % (
            cols - 1, cols, nrows, joined)


def _assert_messages_docked(rows, cols, nrows):
    """The Messages window is still on screen, spans the new width (its
    title row ends with the top-right corner on the last column) and is
    docked on the bottom edge (bottom-left corner on the row above the
    status line)."""
    joined = "\n".join(rows)
    title = _row_of(rows, MESSAGES_TITLE)
    assert title is not None, \
        "Messages window vanished after the resize to %dx%d:\n%s" % (cols, nrows, joined)
    assert rows[title][cols - 1] == CORNER_TR, \
        "Messages window does not reach column %d at %dx%d:\n%s" % (cols - 1, cols, nrows, joined)
    assert rows[nrows - BOTTOM_DOCK_OFFSET][0] == CORNER_BL, \
        "Messages window is not docked on row %d at %dx%d:\n%s" % (
            nrows - BOTTOM_DOCK_OFFSET, cols, nrows, joined)


def test_ncurses_resize_keeps_messages(tmp_path):
    (tmp_path / "Makefile").write_text("all:\n\t@echo %s\n" % MARK)
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="t.c", wait=1.5) as w:
        w.key(ALT_A, delay=3.0)
        assert MESSAGES_TITLE in "\n".join(w.display()), "Messages window did not open"

        # A few shrink/grow cycles through modest sizes, driving KEY_RESIZE each
        # time.  xwpe must never die, and after each step the desk and the
        # Messages window must be laid out for the new size.
        for cols, rows in SIZE_SWEEP:
            w.resize(cols, rows, delay=0.8)
            assert w.alive(), "xwpe died on resize to %dx%d" % (cols, rows)
            screen = w.display()
            _assert_desk_relaid_out(screen, cols, rows)
            _assert_messages_docked(screen, cols, rows)
