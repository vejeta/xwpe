"""X11: the mouse writes the cursor and the block on whole characters.

A Shift click moves one end of the block to the clicked cell and keeps the
other as the anchor; the end it moves depends on which side of the block the
click is on.  A scrollbar drag pulls a cursor that fell out of the new view
back into it, and compares the column the cursor is shown in with the view,
not its byte.  Both used to write the window's positions by hand; they go
through the buffer's owner now, and these rows pin what the user sees.

Geometry: the fixture pins the font (XWPE_FONT_SIZE=10), a cell is 8 by 15
pixels, and the first text row is centred at y = 45.  Each row says in its
docstring which cell it clicks.
"""
import pytest

CELL_W = 8
CELL_H = 15
TEXT_X0 = 12                # the centre of column 0
TEXT_Y0 = 45                # the centre of the first text row
H_THUMB = (220, 516)        # the horizontal scrollbar's thumb at rest

BLOCK_SOURCE = "aébécéd\n"          # columns: a é b é c é d
WIDE_SOURCE = "é" * 150 + "\n"      # 150 columns, 300 bytes


def cell(col, line=0):
    """The pixel at the centre of a text cell."""
    return TEXT_X0 + CELL_W * col, TEXT_Y0 + CELL_H * line


@pytest.fixture
def wide_rows(tmp_path):
    """Rows that may be wider than the window, so it has a horizontal bar."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True)
    (cfg / "xwperc").write_text("[General]\nMaxColumn : 500\n")


def mark_block(xwpe, first, last):
    """Mark the block from column `first` to column `last` of the first row
    with Ctrl-K B and Ctrl-K K, the cursor ending on `last`."""
    xwpe.key("Home", delay=0.2)
    for _ in range(first):
        xwpe.key("Right", delay=0.15)
    xwpe.key("ctrl+k", delay=0.2)
    xwpe.key("b", delay=0.2)
    for _ in range(last - first):
        xwpe.key("Right", delay=0.15)
    xwpe.key("ctrl+k", delay=0.2)
    xwpe.key("k", delay=0.2)


def shift_click_then_delete_block(xwpe, col):
    """A Shift click on a column of the first row, then Ctrl-K Y: what the
    block held goes, and the file says which characters it held."""
    px, py = cell(col)
    xwpe.click(px, py, delay=0.5, hold="shift")
    xwpe.key("ctrl+k", delay=0.3)
    xwpe.key("y", delay=0.3)
    xwpe.save()
    return xwpe.saved_text()


# @scenario SC-edit-model-53
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCK_SOURCE}], indirect=True)
def test_shift_click_behind_the_block_moves_its_end(xwpe):
    """The block is `bé` (columns 2 to 4).  A Shift click on the `d` (column
    6) moves its end there: the block is `bécé`, and Ctrl-K Y leaves `aéd`."""
    mark_block(xwpe, 2, 4)
    assert shift_click_then_delete_block(xwpe, 6) == "aéd\n"


# @scenario SC-edit-model-53
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCK_SOURCE}], indirect=True)
def test_shift_click_before_the_block_moves_its_begin(xwpe):
    """The same block with the cursor back on its begin (column 2); a Shift
    click on the first `é` (column 1) moves the begin there: the block is
    `ébé`, and Ctrl-K Y leaves `acéd`."""
    mark_block(xwpe, 2, 4)
    xwpe.key("Left", delay=0.2)
    xwpe.key("Left", delay=0.2)
    assert shift_click_then_delete_block(xwpe, 1) == "acéd\n"


# @scenario SC-edit-model-53
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCK_SOURCE}], indirect=True)
def test_shift_click_inside_the_block_from_its_end_moves_the_end(xwpe):
    """The cursor is on the block's end (column 4) when a Shift click lands
    on the `é` inside it (column 3): the end side was nearer, so the block
    runs from that `é` to the end, and Ctrl-K Y leaves `aébcéd`."""
    mark_block(xwpe, 2, 4)
    assert shift_click_then_delete_block(xwpe, 3) == "aébcéd\n"


# @scenario SC-edit-model-53
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": BLOCK_SOURCE}], indirect=True)
def test_shift_click_inside_the_block_from_its_begin_moves_the_begin_side(xwpe):
    """The cursor is back on the block's begin (column 2) when the Shift click
    lands on the `é` inside it (column 3): the block runs from its begin to
    that `é`, and Ctrl-K Y leaves `aéécéd`."""
    mark_block(xwpe, 2, 4)
    xwpe.key("Left", delay=0.2)
    xwpe.key("Left", delay=0.2)
    assert shift_click_then_delete_block(xwpe, 3) == "aéécéd\n"


# @scenario SC-edit-model-53
@pytest.mark.parametrize("xwpe", [{"XWPE_TEST_SOURCE": WIDE_SOURCE}], indirect=True)
def test_a_scrollbar_drag_keeps_a_cursor_that_is_still_in_view(wide_rows, xwpe):
    """The cursor stands on column 100 of a row of 150 accented letters (200
    bytes before it).  A short drag of the horizontal thumb moves the view a
    little to the right; column 100 is still inside it, so the cursor stays,
    and a letter typed goes in at column 100.  Compared by bytes, 200 would
    be taken for a position far past the view and the cursor sent to its left
    edge."""
    px, py = cell(100)
    xwpe.click(px, py, delay=0.4)
    xwpe.drag(H_THUMB[0], H_THUMB[1], H_THUMB[0] + 60, H_THUMB[1])
    xwpe.type("Z")
    xwpe.save()
    text = xwpe.saved_text()
    assert xwpe.proc.poll() is None, "xwpe died during the scrollbar drag"
    assert text.index("Z") == 100, \
        "the drag took the cursor from column 100 to column %d" % text.index("Z")
