"""Contextual bottom status/hint bar tests for xwpe (X11/Xft) -- twin of
tests/test_menu_status_bar.py for wpe.

The wpe test pins the exact hint strings (eblst_o vs mblst_o); here we confirm
the SAME contextual swap renders in the X11/Xft front-end: the bottom hint bar
must change when focus moves from the editor to the Messages window.  We read
only the bottom strip of the window so the diff reflects the hint bar, not the
Messages content that also appears above it.
"""
import time

from conftest import changed_pixels


def _bottom_strip(img, height=24):
    w, h = img.size
    return img.crop((0, h - height, w, h))


def test_hint_bar_changes_with_focused_window(xwpe):
    """Editor -> Messages focus must repaint the bottom hint bar (X11)."""
    editor = _bottom_strip(xwpe.screenshot())
    xwpe.menu("r", "m")                  # Run -> Make : opens Messages
    time.sleep(2.5)
    xwpe.menu("w", "x")                  # Window -> Next : focus Messages
    time.sleep(0.6)
    messages = _bottom_strip(xwpe.screenshot())
    assert xwpe.proc.poll() is None, "xwpe died switching focus to Messages"
    assert changed_pixels(editor, messages) > 500, \
        "the bottom hint bar should change between editor and Messages focus"
