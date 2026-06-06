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


def _right_vbar_groups(img, y0f=0.10, y1f=0.92):
    """Count distinct vertical scrollbar bars near the right edge: groups of
    adjacent columns (in the rightmost ~30px) that hold a tall dark vertical
    run.  Only the ACTIVE window's scrollbar should be there (1 group); a
    covered window that is wider/offset would peek a SECOND bar to its side."""
    px = img.load()
    w, h = img.size
    cols = []
    for x in range(w - 30, w):
        run = best = 0
        for y in range(int(h * y0f), int(h * y1f)):
            if sum(px[x, y][:3]) < 150:
                run += 1
                best = max(best, run)
            else:
                run = 0
        if best > 150:
            cols.append(x)
    groups, prev = 0, -10
    for x in cols:
        if x - prev > 2:
            groups += 1
        prev = x
    return groups


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


def _size_move(xwpe, *seq):
    """Drive xwpe's Size/Move (Ctrl-F5): each (key, n) sends key n times, then
    Enter commits.  Arrows move the window a cell at a time; Shift+arrows resize
    the bottom/right edge (we_wind.c e_size_move)."""
    xwpe.key("ctrl+F5", delay=0.3)
    for k, n in seq:
        for _ in range(n):
            xwpe.key(k, delay=0.04)
    xwpe.key("Return", delay=0.3)


def _dark_in_box(img, x0, x1, y0, y1):
    """Count near-black pixels in a box -- a scrollbar bar reads as dark."""
    px = img.load()
    return sum(1 for x in range(x0, x1) for y in range(y0, y1)
               if sum(px[x, y][:3]) < 150)


def test_triple_overlap_no_scrollbar_bleed(xwpe):
    """Three windows stacked so window 1's right-edge scrollbar is covered by
    TWO windows at once -- the bug the maintainer hit dragging a 3rd window over
    two others (#165).  Window 1's vertical scrollbar reappeared in the band
    where it intersects BOTH covering windows.

    Root cause (we_render_cairo.c): the chrome clipped each window's scrollbars
    to "its rect minus the covers" with a Cairo EVEN-ODD path.  Even-odd is XOR,
    which equals set-difference only while the covers don't overlap each other.
    Where two higher windows overlap, a point is in 3 rectangles (window 1 + 2
    covers) -> odd -> even-odd wrongly keeps it, so window 1's scrollbar bled
    through the triple-overlap band.  Fix: e_chrome_visible_region() subtracts
    the covers with cairo_region_t (true integer-rectangle set algebra).

    Construction (deterministic, via Size/Move):
      w1 = t.c, narrowed to col 63, full height  -> scrollbar mid-screen, top
           rows visible (so the bar IS drawn).
      w2, w3 = full-width bottom strips overlapping each other AND w1's right
           edge -> a double-covered band over w1's scrollbar column.
    """
    _size_move(xwpe, ("shift+Left", 64))                 # w1 narrow, full height
    xwpe.menu("f", "n")
    time.sleep(0.4)
    _size_move(xwpe, ("shift+Right", 70), ("shift+Up", 16), ("Down", 15))  # w2
    xwpe.menu("f", "n")
    time.sleep(0.4)
    _size_move(xwpe, ("shift+Up", 4), ("Up", 5))         # w3 over w1+w2
    time.sleep(0.4)
    shot = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died building the 3-window stack"

    w, h = shot.size
    xc = int(63 * w / 128)                # window 1's scrollbar column (col 63)
    # Precondition: window 1's scrollbar must actually be drawn (its top is
    # exposed) -- else the construction drifted and the test would pass vacuously.
    top = _dark_in_box(shot, xc, xc + 10, int(0.06 * h), int(0.22 * h))
    assert top > 20, \
        "scenario did not build: window 1's scrollbar is not visible at its " \
        "exposed top (got %d dark px) -- Size/Move counts need adjusting" % top
    # The bug: window 1's scrollbar reappears in the double-covered band, the
    # middle third of the screen at that same column.  Must be clear now.
    bled = _dark_in_box(shot, xc, xc + 10, int(0.34 * h), int(0.52 * h))
    assert bled < 30, \
        "window 1's vertical scrollbar bled through the double-covered band " \
        "(%d dark px at its column mid-screen; a clean clip leaves ~0)" % bled


