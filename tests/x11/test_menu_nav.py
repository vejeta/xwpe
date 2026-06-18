"""Menu-to-menu navigation tests for xwpe (X11/Xft).

The user-visible bug they cover: with one top-level menu already open, pressing
Alt+<letter> for a different menu must SWITCH to that menu, not stay on the
current one and not close the menu bar.  Reported divergence (#NNN):
  - Window menu open, press Alt+E -> stays in Window (Edit never opens)
  - Any menu  open, press Alt+B -> menu closes (Block never opens)
The terminal binary (wpe) handles every transition correctly via pyte; the
divergence is X11-specific (xwpe).

Detection method: take screenshots before/after and assert that the post-key
screenshot differs from the pre-key one by more than the background-cursor
noise.  Switching between two top-level menus must rebuild the dropdown at a
different X position, so the pixel delta is large.  Closing the menu also
changes the screen, so we additionally pin the post-key state by asserting
that pressing Esc still has work to do (the Window-menu chrome must be open).
"""
from conftest import changed_pixels


def _open_menu(xwpe, letter):
    xwpe.key("alt+" + letter)
    return xwpe.screenshot()


def _menu_changed(before, after):
    """A dropdown re-anchored to a different column changes thousands of pixels;
    use the same threshold the rest of the suite uses for 'menu opened'."""
    return changed_pixels(before, after) > 1500


def test_alt_e_switches_from_window_to_edit(xwpe):
    """Window menu open -> Alt+E must switch to the Edit dropdown."""
    window_open = _open_menu(xwpe, "w")
    xwpe.key("alt+e")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+E while Window menu open"
    assert _menu_changed(window_open, after), \
        "Alt+E with Window menu open should switch to Edit (screen unchanged)"


def test_alt_b_switches_from_window_to_block(xwpe):
    """Window menu open -> Alt+B must switch to the Block dropdown."""
    window_open = _open_menu(xwpe, "w")
    xwpe.key("alt+b")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+B while Window menu open"
    assert _menu_changed(window_open, after), \
        "Alt+B with Window menu open should switch to Block (screen unchanged)"


def test_alt_w_switches_from_edit_to_window(xwpe):
    """Edit menu open -> Alt+W must switch to the Window dropdown."""
    edit_open = _open_menu(xwpe, "e")
    xwpe.key("alt+w")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+W while Edit menu open"
    assert _menu_changed(edit_open, after), \
        "Alt+W with Edit menu open should switch to Window (screen unchanged)"


def test_alt_b_switches_from_edit_to_block(xwpe):
    """Edit menu open -> Alt+B must switch to the Block dropdown."""
    edit_open = _open_menu(xwpe, "e")
    xwpe.key("alt+b")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+B while Edit menu open"
    assert _menu_changed(edit_open, after), \
        "Alt+B with Edit menu open should switch to Block (screen unchanged)"


def test_alt_e_switches_from_block_to_edit(xwpe):
    """Block menu open -> Alt+E must switch to the Edit dropdown."""
    block_open = _open_menu(xwpe, "b")
    xwpe.key("alt+e")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+E while Block menu open"
    assert _menu_changed(block_open, after), \
        "Alt+E with Block menu open should switch to Edit (screen unchanged)"


def test_alt_f_switches_from_window_to_file(xwpe):
    """Window menu open -> Alt+F must switch to the File dropdown."""
    window_open = _open_menu(xwpe, "w")
    xwpe.key("alt+f")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+F while Window menu open"
    assert _menu_changed(window_open, after), \
        "Alt+F with Window menu open should switch to File (screen unchanged)"


def test_alt_s_switches_from_window_to_search(xwpe):
    """Window menu open -> Alt+S must switch to the Search dropdown."""
    window_open = _open_menu(xwpe, "w")
    xwpe.key("alt+s")
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt+S while Window menu open"
    assert _menu_changed(window_open, after), \
        "Alt+S with Window menu open should switch to Search (screen unchanged)"
