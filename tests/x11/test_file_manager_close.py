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
    """Pixel of the File-Manager close [X] -- the cell xe-2 on its top border.

    The close box is the [X] that e_hit_close_button() accepts at column
    (f->e.x - 2) on the window's TOP border row (f->a.y) -- a one-cell hit
    target, and ONLY on that exact row (it rejects clicks one row lower).  It
    is painted in the window-border accent colour (blue-purple, er.fb), the
    same colour as the frame, so it cannot be told from the border by colour
    alone.  Locate it geometrically instead:

      * the File-Manager opens as window 2, cascaded BELOW the editor, so its
        own top border is the topmost accent row in this band (~0.18h), well
        below the editor's title bar at the very top of the screen;
      * cap the x-scan short of the screen edge so the editor frame BEHIND the
        File-Manager (full width, ~0.94w) does not capture "rightmost";
      * the right border sits at f->e.x; the close box is ~one and a half cells
        (a cell is ~13px at 1024 wide) left of it, on the border row.
    """
    w, h = img.size
    px = img.load()
    # The frame accent colour is identical to the editor BACKGROUND (both er.fb,
    # RGB 72,61,139), so the title bar cannot be found by frame colour.  Anchor
    # instead on the File-Manager's unmistakable bright-CYAN list panels (Dir
    # tree / Files), which no other window paints, then read the [X] off the
    # title bar that sits a fixed gap above them.  The close glyph is light text
    # (like the "File-Manager" title and the window number) on the frame.
    def is_cyan(x, y):
        r, g, b = px[x, y][:3]
        return r < 100 and g > 150 and b > 150

    def is_light(x, y):
        r, g, b = px[x, y][:3]
        return r > 180 and g > 180 and b > 180

    cyan = [(x, y) for y in range(0, h) for x in range(int(w * 0.20), int(w * 0.85))
            if is_cyan(x, y)]
    if not cyan:
        return None
    cyan_top = min(p[1] for p in cyan)             # top of the list panels
    cyan_right = max(p[0] for p in cyan)           # right edge of the Files panel
    # The title bar is the band of light glyphs above the panels (Directory/Name
    # fields sit between).  Find its topmost light row, then the rightmost light
    # pixel on it -- the [X] close glyph at the right end of the title.
    band = [(x, y)
            for y in range(max(0, cyan_top - 130), cyan_top - 60)
            for x in range(int(w * 0.30), cyan_right + 40)
            if is_light(x, y)]
    if not band:
        return None
    title_y = min(p[1] for p in band)
    row = [p for p in band if p[1] <= title_y + 6]
    xr = max(p[0] for p in row)                    # right end of the title bar
    return (xr - 17, title_y + 1)                  # centre of the [X] close cell


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
