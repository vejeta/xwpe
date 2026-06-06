"""Window-menu (Alt-W) tests for xwpe (X11/Xft) -- the X11 twin of the wpe
Window coverage (tests/test_window_menu.py, tests/test_window_views.py).

Window operations are layout changes, so we assert on a screenshot diff that
the arrangement changed.  Tile/Cascade need two windows, so we open a second
with File -> New first.

Window items (we_menue.c): Zoom z, Tile t, Cascade a, Next x, Close c,
List All l.
"""
from conftest import changed_pixels


def _second_window(xwpe):
    """Open a second edit window (File -> New) and return a screenshot."""
    xwpe.menu("f", "n")                  # File -> New (Noname)
    return xwpe.screenshot()


def test_list_all_opens_the_window_chooser(xwpe):
    """Window -> List All opens the 'Windows' chooser dialog."""
    before = xwpe.screenshot()
    xwpe.menu("w", "l")                  # Window -> List All
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening Window -> List All"
    assert changed_pixels(before, after) > 1500, \
        "List All should open the window chooser (screen barely changed)"


def test_tile_rearranges_two_windows(xwpe):
    """Window -> Tile re-lays-out the two open windows."""
    base = _second_window(xwpe)
    xwpe.menu("w", "t")                  # Window -> Tile
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died during Window -> Tile"
    assert changed_pixels(base, after) > 1500, \
        "Tile should rearrange the windows (screen barely changed)"


def test_cascade_rearranges_two_windows(xwpe):
    """Window -> Cascade overlaps the two open windows."""
    base = _second_window(xwpe)
    xwpe.menu("w", "t")                  # Tile first (a known split layout)
    base = xwpe.screenshot()
    xwpe.menu("w", "a")                  # Window -> Cascade
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died during Window -> Cascade"
    assert changed_pixels(base, after) > 1500, \
        "Cascade should rearrange the windows (screen barely changed)"


def test_zoom_maximises_active_window(xwpe):
    """Window -> Zoom grows the active window over the other (tiled) one."""
    _second_window(xwpe)
    xwpe.menu("w", "t")                  # Tile so both are visible side by side
    base = xwpe.screenshot()
    xwpe.menu("w", "z")                  # Window -> Zoom
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died during Window -> Zoom"
    assert changed_pixels(base, after) > 1500, \
        "Zoom should maximise the active window (screen barely changed)"


# --- shortcut path (#159): Window accelerators in the X11 input path ---

def test_zoom_via_alt_z(xwpe):
    """Alt-Z (advertised) zooms the active window."""
    _second_window(xwpe)
    xwpe.menu("w", "t")                  # Tile so both are visible
    base = xwpe.screenshot()
    xwpe.key("alt+z")                    # Zoom
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Z"
    assert changed_pixels(base, after) > 1500, \
        "Alt-Z should zoom the active window (screen barely changed)"


def test_list_all_via_alt_0(xwpe):
    """Alt-0 (advertised) opens the 'Windows' chooser."""
    before = xwpe.screenshot()
    xwpe.key("alt+0")                    # List All
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Alt-0"
    assert changed_pixels(before, after) > 1500, \
        "Alt-0 should open the window chooser (screen barely changed)"


def test_close_via_ctrl_w(xwpe):
    """Ctrl-W (modern close) closes the active window, revealing the other."""
    _second_window(xwpe)                 # two windows; the new one is active
    before = xwpe.screenshot()
    xwpe.key("ctrl+w")                   # close the active window
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on Ctrl-W (close)"
    assert changed_pixels(before, after) > 1500, \
        "Ctrl-W should close the active window (screen barely changed)"
