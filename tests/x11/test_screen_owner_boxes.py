"""The screen has one owner, on X11: a box is laid out again after a resize.

The terminal row (tests/test_screen_owner_boxes.py) reads the cells; this one
reads the pixels the graphical backend draws.  A box is open when the window
shrinks: the display draws the desk again at the new size and tells the box,
which draws itself again over it.  Once Esc closes the box, the window shows
the desk the editor draws at that size, not the pixels saved behind the box
before the resize.

The Wayland twin (tests/wayland/test_screen_owner_boxes.py) runs the same
body.
"""
import os
import subprocess

from conftest import changed_pixels, _sleep, xtool_env

# Two sizes below every size a row starts at (the harness's screen, or the
# size the compositor's output kept from an earlier row); the smaller one
# still has room for the About box.
START_W, START_H = 1024, 768      # the size every graphical row starts at
FULL_W, FULL_H = 880, 620
SMALL_W, SMALL_H = 800, 560
SETTLE_S = 1.5
CLOSE_S = 0.8
MENU_BAR_PX = 18                  # one text row at the font size the harness pins

# What each box is opened with.
BOXES = {
    "the File dropdown": ("alt+f",),
    "the About box": ("alt+f", "Left", "a"),
}


def _xdo(*args):
    """xdotool on the display the editor's window is on (the X server, or
    the one the compositor's window is on)."""
    subprocess.run(["xdotool", *args], env=xtool_env(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def _window(xwpe):
    """The window the session drives (X11 and Wayland name it apart)."""
    return getattr(xwpe, "win", None) or xwpe.wid


def _size(xwpe, w, h):
    _xdo("windowsize", _window(xwpe), str(w), str(h))
    _sleep(SETTLE_S)


def _let_the_window_be_sized(xwpe):
    """The window manager of the X11 harness keeps an application window
    full-screen and puts back any other size at once; a window it does not
    manage keeps the size it is given.  Unmapped and mapped again as such,
    the editor sees the size changes a user would make."""
    if getattr(xwpe, "win", None) is None:
        xwpe.key("Escape", delay=SETTLE_S)   # Wayland: the editor is up
        return
    _xdo("set_window", "--overrideredirect", "1", xwpe.win)
    _xdo("windowunmap", "--sync", xwpe.win)
    _xdo("windowmap", "--sync", xwpe.win)
    _sleep(SETTLE_S)
    xwpe.focus()


def _where(a, b):
    """The box around the pixels that differ, for the failure message."""
    from PIL import ImageChops
    return ImageChops.difference(a, b).getbbox()


def _resized(before, after):
    """Did the editor take the new size?  An X11 screenshot is the whole
    screen, where the window's pixels change; a Wayland one is the editor's
    own buffer, which changes size."""
    if before.size != after.size:
        return True
    return changed_pixels(before, after) > 0


def _box_region(without, with_box):
    """Where the box is: the pixels it changes below the menu bar.  The bar
    row and the windows' scrollbar overlay are left out: the bar marks the
    open menu, and the overlay waits while a box is up, so both differ for
    reasons of their own."""
    width, height = with_box.size
    below_bar = (0, MENU_BAR_PX, width, height)
    found = _where(without.crop(below_bar), with_box.crop(below_bar))
    if found is None:
        return None
    x1, y1, x2, y2 = found
    return (x1, y1 + MENU_BAR_PX, x2, y2 + MENU_BAR_PX)


def _open(xwpe, keys):
    xwpe.key(*keys, delay=0.6)
    _sleep(SETTLE_S)


# @scenario SC-screen-04
def test_resize_under_a_box_repaints_the_desk_and_the_box(xwpe):
    """Each box is drawn again after the shrink exactly as it is drawn when
    opened at that size, and once it closes the window is the desk drawn
    afresh at that size.  The boxes are taken in one editor: the
    compositor's output outlives a row, and an editor started at an output
    an earlier row resized does not follow the next size change."""
    _let_the_window_be_sized(xwpe)
    try:
        for box in sorted(BOXES):
            _size(xwpe, FULL_W, FULL_H)
            _resize_under(xwpe, box)
    finally:
        # The rows after this one measure pixels at the harness's size.
        _size(xwpe, START_W, START_H)


def _resize_under(xwpe, box):
    """The body of the row, from the larger size."""
    keys = BOXES[box]
    full = xwpe.screenshot()
    _size(xwpe, SMALL_W, SMALL_H)
    fresh = xwpe.screenshot()
    assert _resized(full, fresh), \
        "the window did not change size (%s, then %s): the row would prove " \
        "nothing" % (full.size, fresh.size)
    _size(xwpe, FULL_W, FULL_H)
    _open(xwpe, keys)
    _size(xwpe, SMALL_W, SMALL_H)
    assert xwpe.proc.poll() is None, "xwpe died on the resize under %s" % box
    under = xwpe.screenshot()
    xwpe.key("Escape", delay=CLOSE_S)
    _sleep(SETTLE_S)
    closed = xwpe.screenshot()
    assert changed_pixels(under, closed) > 0, \
        "%s was not on the window after the resize" % box
    assert changed_pixels(closed, fresh) == 0, \
        "%d pixels differ between the window after %s closed and the desk " \
        "drawn afresh at that size, within %s" \
        % (changed_pixels(closed, fresh), box, _where(closed, fresh))
    _open(xwpe, keys)
    reopened = xwpe.screenshot()
    region = _box_region(closed, reopened)
    assert region is not None, "%s did not open again" % box
    assert changed_pixels(under.crop(region), reopened.crop(region)) == 0, \
        "%d pixels of %s differ between the box drawn again after the " \
        "resize and the box opened at that size, within %s" \
        % (changed_pixels(under.crop(region), reopened.crop(region)), box,
           region)
    xwpe.key("Escape", delay=CLOSE_S)
    assert xwpe.proc.poll() is None, "xwpe died closing %s" % box
