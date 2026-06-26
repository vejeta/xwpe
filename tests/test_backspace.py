"""Backspace / DEL editing in the buffer for wpe (terminal mode).

macOS Terminal.app, iTerm2 and kitty send DEL (0x7f) for the Backspace key,
not the BS (0x08) that ncurses maps to KEY_BACKSPACE.  Until 1.6.7 a raw 0x7f
reached the editor untranslated and Backspace simply did nothing there -- you
could type but not erase.  e_t_getch now folds a bare 0x7f to the same
delete-left action as KEY_BACKSPACE (and ESC+0x7f to Alt-Backspace), so the
key works on those terminals.

These drive the real key bytes through the pty and assert on the file written
to disk, so a regression that breaks delete-left -- or that breaks insertion --
fails here.  They need no toolchain, so they run on every platform's CI.

Run: tests/.venv/bin/python -m pytest -v tests/test_backspace.py
"""
from wpe_driver import WpeSession

DEL = "\x7f"   # what Terminal.app/iTerm2/kitty send for the Backspace key


def test_del_erases_the_character_to_the_left(tmp_path):
    """Type ABC, press Backspace once: the C is erased, AB remains.

    Reaching "AB" needs BOTH insertion and one delete-left to work, so a dead
    Backspace (buffer stays "ABC") or dead typing (buffer empty) both fail."""
    with WpeSession(str(tmp_path), "") as w:
        w.key("A", "B", "C")
        w.key(DEL)
        w.save()
        assert w.alive(), "wpe died on Backspace (DEL 0x7f)"
        assert w.text().strip() == "AB", \
            "Backspace should erase the last char, got %r" % w.text()


def test_repeated_del_erases_several_characters(tmp_path):
    """Three Backspaces after ABC leave the line empty (delete-left repeats)."""
    with WpeSession(str(tmp_path), "") as w:
        w.key("A", "B", "C")
        w.key(DEL, DEL, DEL)
        w.save()
        assert w.alive(), "wpe died on repeated Backspace"
        assert w.text().strip() == "", \
            "Three Backspaces should clear ABC, got %r" % w.text()
