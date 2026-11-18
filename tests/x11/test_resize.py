"""Resizing the X11 window re-lays-out the editor and never crashes.

X11 handles resize in its ConfigureNotify handler (drain to the final size,
re-fit the grid, re-lay-out, repaint).  This drives a shrink/grow sweep of the
xwpe window, including a deep shrink where the bottom key-hint bar no longer
fits (the case whose off-grid extbyte write corrupted the heap on Wayland and
was latent here) -- xwpe must survive every size.

This is also the safety net for refactoring the (duplicated) ConfigureNotify
handler: it fails if a resize stops re-laying-out or starts crashing.

The back buffer has one owner (we_xbackbuf.c).  The editor has no buffer when
Xft finds no font (it then draws with the core font straight on the window) or
when the build has no Xft at all; the same sweep runs in that state too.
"""
import os
import time

import pytest

from conftest import HERE, _xdo, wait_for

# A fontconfig configuration with no font directory: Xft finds nothing, and
# the editor falls back to the core font.  Given to the editor only (the
# per-session environment), never to the X server or the test process.
NO_FONT_FONTCONFIG = os.path.join(HERE, "fontconfig-empty.conf")
NO_FONT_ENV = {"FONTCONFIG_FILE": NO_FONT_FONTCONFIG}

# The editor must still be running this long after its window is mapped: the
# first size the window manager gives it is what used to end it.
ALIVE_AFTER_MAP_S = 3.0
# A window the editor painted shows many pixels that are not the black of an
# unpainted one.
PAINTED_MIN_PIXELS = 2000
BLACK_LEVEL = 24


def _size(win, w, h):
    _xdo("windowsize", win, str(w), str(h))


def _sweep(xwpe):
    """Shrink deep and grow again twice, then settle at 1000x750."""
    xwpe.key("i")  # ensure the editor has focus / is interactive
    for rep in range(2):
        for w in range(1000, 360, -24):        # shrink deep (bottom bar overflows)
            _size(xwpe.win, w, int(w * 0.75))
            time.sleep(0.03)
        for w in range(360, 1000, 24):
            _size(xwpe.win, w, int(w * 0.75))
            time.sleep(0.03)
        assert xwpe.proc.poll() is None, "xwpe died during X11 resize (rep %d)" % rep
    _size(xwpe.win, 1000, 750)
    time.sleep(0.4)
    assert xwpe.proc.poll() is None, "xwpe died after X11 resize sweep"


def _painted_pixels(image):
    """How many pixels of the capture are not (nearly) black."""
    return sum(image.convert("L").histogram()[BLACK_LEVEL + 1:])


def test_x11_resize_sweep_survives(xwpe):
    _sweep(xwpe)


# @scenario SC-x11-backbuf-01
@pytest.mark.parametrize("xwpe", [NO_FONT_ENV], indirect=True)
def test_x11_starts_and_survives_resize_without_an_xft_font(xwpe):
    """Xft finds no font, so the editor draws with the core font and has no
    back buffer.  It used to end at start-up with BadDrawable (a copy from
    pixmap 0) at the first size the window manager gave it, and on the first
    resize under any other.  Now it stays up after its window is mapped, paints
    the window, and survives the same sweep."""
    time.sleep(ALIVE_AFTER_MAP_S)
    assert xwpe.proc.poll() is None, \
        "xwpe ended within %.0f s of its window being mapped" % ALIVE_AFTER_MAP_S
    wait_for(lambda: _painted_pixels(xwpe.screenshot()) > PAINTED_MIN_PIXELS,
             "the core-font editor to paint its window", 10.0)
    _sweep(xwpe)
