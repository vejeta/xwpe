"""File-menu (Alt-F) regression tests for wpe (terminal mode).

File items and bindings (we_menue.c):
  New     -> e_new            Save As -> WpeSaveAsManager (File-Manager dialog)
  Save    -> e_m_save         Save aLl-> e_saveall
  File-Manager / Execute / SHell / Find / Grep / Print File spawn external
  tools and are not driven here.

Covers the deterministic, file-output operations -- including the Save As
File-Manager dialog -- because driving the dialogs is exactly the point.
"""
import os
import time

from wpe_driver import WpeSession, ALT


def test_save_persists_an_edit(tmp_path):
    """Type a character, File -> Save, and the file on disk gains it."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.key("X")                       # insert at the top of the buffer
        w.menu(ALT.FILE, "s")            # File -> Save
        assert w.alive(), "wpe died during File -> Save"
        assert w.text().startswith("X"), \
            "Save should persist the typed char, got %r" % w.text()


def test_save_all_persists_an_edit(tmp_path):
    """Save aLl writes the (single) modified buffer to disk too."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.key("Y")                       # insert at the top
        w.menu(ALT.FILE, "l")            # File -> Save aLl
        assert w.alive(), "wpe died during File -> Save aLl"
        assert w.text().startswith("Y"), \
            "Save aLl should persist the typed char, got %r" % w.text()


def test_save_as_writes_a_new_file(tmp_path):
    """Save As (the File-Manager dialog): type a new name into Name and Alt-S
    writes the buffer to that file."""
    with WpeSession(str(tmp_path), "HELLO\nWORLD\n") as w:
        w.menu(ALT.FILE, "a")            # File -> Save As (File-Manager)
        w.key("out.c")                   # type the name into the Name field
        w.key("\033s")                   # Alt-S : Save
        time.sleep(0.4)
        out = os.path.join(str(tmp_path), "out.c")
        assert w.alive(), "wpe died during File -> Save As"
        assert os.path.exists(out), "Save As should create out.c, dir=%r" % \
            os.listdir(str(tmp_path))
        with open(out) as fh:
            assert fh.read() == "HELLO\nWORLD\n", "Save As wrote wrong content"


def test_new_opens_untitled_buffer(tmp_path):
    """File -> New opens a fresh, untitled ('Noname') edit window."""
    with WpeSession(str(tmp_path), "AAA\nBBB\n") as w:
        w.menu(ALT.FILE, "n")            # File -> New
        screen = "\n".join(w.display())
        assert w.alive(), "wpe died during File -> New"
        assert "Noname" in screen, \
            "File -> New should open an untitled (Noname) buffer"
