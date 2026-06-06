"""Edit-menu (Alt-E) regression tests for wpe (terminal mode).

Edit items and their bindings (we_menue.c):
  Cut    ^X  -> e_edt_del        Undo  ^U -> e_make_undo
  Copy   ^C  -> e_edt_copy       Redo  ^R -> e_make_redo
  Paste  ^V  -> e_edt_einf       Delete ^Del -> e_blck_del
  Show Buffer Alt-Y -> e_show_clipboard

Like test_block.py these drive the deterministic operations through
"Mark Whole" (Block -> Alt-B o) and assert on the file written to disk.
Behaviours we believe should hold but currently do not are marked with
@incoherence(...) so `pytest -rxX` lists them for manual review.

DOUBLE COVERAGE (#159): every item is exercised BOTH through the menu and
through its advertised KEYBOARD SHORTCUT.  The shortcut path is where X11/
terminal mapping bugs hid -- an item can work from the menu yet its advertised
accelerator be dead (e.g. the Alt-F3 close that the WM ate, or Run's Alt-U that
needed the e_prog_switch fallback).  A shortcut test fails if the accelerator
stops dispatching, independently of the menu working.
"""
from wpe_driver import WpeSession, ALT

# Advertised Edit accelerators (control codes / Meta), from we_menue.c.
CUT = "\x18"          # ^X
COPY = "\x03"         # ^C
PASTE = "\x16"        # ^V
UNDO = "\x15"         # ^U
REDO = "\x12"         # ^R
SHOW_BUFFER = "\033y"  # Alt-Y (emacs M-y)


def test_cut_removes_the_block(tmp_path):
    """Mark Whole + Cut empties the buffer (block goes to the clipboard)."""
    with WpeSession(str(tmp_path), "AAA\nBBB\nCCC\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "t")           # Cut
        w.save()
        assert w.alive(), "wpe died during Edit -> Cut"
        assert w.text().strip() == "", \
            "Cut of the whole buffer should empty it, got %r" % w.text()


def test_copy_then_paste_duplicates_the_block(tmp_path):
    """Mark Whole + Copy + Paste leaves the text present twice."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "c")           # Copy (block -> clipboard)
        w.menu(ALT.EDIT, "p")           # Paste (clipboard -> cursor)
        w.save()
        assert w.alive(), "wpe died during Edit -> Copy/Paste"
        assert w.text().count("AAA") == 2 and w.text().count("BBB") == 2, \
            "Copy then Paste should duplicate the block, got %r" % w.text()


def test_cut_then_paste_round_trip(tmp_path):
    """Cut then Paste restores the text (clipboard round trip)."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "t")           # Cut  -> buffer empty, clipboard holds it
        w.menu(ALT.EDIT, "p")           # Paste -> restore
        w.save()
        assert w.alive(), "wpe died during Cut/Paste round trip"
        assert w.text().count("AAA") == 1 and w.text().count("BBB") == 1, \
            "Cut then Paste should restore the text once, got %r" % w.text()


def test_undo_reverts_a_block_delete(tmp_path):
    """Delete the whole buffer, then Undo restores it."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "d")           # Delete
        w.menu(ALT.EDIT, "u")           # Undo
        w.save()
        assert w.alive(), "wpe died during Edit -> Undo"
        assert "AAA" in w.text() and "BBB" in w.text(), \
            "Undo should restore the deleted block, got %r" % w.text()


def test_redo_reapplies_an_undone_delete(tmp_path):
    """Delete, Undo (restores), Redo deletes again."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "d")           # Delete
        w.menu(ALT.EDIT, "u")           # Undo  -> restored
        w.menu(ALT.EDIT, "r")           # Redo  -> deleted again
        w.save()
        assert w.alive(), "wpe died during Edit -> Redo"
        assert w.text().strip() == "", \
            "Redo should re-apply the delete, got %r" % w.text()


# --- shortcut path (#159): same operations via the advertised accelerator ---

def test_cut_via_ctrl_x(tmp_path):
    """^X cuts the marked block (advertised Cut accelerator)."""
    with WpeSession(str(tmp_path), "AAA\nBBB\nCCC\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.key(CUT)                      # ^X
        w.save()
        assert w.alive(), "wpe died on ^X"
        assert w.text().strip() == "", \
            "^X should cut the whole buffer, got %r" % w.text()


def test_copy_paste_via_ctrl_c_ctrl_v(tmp_path):
    """^C then ^V duplicates the marked block."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.key(COPY)                     # ^C
        w.key(PASTE)                    # ^V
        w.save()
        assert w.alive(), "wpe died on ^C/^V"
        assert w.text().count("AAA") == 2 and w.text().count("BBB") == 2, \
            "^C then ^V should duplicate the block, got %r" % w.text()


def test_undo_via_ctrl_u(tmp_path):
    """^U undoes a block delete (the block comes back)."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "d")           # Delete the block
        w.key(UNDO)                     # ^U -> restore
        w.save()
        assert w.alive(), "wpe died on ^U"
        assert "AAA" in w.text() and "BBB" in w.text(), \
            "^U should restore the deleted block, got %r" % w.text()


def test_redo_via_ctrl_r(tmp_path):
    """^R re-applies an undone delete (the block goes away again)."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.menu(ALT.EDIT, "d")           # Delete
        w.key(UNDO)                     # ^U -> restored (AAA back)
        w.key(REDO)                     # ^R -> delete again
        w.save()
        assert w.alive(), "wpe died on ^R"
        assert w.text().strip() == "", \
            "^R should re-apply the delete (empty buffer), got %r" % w.text()


def test_show_buffer_via_alt_y(tmp_path):
    """Alt-Y opens the Show Buffer window (title bar reads ' Buffer ')."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.BLOCK, "o")          # Mark WhOle
        w.key(COPY)                     # put something in the buffer
        w.key(SHOW_BUFFER)              # Alt-Y
        disp = "\n".join(w.display())
        assert w.alive(), "wpe died on Alt-Y"
        assert " Buffer " in disp, \
            "Alt-Y should open the Show Buffer window:\n%s" % disp
