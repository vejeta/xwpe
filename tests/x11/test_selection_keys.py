"""Alt-Insert, Alt-Delete and the dialog field's Alt-paste, on every display.

Alt-Delete copies the marked block and Alt-Insert pastes it, in the editor and
in a dialog's text field (Alt-Insert, or a middle click with Alt held).  Both
go through the one clipboard path every display fills (the OS selection where
the display has one, else the editor's own clipboard), so no display has a
copy of the keys of its own.  The rows here copy inside the editor and read the
effect, so they need no external tool and run wherever the fixtures run; the
rows with an external selection owner live in test_clipboard.py with their
tool skips.

The same body runs under the native-Wayland fixture (tests/wayland/).
"""
import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent
from PIL import ImageChops

from conftest import changed_pixels

# The Search > Find dialog: its size in cells, and its "Text to Find:" field.
FIND_COLS, FIND_ROWS = 57, 16
FIELD_ROW, FIELD_FIRST_COL, FIELD_COLS = 2, 18, 35
# A field redrawn with the same text differs by no more than a cursor.
SAME_FIELD_PIXELS = 60
# Pixel tolerance of a cell size: a whole number of pixels, within this.
CELL_TOLERANCE = 0.25


def _longest_run(counts, minimum=100):
    best, start = None, None
    for i, n in enumerate(counts + [0]):
        if n >= minimum and start is None:
            start = i
        elif n < minimum and start is not None:
            if best is None or (i - start) > (best[1] - best[0] + 1):
                best = (start, i - 1)
            start = None
    return best


def _cell_size(pixels, cells, what):
    size = pixels / float(cells)
    rounded = int(round(size))
    assert abs(size - rounded) <= CELL_TOLERANCE, (
        "%s: %d px is not a whole number of %d cells" % (what, pixels, cells))
    return rounded


