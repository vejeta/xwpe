"""System-menu (Alt-#) regression tests for wpe (terminal mode).

The leftmost '#' menu (we_menue.c mainmenu[0]):
  About WE      -> e_about_WE      System Info   -> e_sys_info
  Clear Desktop -> e_clear_desk    Show Wastebasket / Delete Wastebasket
  Repaint Desktop -> e_repaint_desk

These open informational dialogs or act on the desktop, so they are checked
on the rendered screen (no file output).
"""
from wpe_driver import WpeSession, ALT


def _screen(w):
    return "\n".join(w.display())


def test_about_shows_program_info(tmp_path):
    """# -> About WE shows the program name and the author credits."""
    with WpeSession(str(tmp_path), "AAA\n") as w:
        w.menu(ALT.SYSTEM, "a")          # # -> About WE
        scr = _screen(w)
        assert w.alive(), "wpe died opening About WE"
        assert "Programming Environment" in scr and "Kruse" in scr, \
            "About WE should show the program name and authors, got:\n%s" % scr


def test_system_info_shows_current_file(tmp_path):
    """# -> System Info reports the current file and directory."""
    with WpeSession(str(tmp_path), "AAA\n") as w:
        w.menu(ALT.SYSTEM, "s")          # # -> System Info
        scr = _screen(w)
        assert w.alive(), "wpe died opening System Info"
        assert "Information" in scr and "Current File" in scr and "t.c" in scr, \
            "System Info should report the current file, got:\n%s" % scr


def test_clear_desktop_closes_the_window(tmp_path):
    """# -> Clear Desktop closes the open edit window."""
    with WpeSession(str(tmp_path), "AAA\n") as w:
        assert "t.c" in _screen(w), "the t.c window should be open to start"
        w.menu(ALT.SYSTEM, "c")          # # -> Clear Desktop
        assert w.alive(), "wpe died during Clear Desktop"
        assert "t.c" not in _screen(w), \
            "Clear Desktop should close the t.c window, got:\n%s" % _screen(w)
