"""System-menu (#) tests for xwpe (X11/Xft) -- twin of
tests/test_menu_system.py for wpe.

The '#' menu has no Alt-<letter> hotkey, so it is opened with xwpe's ESC
meta-prefix (esc_menu), the X11 equivalent of the terminal's Alt encoding.
These open informational dialogs; we assert on a screenshot diff that a window
appeared (Xft text can't be read back like the pyte screen).
"""
from conftest import changed_pixels


def test_about_opens_a_dialog(xwpe):
    """# -> About WE opens the About dialog."""
    before = xwpe.screenshot()
    xwpe.esc_menu("numbersign", "a")     # # -> About WE
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening About WE"
    assert changed_pixels(before, after) > 3000, \
        "# -> About WE should open a dialog (screen barely changed)"


def test_system_info_opens_a_dialog(xwpe):
    """# -> System Info opens the Information dialog."""
    before = xwpe.screenshot()
    xwpe.esc_menu("numbersign", "s")     # # -> System Info
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening System Info"
    assert changed_pixels(before, after) > 2000, \
        "# -> System Info should open a dialog (screen barely changed)"


def test_clear_desktop_closes_the_window(xwpe):
    """# -> Clear Desktop closes the open edit window (big screen change)."""
    before = xwpe.screenshot()
    xwpe.esc_menu("numbersign", "c")     # # -> Clear Desktop
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died during Clear Desktop"
    assert changed_pixels(before, after) > 3000, \
        "# -> Clear Desktop should clear the edit window"
