"""Popup close button for xwpe NATIVE WAYLAND -- twin of
tests/x11/test_popup_close_button.py.

Verifies that clicking the dialog's top-right [X] dismisses it.  The close glyph
is located by its red colour in xwpe's own wl_shm frame dump (so no font metrics
are hard-coded), then clicked.  This exercises the full native pointer path:
the click pixel comes from the surface-relative screenshot, so a pass also
confirms the harness maps an xdotool click to the right surface cell."""
import time

from conftest import changed_pixels


def _find_close_x(img):
    """Pixel centroid of the red close glyph in a dialog's top border."""
    w, h = img.size
    px = img.load()
    pts = []
    for y in range(int(h * 0.10), int(h * 0.20)):
        for x in range(int(w * 0.30), int(w * 0.70)):
            r, g, b = px[x, y]
            if r > 140 and g < 80 and b < 80:
                pts.append((x, y))
    if not pts:
        return None
    return (sum(p[0] for p in pts) // len(pts),
            sum(p[1] for p in pts) // len(pts))


def test_clicking_close_box_dismisses_dialog(xwpe):
    """Click the dialog's [X] -> the Editor-Options dialog closes (back to editor)."""
    editor = xwpe.screenshot()
    xwpe.key("alt+o", delay=0.7)         # Options
    xwpe.key("e", delay=1.0)             # Editor -> dialog opens
    opened = xwpe.screenshot()
    assert changed_pixels(editor, opened) > 2000, "dialog did not open"

    spot = _find_close_x(opened)
    assert spot, "could not locate the red close [X] on the dialog title bar"
    xwpe.click(spot[0], spot[1])
    time.sleep(0.5)
    closed = xwpe.screenshot()

    assert xwpe.proc.poll() is None, "xwpe died clicking the close box"
    assert changed_pixels(editor, closed) < 1500, \
        "clicking [X] should dismiss the dialog and restore the editor"
