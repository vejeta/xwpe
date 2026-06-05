"""Edit-menu (Alt-E) regression tests for wpe (terminal mode).

Edit items and their bindings (we_menue.c):
  Cut    ^X  -> e_edt_del        Undo  ^U -> e_make_undo
  Copy   ^C  -> e_edt_copy       Redo  ^R -> e_make_redo
  Paste  ^V  -> e_edt_einf       Delete ^Del -> e_blck_del
  Show Buffer ^W -> e_show_clipboard

Like test_block.py these drive the deterministic operations through
"Mark Whole" (Block -> Alt-B o) and assert on the file written to disk.
Behaviours we believe should hold but currently do not are marked with
@incoherence(...) so `pytest -rxX` lists them for manual review.
"""
from wpe_driver import WpeSession, ALT


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