def test_click_reorder_no_scrollbar_bleed(xwpe):
    """Click-to-raise a covered window must re-clip every window's scrollbars to
    the NEW z-order -- the 3-window drag/click bug.

    Root cause (we_render_cairo.c wpe_render_chrome): the chrome indexed the
    window arrays by window NUMBER (f[edt[w]], is_active = edt[w]==edt[curedt])
    while the cell compositor (e_repaint_desk_nopic) indexes them by Z-LEVEL
    (f[w], active = f[mxedt]).  The two agree ONLY while edt[] is the identity
    (windows never reordered).  After a mouse click raises a covered window,
    edt[] is permuted -- the chrome then read the WRONG window's geometry, drew
    the wrong window's scrollbars, and the clip used the wrong rectangles, so a
    behind window's vertical AND horizontal scrollbar bled through the front one.
    This exercises that exact path: overlap, then reorder by clicking."""
    xwpe.menu("f", "n")
    time.sleep(0.5)                      # second window
    xwpe.menu("f", "n")
    time.sleep(0.5)                      # third window
    xwpe.menu("w", "a")
    time.sleep(0.7)                      # Cascade -> diagonal partial overlap
    # Raise a COVERED window by clicking its peeking top-left corner (the
    # bottommost cascade window starts furthest up-left).  This permutes edt[].
    xwpe.click(60, 40)
    time.sleep(0.7)
    shot = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died on click-to-raise"
    # After the reorder the chrome must still show only the front window's
    # vertical scrollbar at the right edge, and no stacked horizontal-bar smear.
    vbars = _right_vbar_groups(shot)
    assert vbars <= 1, \
        "after click-reorder a covered window's vertical scrollbar bled past " \
        "the front window: %d distinct right-edge bars (expected <=1)" % vbars
    mid = _horizontal_bar_rows(shot)
    assert not mid, \
        "after click-reorder a covered window's horizontal scrollbar bled " \
        "across the middle of the front window at rows %r" % mid


def test_cascade_no_scrollbar_bleed(xwpe):
    """Cascade overlaps 3 windows diagonally, each slightly wider/offset, so a
    covered window's right-edge (and bottom-edge) scrollbar sticks out past the
    window stacked on top -- a lone "floating" scrollbar, the 3-window drag bug.
    Only the FRONT window's scrollbars must show: a covered window down to just
    its border shows the plain border line, not a scrollbar.  Guards both the
    even-odd clip and the floating-scrollbar suppression."""
    xwpe.menu("f", "n")
    time.sleep(0.5)                      # second window
    xwpe.menu("f", "n")
    time.sleep(0.5)                      # third window
    xwpe.menu("w", "a")
    time.sleep(0.7)                      # Cascade -> diagonal partial overlap
    shot = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died cascading windows"
    # Right edge: only the active window's vertical scrollbar may show.  Before
    # the fix the wider/offset covered window peeked a SECOND vertical bar to its
    # side.
    vbars = _right_vbar_groups(shot)
    assert vbars <= 1, \
        "a covered window's vertical scrollbar bled past the front window: " \
        "%d distinct right-edge bars (expected 1, the active window's)" % vbars
    # Bottom band: no tall stacked smear of covered windows' horizontal bars
    # (the suppressed covered bars leave at most the front window's single bar).
    rows = _bar_rows(shot, 0.86, 0.97)
    span = (rows[-1] - rows[0]) if rows else 0
    assert span <= 20, \
        "covered windows' horizontal scrollbars bled through the cascade: " \
        "bottom bars span %dpx (a single front-window scrollbar is ~16px)" % span
