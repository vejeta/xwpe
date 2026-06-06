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


def _horizontal_bar_rows(img):
    """Rows in the mid-band that are mostly a dark horizontal bar (a scrollbar
    bleeding across the window body).  Excludes the title and the window's own
    bottom scrollbar."""
    px = img.load()
    w, h = img.size
    rows = []
    for y in range(int(h * 0.20), int(h * 0.88)):
        dark = sum(1 for x in range(100, w - 20) if sum(px[x, y][:3]) < 150)
        if dark > 700:
            rows.append(y)
    return rows


def test_zoom_over_messages_no_scrollbar_bleed(xwpe):
    """Zooming Messages over the editor must not leave the editor's scrollbar
    bar across the middle of the screen."""
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
