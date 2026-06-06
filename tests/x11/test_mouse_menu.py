"""Mouse menu access for xwpe (X11/Xft): clicking the menu bar opens a menu.

Complements the keyboard menu coverage (test_menu_*.py): here the top-level
menu is opened with a real mouse click on the menu-bar label, and a menu item
is invoked by clicking it -- the e_mouse menu-bar hit path, not Alt-<letter>.
"""
import time

from conftest import changed_pixels


def test_click_menubar_opens_and_escape_closes(xwpe):
    """Clicking 'File' in the menu bar drops its menu; Escape closes it."""
    base = xwpe.screenshot()
    xwpe.click(63, 9)                      # "File" label in the menu bar
    opened = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died clicking the menu bar"
    assert changed_pixels(base, opened) > 10000, \
        "clicking File should drop its menu (screen barely changed)"
    xwpe.key("Escape")
    closed = xwpe.screenshot()
    assert changed_pixels(base, closed) < 1500, \
        "Escape should close the menu and restore the screen"


def test_click_menu_item_invokes_it(xwpe):
    """Open File by click, then click the 'New' item -> a fresh Noname buffer
    (a second window appears: a large, persistent screen change, no Escape)."""
    base = xwpe.screenshot()
    xwpe.click(63, 9)                      # open File
    time.sleep(0.3)
    xwpe.click(60, 59)                     # the "New" item in the dropdown
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died invoking a menu item by click"
    # A new window changes the title bar ('Noname', window number 2) and content;
    # and unlike an open menu, it does not revert on its own.
    assert changed_pixels(base, after) > 3000, \
        "clicking File > New should open a new buffer window"
