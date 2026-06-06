"""File-menu (Alt-F) tests for xwpe (X11/Xft) -- twin of
tests/test_menu_file.py for wpe.

Save / Save aLl / Save As (the File-Manager dialog) / New, asserted on the
files written to disk.  Driving the X11 dialogs is the point: the field editor
and Alt-S confirmation go through xwpe's own X11 input path.

The shared `xwpe` fixture seeds a multi-line C file as session.srcfile.
"""
import os
import time


def _sibling(xwpe, name):
    return os.path.join(os.path.dirname(xwpe.srcfile), name)


def test_save_persists_an_edit(xwpe):
    """Type a char, File -> Save, the file on disk gains it."""
    xwpe.type("X")                       # insert at the top
    xwpe.menu("f", "s")                  # File -> Save
    assert xwpe.proc.poll() is None, "xwpe died during File -> Save"
    assert xwpe.saved_text().startswith("X"), \
        "Save should persist the typed char, got %r" % xwpe.saved_text()


def test_save_all_persists_an_edit(xwpe):
    """File -> Save aLl writes the modified buffer to disk."""
    xwpe.type("Y")
    xwpe.menu("f", "l")                  # File -> Save aLl
    assert xwpe.proc.poll() is None, "xwpe died during File -> Save aLl"
    assert xwpe.saved_text().startswith("Y"), \
        "Save aLl should persist the typed char, got %r" % xwpe.saved_text()


def test_save_as_writes_a_new_file(xwpe):
    """Save As (File-Manager): type a name into Name, Alt-S writes the file."""
    xwpe.menu("f", "a")                  # File -> Save As (File-Manager)
    xwpe.type("out.c")                   # name into the Name field
    xwpe.key("alt+s")                    # Alt-S : Save
    time.sleep(0.5)
    out = _sibling(xwpe, "out.c")
    assert xwpe.proc.poll() is None, "xwpe died during File -> Save As"
    assert os.path.exists(out), "Save As should create out.c, dir=%r" % \
        os.listdir(os.path.dirname(xwpe.srcfile))
    assert "int main" in open(out).read(), "Save As wrote wrong content"


def test_new_opens_editable_buffer(xwpe):
    """File -> New gives a fresh buffer; typing + Save As writes its content."""
    xwpe.menu("f", "n")                  # File -> New (Noname)
    xwpe.type("NEWDATA")                 # type into the new buffer
    xwpe.menu("f", "a")                  # Save As
    xwpe.type("fresh.c")
    xwpe.key("alt+s")
    time.sleep(0.5)
    out = _sibling(xwpe, "fresh.c")
    assert xwpe.proc.poll() is None, "xwpe died during File -> New + Save As"
    assert os.path.exists(out), "New + Save As should create fresh.c, dir=%r" % \
        os.listdir(os.path.dirname(xwpe.srcfile))
    assert "NEWDATA" in open(out).read(), \
        "the New buffer's typed text should be saved, got %r" % open(out).read()


# --- shortcut path (#159): File accelerators in the X11 input path ---
# These are bare function keys; they reach xwpe only because conftest injects
# them BY KEYCODE (xdotool bug #491 otherwise turns F2 into Alt-F2) -- see the
# README "Function keys are injected by keycode" note.

def test_save_via_f2(xwpe):
    """F2 (advertised Save accelerator) persists the edit to disk."""
    xwpe.type("X")                       # insert at the top
    xwpe.key("F2")                       # File -> Save
    assert xwpe.proc.poll() is None, "xwpe died on F2 (Save)"
    assert xwpe.saved_text().startswith("X"), \
        "F2 should persist the typed char, got %r" % xwpe.saved_text()


def test_open_via_f3(xwpe):
    """F3 (advertised File-Manager/Open accelerator) opens the File-Manager
    dialog -- a large screen change with no menu navigation."""
    from conftest import changed_pixels
    before = xwpe.screenshot()
    xwpe.key("F3")                       # File-Manager (Open)
    time.sleep(0.5)
    assert xwpe.proc.poll() is None, "xwpe died on F3 (File-Manager)"
    assert changed_pixels(before, xwpe.screenshot()) > 20000, \
        "F3 should open the File-Manager dialog (screen barely changed)"
