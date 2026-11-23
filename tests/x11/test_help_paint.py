"""A help page and a syntax-coloured source are painted by the graphical editor.

Twin of tests/test_help_paint.py: the painters of a help page (its links, its
file links, the end of the page) and of a source with syntax colours read each
row of the text through the readers of we_rowread.h, and the graphical painter
also draws a box round a link and round the text of a file link, which the
terminal painter never does.  The terminal rows read the cells back; here the
text is pixels, so the rows read that the editor is alive after each step,
that the help page is drawn (the screen changed), and that painting it a
second time gives the same picture.

The crafted pages are the nine of tests/help_pages.py, the pages the terminal
row drives, so every shape of a link the painter can meet is drawn here too,
box included.

The wayland twin runs this same body.
"""
import pytest

from conftest import _sleep, changed_pixels
from help_pages import PAGE_NAMES, PAGES, options_text, page_text

SETTLE_S = 1.5
SAME_PICTURE_PIXELS = 200       # a cursor's blink and no more
HELP_PIXELS = 3000
OPEN_S = 1.5
CLOSE_S = 1.0
PAGE_KEYS = ["Down", "Down", "Right", "Return", "Escape"]


def _page_params():
    """One fixture parameter per crafted page: the help file F1 opens, and the
    options that hold a long page row as one row."""
    params = []
    for name in PAGE_NAMES:
        own_row, maxcolumn = PAGES[name]
        params.append({"XWPE_TEST_HELP": page_text(own_row),
                       "XWPE_TEST_OPTIONS": options_text(maxcolumn)})
    return params


# @scenario SC-row-readers-05x
def test_x11_help_page_is_painted_twice_alike(xwpe):
    """F1 opens the help and a few steps through its links are drawn; closing
    it and opening it again draws the same page."""
    _sleep(SETTLE_S)
    source = xwpe.screenshot()
    xwpe.key("F1", delay=OPEN_S)
    first = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the help with F1"
    assert changed_pixels(source, first) > HELP_PIXELS, \
        "F1 should open the help viewer (screen barely changed)"
    for key in PAGE_KEYS:
        xwpe.key(key, delay=0.8)
        assert xwpe.proc.poll() is None, "xwpe died on %s in the help viewer" % key
    xwpe.key("Escape", delay=CLOSE_S)
    xwpe.key("F1", delay=OPEN_S)
    second = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the help a second time"
    assert changed_pixels(first, second) < SAME_PICTURE_PIXELS, \
        "the first page of the help was drawn differently the second time"


# @scenario SC-row-readers-05c
@pytest.mark.parametrize("xwpe", _page_params(), ids=PAGE_NAMES, indirect=True)
def test_x11_crafted_help_page_is_painted_twice_alike(xwpe, request, tmp_path):
    """Each crafted page is drawn and drawn the same twice: a file link gets
    its box under X11, and the loops that find the end of a link stop at the
    end of the row's text whatever the page holds."""
    page = request.node.callspec.id
    help_file = tmp_path / "help.xwpe"
    assert help_file.exists(), "the crafted page %s was not written" % page
    assert xwpe.owned.env.get("XWPE_LIB") == str(tmp_path), \
        "the editor of the page %s reads its help elsewhere" % page
    _sleep(SETTLE_S)
    source = xwpe.screenshot()
    xwpe.key("F1", delay=OPEN_S)
    first = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the page %s" % page
    assert changed_pixels(source, first) > HELP_PIXELS, \
        "F1 should open the page %s (screen barely changed)" % page
    xwpe.key("Escape", delay=CLOSE_S)
    xwpe.key("F1", delay=OPEN_S)
    second = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the page %s again" % page
    assert changed_pixels(first, second) < SAME_PICTURE_PIXELS, \
        "the page %s was drawn differently the second time" % page


# @scenario SC-row-readers-06x
def test_x11_source_with_syntax_colours_is_painted(xwpe):
    """The source the editor opens with is drawn in colours: typing at the end
    of a row and moving over it keeps the editor alive and the picture the
    same after the cursor comes back."""
    _sleep(SETTLE_S)
    before = xwpe.screenshot()
    for key in ["End", "Left", "Right", "Home", "Down", "End"]:
        xwpe.key(key, delay=0.5)
        assert xwpe.proc.poll() is None, "xwpe died on %s in a coloured source" % key
    xwpe.key("ctrl+Home", delay=0.8)
    after = xwpe.screenshot()
    assert changed_pixels(before, after) < SAME_PICTURE_PIXELS, \
        "the coloured source is drawn differently after the cursor moved over it"
