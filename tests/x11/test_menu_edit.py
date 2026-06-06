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


# --- shortcut path (#159): same operations via the advertised accelerator ---
# The X11 keyboard path (we_xterm.c) decodes Ctrl-/Alt- separately from ncurses,
# so an accelerator can be dead under xwpe while working under wpe (and vice
# versa).  This is exactly where the Alt-F3 close and Alt-U Run breaks lived.

def test_cut_via_ctrl_x(xwpe):
    """^X cuts the marked block."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.key("ctrl+x")           # Cut
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on ^X"
    assert xwpe.saved_text().strip() == "", \
        "^X should cut the whole buffer, got %r" % xwpe.saved_text()


def test_copy_paste_via_ctrl_c_ctrl_v(xwpe):
    """^C then ^V duplicates the marked block."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.key("ctrl+c")           # Copy
    xwpe.key("ctrl+v")           # Paste
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on ^C/^V"
    assert _count(xwpe.saved_text()) == 2, \
        "^C then ^V should duplicate the block, got %r" % xwpe.saved_text()


def test_undo_via_ctrl_u(xwpe):
    """^U undoes a block delete."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.menu("e", "d")          # Delete
    xwpe.key("ctrl+u")           # Undo
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on ^U"
    assert _count(xwpe.saved_text()) == 1, \
        "^U should restore the deleted block, got %r" % xwpe.saved_text()


def test_redo_via_ctrl_r(xwpe):
    """^R re-applies an undone delete."""
    xwpe.menu("b", "o")          # Mark WhOle
    xwpe.menu("e", "d")          # Delete
    xwpe.key("ctrl+u")           # Undo -> restored
    xwpe.key("ctrl+r")           # Redo -> deleted again
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on ^R"
    assert xwpe.saved_text().strip() == "", \
        "^R should re-apply the delete, got %r" % xwpe.saved_text()


def test_show_buffer_via_alt_y(xwpe):
    """Alt-Y opens the Show Buffer window (the accelerator dispatches in X11).

    We open it from a clean editor (no prior copy): an empty Buffer window
    drawn over the editor text changes the screen clearly.  Asserting on a pixel
    delta -- not on the exact count -- because the buffer mirrors the source
    when it holds a copy (its content looks like the editor underneath), which
    is why a copy-first variant is a poor signal.  A dead accelerator leaves the
    screen unchanged."""
    from conftest import changed_pixels
    base = xwpe.screenshot()
    xwpe.key("alt+y", delay=1.0)     # Show Buffer
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Y"
    assert changed_pixels(base, after) > 800, \
        "Alt-Y should open the Show Buffer window (screen should change), " \
        "changed=%d" % changed_pixels(base, after)