class FindDialog:
    """Where the Find dialog's text field is on screen."""

    def __init__(self, before, after):
        diff = ImageChops.difference(before, after).convert("L")
        mask = diff.point(lambda p: 255 if p > 40 else 0)
        w, h = mask.size
        cols = _longest_run([mask.crop((x, 0, x + 1, h)).histogram()[255]
                             for x in range(w)])
        rows = _longest_run([mask.crop((0, y, w, y + 1)).histogram()[255]
                             for y in range(h)])
        assert cols and rows, "the Find dialog did not appear"
        self.cell_w = _cell_size(cols[1] - cols[0] + 1, FIND_COLS, "dialog width")
        self.cell_h = _cell_size(rows[1] - rows[0] + 1, FIND_ROWS, "dialog height")
        self.x0, self.y0 = cols[0], rows[0]

    def field_box(self):
        """The pixel box of the field's row, label and field together."""
        left = self.x0 + FIELD_FIRST_COL * self.cell_w
        top = self.y0 + FIELD_ROW * self.cell_h
        return (left, top, left + FIELD_COLS * self.cell_w, top + self.cell_h)

    def field_middle(self):
        """A pixel inside the field, clear of its border cells."""
        left, top, right, bottom = self.field_box()
        return ((left + right) // 2, (top + bottom) // 2)


def open_find(xwpe):
    """Search > Find; returns the dialog's geometry."""
    before = xwpe.screenshot()
    xwpe.key("alt+s", delay=0.7)
    xwpe.key("f", delay=1.0)
    return FindDialog(before, xwpe.screenshot())


def close_dialog(xwpe):
    xwpe.key("Escape", delay=0.6)


def field_image(xwpe, dialog):
    return xwpe.screenshot().crop(dialog.field_box())


def copy_two_lines(xwpe):
    """Mark the first two lines whole and Copy them (Ctrl-Insert): the editor's
    clipboard then holds "int main(){" and "  int a = 1;".  Two lines, so the
    Find dialog is not seeded with the marked text."""
    xwpe.key("ctrl+k"); xwpe.key("b")
    xwpe.key("Down"); xwpe.key("Down")
    xwpe.key("ctrl+k"); xwpe.key("k")
    xwpe.key("ctrl+Insert", delay=0.6)


def first_line(xwpe):
    xwpe.save()
    return xwpe.saved_text().splitlines()[0]


# @scenario SC-display-02
def test_alt_insert_pastes_and_alt_delete_copies_in_the_editor(xwpe):
    """"int" marked, Alt-Delete, End, Alt-Insert gives "int main(){int" on the
    first line and the editor stays alive.  Alt-Delete copies the block and
    Alt-Insert pastes it, on every display."""
    xwpe.key("ctrl+k"); xwpe.key("b")
    xwpe.key("Right"); xwpe.key("Right"); xwpe.key("Right")
    xwpe.key("ctrl+k"); xwpe.key("k")
    xwpe.key("alt+Delete", delay=0.6)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Delete"
    xwpe.key("End", "alt+Insert", delay=0.6)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Insert"
    line = first_line(xwpe)
    assert line == "int main(){int", \
        "Alt-Delete should copy the block and Alt-Insert paste it, the first " \
        "line is %r" % line


# @scenario SC-display-02
def test_alt_insert_after_copying_whole_lines_does_not_join_the_next_line(xwpe):
    """Two whole lines copied with Ctrl-Insert and pasted with Alt-Insert land
    as two whole lines before the line the cursor is on, as Ctrl-V pastes them.
    The X11 key used to rebuild the text from the selection cache, which has
    lost the last line break, and so glued the second line to the next one."""
    copy_two_lines(xwpe)
    xwpe.key("alt+Insert", delay=0.6)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Insert"
    xwpe.save()
    lines = xwpe.saved_text().splitlines()
    assert lines[:5] == ["int main(){", "  int a = 1;",
                         "int main(){", "  int a = 1;", "  int b = 2;"], \
        "the pasted lines should stay whole lines, got %r" % lines[:5]


# @scenario SC-display-03
def test_alt_insert_in_a_dialog_field_pastes_the_clipboard(xwpe):
    """A copied line, Search > Find, Alt-Insert in the "Text to Find:" field:
    the field shows what Ctrl-V puts there, and is not empty."""
    copy_two_lines(xwpe)
    dialog = open_find(xwpe)
    empty = field_image(xwpe, dialog)
    xwpe.key("ctrl+v", delay=0.6)
    by_ctrl_v = field_image(xwpe, dialog)
    close_dialog(xwpe)
    assert changed_pixels(empty, by_ctrl_v) > SAME_FIELD_PIXELS, \
        "Ctrl-V put nothing in the field: the row cannot tell the paste"
    open_find(xwpe)
    xwpe.key("alt+Insert", delay=0.6)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Insert in a field"
    by_alt_insert = field_image(xwpe, dialog)
    close_dialog(xwpe)
    assert changed_pixels(by_ctrl_v, by_alt_insert) < SAME_FIELD_PIXELS, \
        "Alt-Insert should paste the clipboard into the field like Ctrl-V"


# @scenario SC-display-03
def test_alt_click_in_a_dialog_field_pastes_the_clipboard(xwpe):
    """A copied line, Search > Find, a middle click with Alt held inside the
    "Text to Find:" field: the field shows what Ctrl-V puts there."""
    copy_two_lines(xwpe)
    dialog = open_find(xwpe)
    empty = field_image(xwpe, dialog)
    xwpe.key("ctrl+v", delay=0.6)
    by_ctrl_v = field_image(xwpe, dialog)
    close_dialog(xwpe)
    assert changed_pixels(empty, by_ctrl_v) > SAME_FIELD_PIXELS, \
        "Ctrl-V put nothing in the field: the row cannot tell the paste"
    open_find(xwpe)
    px, py = dialog.field_middle()
    xwpe.click(px, py, button=2, hold="alt", delay=0.6)
    assert xwpe.proc.poll() is None, "xwpe died on an Alt middle click in a field"
    by_click = field_image(xwpe, dialog)
    close_dialog(xwpe)
    assert changed_pixels(by_ctrl_v, by_click) < SAME_FIELD_PIXELS, \
        "an Alt middle click should paste the clipboard into the field"
