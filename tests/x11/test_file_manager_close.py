"""File-manager dialog close box for xwpe (X11/Xft).

Twin of tests/test_file_manager_close.py.  The File-Manager (File menu -> first
item) is a fixed-size picker: it must carry only a close [X] (no maximize box),
and clicking that [X] must dismiss it.  Regression: the close glyph is drawn
top-right (e.x-2) but WpeMngMouseInFileManager hit-tested the old top-left
position, so clicking the visible ✕ did nothing.

We locate the close glyph by its colour at the top of the dialog and click it,
then assert the screen returns to the editor (the dialog is gone).
"""
import time

from conftest import changed_pixels


def _open_file_manager(xwpe):
    xwpe.key("alt+f", delay=0.6)
    xwpe.key("Return", delay=1.0)        # first File-menu item == File-Manager


def _find_close_glyph(img):
    """Rightmost cell of the dialog close [X] in the title band.

    The close glyph is painted in the window-border accent colour (a distinct
    blue-purple, er.fb) along the top of the centred dialog; we take the
    right-most run of such pixels in the top band -- that is the close box."""
    w, h = img.size
    px = img.load()
    pts = []
    for y in range(int(h * 0.02), int(h * 0.08)):
        for x in range(int(w * 0.30), int(w * 0.95)):
            r, g, b = px[x, y][:3]
            # blue-purple accent: clearly more blue than red/green, not grey
            if b > 100 and b - r > 40 and b - g > 40:
                pts.append((x, y))
    if not pts:
        return None
    xmax = max(p[0] for p in pts)
    cluster = [p for p in pts if p[0] >= xmax - 12]
    return (sum(p[0] for p in cluster) // len(cluster),
            sum(p[1] for p in cluster) // len(cluster))


def test_file_manager_close_box_dismisses(xwpe):
    """Click the File-Manager [X] -> the dialog closes (back to the editor)."""
    editor = xwpe.screenshot()
    _open_file_manager(xwpe)
    opened = xwpe.screenshot()
    assert changed_pixels(editor, opened) > 20000, "File-Manager did not open"

    spot = _find_close_glyph(opened)
    assert spot, "could not locate the File-Manager close [X]"
    xwpe.click(spot[0], spot[1])
    time.sleep(0.6)
    closed = xwpe.screenshot()

    assert xwpe.proc.poll() is None, "xwpe died clicking the FM close box"
    assert changed_pixels(editor, closed) < 8000, \
        "clicking [X] should dismiss the File-Manager and restore the editor"
