"""Popup close button for xwpe (X11/Xft) -- twin of tests/test_popup_close_button.py.

The wpe test pins the glyphs (close kept, maximize gone).  Here we verify the
behaviour that makes the modernized top-right [X] worth keeping: clicking it
dismisses the dialog, exactly like pressing Esc.  We locate the close glyph by
its red colour (so the test does not hard-code font metrics) and click it.
"""
import time

from conftest import changed_pixels


def _find_close_x(img):
    """Pixel centroid of the red close glyph in a dialog's top border.

    The close box is the only red glyph along the top edge of a centred
    dialog; scanning a top band for red pixels finds it without assuming a
    cell size."""
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
