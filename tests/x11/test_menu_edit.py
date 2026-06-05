"""Edit-menu (Alt-E) tests for xwpe (X11/Xft) -- the twin of
tests/test_menu_edit.py for wpe.

Running the same operations under both front-ends is deliberate: the X11
keyboard/menu path (we_xterm.c) decodes input separately from ncurses, so a
behaviour can diverge between wpe and xwpe -- exactly how the keyboard ^K B
block-mode bug was found.  These assert on the file written to disk (ground
truth), driving the editor through the menus.

The shared `xwpe` fixture seeds a small C file; we assert on token counts so
the test does not depend on exact layout.
"""


def _count(text, token="return 0"):
    return text.count(token)


def test_cut_removes_the_block(xwpe):
    """Mark Whole + Cut empties the buffer."""
    xwpe.menu("b", "o")          # Block -> Mark WhOle
    xwpe.menu("e", "t")          # Edit  -> Cut
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Edit -> Cut"
    assert xwpe.saved_text().strip() == "", \
        "Cut of the whole buffer should empty it, got %r" % xwpe.saved_text()


def test_copy_then_paste_duplicates_the_block(xwpe):
    """Mark Whole + Copy + Paste leaves the text present twice."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.menu("e", "c")          # Copy
    xwpe.menu("e", "p")          # Paste
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Edit -> Copy/Paste"
    assert _count(xwpe.saved_text()) == 2, \
        "Copy then Paste should duplicate the block, got %r" % xwpe.saved_text()


def test_cut_then_paste_round_trip(xwpe):
    """Cut then Paste restores the text once."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.menu("e", "t")          # Cut
    xwpe.menu("e", "p")          # Paste
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Cut/Paste round trip"
    assert _count(xwpe.saved_text()) == 1, \
        "Cut then Paste should restore the text once, got %r" % xwpe.saved_text()


def test_undo_reverts_a_block_delete(xwpe):
    """Delete the whole buffer, then Undo restores it."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.menu("e", "d")          # Delete
    xwpe.menu("e", "u")          # Undo
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Edit -> Undo"
    assert _count(xwpe.saved_text()) == 1, \
        "Undo should restore the deleted block, got %r" % xwpe.saved_text()


def test_redo_reapplies_an_undone_delete(xwpe):
    """Delete, Undo (restores), Redo deletes again."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.menu("e", "d")          # Delete
    xwpe.menu("e", "u")          # Undo
    xwpe.menu("e", "r")          # Redo
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died during Edit -> Redo"
    assert xwpe.saved_text().strip() == "", \
        "Redo should re-apply the delete, got %r" % xwpe.saved_text()
