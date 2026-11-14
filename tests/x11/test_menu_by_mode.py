"""The menus by key style and block mode, on the graphical display.

The terminal rows (tests/test_menu_golden.py, tests/test_menu_record.py) read
the menus from the screen and from the editor's state.  A graphical display has
no text screen and these rows have no state channel, so they press the keys and
read what the menus did, in the file on disk: Block > Mark Word is there with
the Modern keys and in the Selection block mode and not in the Borland keys
with anchor blocks, and a menu item that finds nothing to act on says so
without changing the file.  The Wayland twin runs this body under its own
fixture.
"""
import pytest

from conftest import _sleep

MODERN = {"XWPE_KEYS": "modern"}
FIRST_WORD = "int"
FIRST_LINE_AFTER_WORD = " main(){"


def _first_line_after(xwpe, keys):
    """Press @keys, delete the marked block with Ctrl-K Y, save, and return
    the file's first line."""
    for k in keys:
        xwpe.key(k)
    xwpe.key("ctrl+k")
    xwpe.key("y")
    xwpe.save()
    return xwpe.saved_text().splitlines()[0]


# @scenario SC-menus-40
@pytest.mark.parametrize("xwpe", [MODERN], indirect=True)
def test_alt_b_k_marks_the_word_with_the_modern_keys(xwpe):
    """Alt-B K marks the word under the cursor: deleting the block takes it."""
    line = _first_line_after(xwpe, ["alt+b", "k"])
    assert xwpe.proc.poll() is None, "xwpe died on Alt-B K"
    assert line == FIRST_LINE_AFTER_WORD, \
        "Alt-B K should mark %r and Ctrl-K Y delete it, the first line is %r" % (FIRST_WORD, line)


# @scenario SC-menus-41
def test_alt_b_k_is_no_row_with_the_borland_keys(xwpe):
    """Positive control: the Borland keys with anchor blocks have no Mark Word
    row, so the letter marks no word and the first line is as it was."""
    line = _first_line_after(xwpe, ["alt+b", "k"])
    assert xwpe.proc.poll() is None
    assert line.startswith(FIRST_WORD), line


# @scenario SC-menus-42
def test_alt_b_o_still_marks_the_whole_text(xwpe):
    """Positive control kept from before the table: Mark WhOle is Alt-B O."""
    xwpe.key("alt+b")
    xwpe.key("o")
    xwpe.key("ctrl+k")
    xwpe.key("y")
    xwpe.save()
    assert xwpe.proc.poll() is None
    assert xwpe.saved_text().strip() == ""


# @scenario SC-menus-43
def test_cut_with_nothing_marked_changes_nothing(xwpe):
    """Edit > Cut with no block says so on the status row and leaves the file
    as it was."""
    before = open(xwpe.srcfile).read()
    xwpe.key("alt+e")
    xwpe.key("t")
    xwpe.save()
    assert xwpe.proc.poll() is None
    assert xwpe.saved_text() == before


# @scenario SC-menus-44
def test_undo_with_nothing_to_undo_opens_no_box(xwpe):
    """Ctrl-U with nothing to undo says so on the status row: no box takes the
    next key, so the first typed letter goes into the text."""
    xwpe.key("ctrl+u")
    xwpe.key("Z")
    xwpe.save()
    assert xwpe.proc.poll() is None
    assert xwpe.saved_text().splitlines()[0].startswith("Zint"), xwpe.saved_text()[:40]
