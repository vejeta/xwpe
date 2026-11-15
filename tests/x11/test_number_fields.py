"""The number fields of Options > Editor in the graphical editor (X11 and Wayland).

Tabstops is a number the editor divides by, so 0 in its field is not a
preference: it ended the editor at the first tab on screen and lost what was
unsaved.  The field now refuses a number outside 1 to 99 where it was typed:
the range is said on the bottom row, the box stays open and the editor lives.
The terminal rows (tests/test_number_fields.py) read the screen as text; these
rows read it as pixels, because the dialog engine, the accept loop and the
bottom row are shared by every backend and only a drawn frame shows that the
graphical backends hold the same behaviour.

Every row starts the editor on a file with a tab, so that Tabstops 0 is a
death and not a number nobody reads, and tells a closed box from an open one
by how many pixels differ from the editor before the box was opened.  This
module is also run, unchanged, by tests/wayland/test_number_fields.py.
"""
import pytest

pytest.importorskip("PIL")  # python3-pil; skip cleanly if absent
from conftest import changed_pixels

SEED = "int main(void)\n{\n\treturn 0;\n}\n"
WITH_A_TAB = {"XWPE_TEST_SOURCE": SEED}
with_a_tab = pytest.mark.parametrize("xwpe", [WITH_A_TAB], indirect=True)

# The box covers well over this many pixels; a closed box leaves at most the
# few text pixels of a tab drawn over another number of columns.
BOX_PIXELS = 2000
BOX_GONE_PIXELS = 1000
# The bottom row of the 1024x768 window: one text row of the status bar.
STATUS_STRIP_HEIGHT = 24
# The least pixels a sentence on the bottom row changes.
SENTENCE_PIXELS = 100


def open_editor_options(xwpe):
    """Options menu (Alt-O) -> Editor (e)."""
    xwpe.key("alt+o", delay=0.7)
    xwpe.key("e", delay=1.0)


def type_tabstops(xwpe, digits):
    """Options > Editor, Alt-T to the Tabstops field, then @digits."""
    open_editor_options(xwpe)
    xwpe.key("alt+t", delay=0.5)
    for digit in digits:
        xwpe.key(digit, delay=0.3)


def bottom_strip(img):
    """The status row of the window, cut from a screenshot."""
    width, height = img.size
    return img.crop((0, height - STATUS_STRIP_HEIGHT, width, height))


def box_is_open(xwpe, editor):
    """True when the screen differs from the editor without the box by what a
    box covers."""
    return changed_pixels(editor, xwpe.screenshot()) > BOX_PIXELS


def box_is_gone(xwpe, editor):
    """True when the screen is the editor again, give or take the text of a
    tab drawn over another number of columns."""
    return changed_pixels(editor, xwpe.screenshot()) < BOX_GONE_PIXELS


def editor_lives(xwpe):
    """True while the editor process has not ended."""
    return xwpe.proc.poll() is None


# @scenario SC-number-fields-01x
# @scenario SC-number-fields-01w
@with_a_tab
def test_x11_tabstops_zero_is_refused_and_the_box_stays_open(xwpe):
    """Tabstops 0 divides the editor by zero: Enter on it is refused, the
    editor lives, the box is still on screen, and the bottom row now says the
    range.  Esc then closes the box and leaves the editor as it was."""
    editor = xwpe.screenshot()
    type_tabstops(xwpe, "0")
    before_enter = xwpe.screenshot()
    xwpe.key("Return", delay=1.0)
    assert editor_lives(xwpe), "xwpe died on Tabstops 0 (exit %s)" % xwpe.proc.poll()
    assert box_is_open(xwpe, editor), "the box closed on Tabstops 0"
    said = changed_pixels(bottom_strip(before_enter), bottom_strip(xwpe.screenshot()))
    assert said > SENTENCE_PIXELS, \
        "the bottom row did not change, so it does not say the range (%d px)" % said
    xwpe.key("Escape", delay=1.0)
    assert editor_lives(xwpe), "xwpe died after Esc"
    assert box_is_gone(xwpe, editor), "Esc did not close the box"


# @scenario SC-number-fields-C4x
# @scenario SC-number-fields-C4w
@with_a_tab
def test_x11_tabstops_four_is_taken_and_the_box_closes(xwpe):
    """A number inside the range is taken: Return on Tabstops 4 closes the box
    and the editor lives (the control of the refusal above)."""
    editor = xwpe.screenshot()
    type_tabstops(xwpe, "4")
    assert box_is_open(xwpe, editor), "the box did not open"
    xwpe.key("Return", delay=1.0)
    assert editor_lives(xwpe), "xwpe died on Tabstops 4 (exit %s)" % xwpe.proc.poll()
    assert box_is_gone(xwpe, editor), "the box stayed open on Tabstops 4"
