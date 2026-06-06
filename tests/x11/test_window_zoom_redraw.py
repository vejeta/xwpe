"""Window Zoom/cover redraw under xwpe (X11/Xft) -- no covered-window scrollbar bleed.

Bug (reported by the maintainer): with two windows open -- an editor and the
Messages window from F9 -- zooming Messages to full size (or dragging a window
over another) left the COVERED window's scrollbar bleeding through, as a dark
bar across the middle of the top window.  X11 only; the ncurses build composites
one cell grid (last-writer-wins) so it never had the bug.

Root cause: the modern fluid scrollbars are drawn directly to the Cairo surface
by wpe_render_chrome() (we_render_cairo.c), per window, every frame, AFTER the
cell render and with NO z-order clipping -- so a covered window's scrollbar
painted on top of the window covering it.  (This is why it was invisible to the
schirm/extbyte diff renderer.)  Fix: clip each window's scrollbars to its
visible region -- its rectangle MINUS every window stacked above it -- using a
Cairo even-odd fill-rule clip (the idiomatic Cairo region clip; cf. ncurses
panel update_panels and Turbo Vision's clip-to-exposed-region contract).

This guards the fix and catches any regression of the chrome z-order clip.
"""
import time


def _bar_rows(img, y0f, y1f):
    """Rows in the band [y0f, y1f) (fractions of height) that are mostly a dark
    horizontal bar -- i.e. a scrollbar painted across most of the width."""
    px = img.load()
    w, h = img.size
    rows = []
    for y in range(int(h * y0f), int(h * y1f)):
        dark = sum(1 for x in range(100, w - 20) if sum(px[x, y][:3]) < 150)
        if dark > 700:
            rows.append(y)
    return rows


def _horizontal_bar_rows(img):
    """Dark horizontal bars across the MIDDLE of the screen (a covered window's
    scrollbar bleeding through a window zoomed over it).  Excludes the title and
    any window's own bottom scrollbar."""
    return _bar_rows(img, 0.20, 0.88)


def test_zoom_over_messages_no_scrollbar_bleed(xwpe):
    """Zooming Messages over the editor must not leave the editor's scrollbar
    bar across the middle of the screen (FULL cover)."""
    xwpe.key("F9")
    time.sleep(3.0)                      # compile -> Messages window appears
    xwpe.key("alt+n")
    time.sleep(0.6)                      # make Messages the active window
    xwpe.key("alt+z")
    time.sleep(0.8)                      # zoom Messages to full
    shot = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died zooming Messages"
    bars = _horizontal_bar_rows(shot)
    assert not bars, \
        "a covered window's scrollbar bled through the zoomed window at rows %r" % bars


def test_cascade_no_scrollbar_bleed(xwpe):
    """Cascade overlaps windows diagonally (PARTIAL cover): a lower window's
    bottom edge -- and its horizontal scrollbar -- falls inside the window
    stacked on top.  That covered scrollbar must be clipped, not bleed through
    as a dark bar inside the top window.  This exercises the even-odd partial
    clip that the full-cover zoom case does not.  (Cascade is a re-layout/
    resize of every window, so it also guards the resize path: the chrome clip
    is recomputed from the current geometry every frame, regardless of how the
    overlap arose -- zoom, drag, resize or cascade.)"""
    xwpe.menu("f", "n")
    time.sleep(0.5)                      # second window
    xwpe.menu("f", "n")
    time.sleep(0.5)                      # third window
    xwpe.menu("w", "a")
    time.sleep(0.7)                      # Cascade -> diagonal partial overlap
    shot = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died cascading windows"
    # Each cascaded window's horizontal scrollbar sits at its own bottom edge,
    # a few rows apart.  With the clip, only the FRONT window's scrollbar shows
    # at the bottom of the stack -- a single bar (~16px tall).  Without it, the
    # covered windows' scrollbars bleed through too, stacking into a much taller
    # smear (~33px for three windows).  Measure the vertical SPAN of bottom-band
    # scrollbar rows: position-independent, so it does not depend on the exact
    # cascade geometry.
    rows = _bar_rows(shot, 0.86, 0.97)
    assert rows, "no bottom scrollbar found after cascade (front window chrome missing?)"
    span = rows[-1] - rows[0]
    assert span <= 20, \
        "covered windows' scrollbars bled through the cascade: bottom bars span " \
        "%dpx (rows %d..%d); a single front-window scrollbar is ~16px" \
        % (span, rows[0], rows[-1])
