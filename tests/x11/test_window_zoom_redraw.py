"""Window Zoom redraw under xwpe (X11/Xft).

Bug (reported by the maintainer): with two windows open -- an editor and the
Messages window from F9 -- zooming Messages to full size leaves the EDITOR's
horizontal scrollbar bleeding through, as a dark bar across the middle of the
now-full Messages window.

Root cause (investigation): the scrollbar's thick bar is painted by
e_make_xrect (X11 filled rectangles), a separate overlay from the SCREENCELL
grid.  When a window is covered (here by the zoomed Messages), those rectangles
are not cleared -- and the extbyte clear in e_invalidate_area is compiled out
because this Xft build defines NONEWSTYLE (NEWSTYLE off).  A full Repaint
Desktop reproduces the same bar, confirming it is the covered window's chrome
being recomposited, not stale state a single redraw missed.

This is a cosmetic artifact in a niche multi-window zoom scenario (no crash, no
data loss); the proper fix is to clear the e_make_xrect overlay for covered
regions and is tracked for 1.6.4.  The test is marked @incoherence (xfail) so it
documents the bug, lists under `pytest -rxX`, and flips to XPASS the moment the
overlay clearing is fixed.
"""
import time

from conftest import incoherence


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


@incoherence("zoom over a window leaves the covered window's scrollbar "
             "(e_make_xrect overlay) bleeding through -- fix tracked for 1.6.4")
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
