"""Edit-menu (Alt-E) tests for xwpe NATIVE WAYLAND -- the twin of
tests/x11/test_menu_edit.py (Xlib backend) and tests/test_menu_edit.py (wpe /
ncurses).

Running the SAME operations under all three front-ends is deliberate: the
Wayland keyboard path (we_wayland.c: wl_keyboard + xkbcommon -> keysym_to_xwpe)
decodes input separately from both ncurses and Xlib, so a behaviour can diverge.
These drive the editor through the menus and assert on the file written to disk
(ground truth); one asserts on a screen-pixel delta from xwpe's own wl_shm frame
dump.  The `xwpe` fixture (this directory's conftest) brings up a native
wl_surface under headless weston."""


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


# --- shortcut path: same operations via the advertised accelerator ---
# The Wayland keyboard path decodes Ctrl-/Alt- separately, so an accelerator can
# be dead under one backend while working under another.

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


def test_show_buffer_via_alt_y(xwpe):
    """Alt-Y opens the Show Buffer window (a clear screen change).

    Asserts on a pixel delta from xwpe's own wl_shm frame dump -- a dead
    accelerator leaves the screen unchanged."""
    from conftest import changed_pixels
    base = xwpe.screenshot()
    xwpe.key("alt+y", delay=1.0)     # Show Buffer
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Y"
    assert changed_pixels(base, after) > 800, \
        "Alt-Y should open the Show Buffer window (screen should change), " \
        "changed=%d" % changed_pixels(base, after)
