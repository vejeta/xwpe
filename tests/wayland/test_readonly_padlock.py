"""Read-only window padlock renders as a vector icon on Wayland.

The clipboard viewer (Alt-Y) is a read-only window; xwpe marks its title bar
with a padlock.  The colour-emoji glyph renders at its own large bitmap size and
showed clipped ("half padlock"), so the graphical backends paint a vector lock
scaled to the cell (draw_lock).  This guards that: after Alt-Y the padlock is
actually drawn on the window's title bar -- a compact vertical cluster of
foreground pixels only the lock body/shackle produces, not the 1px title border.
"""


def _is_purple(p):
    # xwpe's window title bar (dark blue/purple) vs the light editor/menu areas
    return p[2] > 90 and p[2] > p[0] + 25 and p[2] > p[1] + 25


def _is_fg(p):
    # the padlock is painted in the near-white title foreground colour
    return p[0] > 190 and p[1] > 190 and p[2] > 190


def test_readonly_padlock_is_drawn(xwpe):
    xwpe.type("read only viewer content")
    xwpe.key("shift+Home")
    xwpe.key("ctrl+c")
    xwpe.key("alt+y")                 # Show Buffer -> read-only window
    assert xwpe.proc.poll() is None, "xwpe died opening the read-only viewer"

    img = xwpe.screenshot()
    assert img is not None, "no frame was dumped"
    px = img.load()
    w, h = img.size

    # Locate the window title bar: rows whose left third is mostly purple.
    title_rows = [y for y in range(h)
                  if sum(_is_purple(px[x, y]) for x in range(0, w // 3)) > w // 6]
    assert title_rows, "no purple window title bar found in the frame"
    y0, y1 = min(title_rows), max(title_rows)

    # The padlock sits a couple of cells in from the left.  Its body+shackle
    # stack several foreground pixels in a single column; the 1px title border
    # never does.  Skip the far-left window frame (x < 8).
    tallest = max(
        (sum(_is_fg(px[x, y]) for y in range(y0, y1 + 1))
         for x in range(8, min(w // 2, 120))),
        default=0)
    assert tallest >= 5, (
        "padlock not drawn on the title bar (tallest fg column = %d px); "
        "the vector lock should stack the body and shackle" % tallest)
