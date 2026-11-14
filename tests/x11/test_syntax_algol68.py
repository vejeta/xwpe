"""Algol 68 syntax highlighting in xwpe (X11/Xft), for BOTH dialects.

The pyte sibling (tests/test_syntax_algol68.py) proves the highlighting LOGIC
deterministically by reading each cell's SGR colour.  pyte cannot exercise
xwpe's real Xft glyph rendering, so this asserts the colour actually reaches the
screen in the X11 backend.

Method: render the SAME source twice -- once as ".a68" (syntax rules apply, the
reserved words get a highlight colour) and once as ".txt" (no syntax rule, every
glyph is drawn in the plain foreground).  The two files have identical text at
identical cell positions, so the glyph shapes and layout match pixel-for-pixel;
the ONLY thing that can differ in the editor text band is the colour of the
reserved words.  A non-trivial pixel difference there therefore proves the
keywords were highlighted.  Done for both stropping regimes (ga68 lowercase and
a68g UPPER) because syntax_def gives .a68 a case-INSENSITIVE keyword set.
"""
import os
import shutil
import subprocess
import time

import pytest

from conftest import (
    HERE, WINDOW_GONE_S, _find_xwpe_window, _force_window_geometry,
    _window_ids, launch_editor, XwpeSession,
)
from gui_owner import wait_for

# The repo's syntax_def carries the Algol 68 block.  xwpe reads
# $HOME/.xwpe/syntax_def first and the fixtures set HOME=tmp_path, so we drop
# the build-tree copy there -- otherwise xwpe would load a stale system install
# that predates the Algol 68 entry and nothing would highlight.
SYNTAX_DEF = os.path.normpath(os.path.join(HERE, "..", "..", "syntax_def"))

# Same program in each stropping regime; "result" is an ordinary identifier.
LOWER = (
    "begin\n"
    "   int result := 42;\n"
    "   puts (\"done'n\")\n"
    "end\n"
)
UPPER = (
    "BEGIN\n"
    "   INT result := 42;\n"
    "   print((result, newline))\n"
    "END\n"
)

# Editor text band: skip the top window-title border (where the .a68/.txt
# filename extension differs) and the bottom status line, leaving only the
# source text -- the region where a highlight colour change must show up.
TEXT_BAND = (0, 70, 1024, 680)


def _install_syntax(home):
    """Make xwpe under test use THIS repo's syntax_def (Algol 68 aware)."""
    xd = os.path.join(home, ".xwpe")
    os.makedirs(xd, exist_ok=True)
    shutil.copyfile(SYNTAX_DEF, os.path.join(xd, "syntax_def"))


def _launch(owner, request, tmp_path, fname, text):
    """Start xwpe editing tmp_path/fname containing `text`; return the session.

    The launcher gives the editor the same environment as the conftest
    fixture (no eager language server, which would pop a Messages window
    mid-test and change pixels in the text band diffed for colour; the
    suite's display only, on a Wayland host too).  Its end is registered
    with the row before the window is waited for."""
    _install_syntax(str(tmp_path))
    src = tmp_path / fname
    src.write_text(text)
    owned = launch_editor(owner, tmp_path, request, [str(src)])
    win = _find_xwpe_window()
    assert win, "xwpe window did not appear for %s" % fname
    _force_window_geometry(win)
    time.sleep(1.0)
    session = XwpeSession(owned.proc, win, owned)
    session.srcfile = str(src)
    session.focus()
    return session


def _shoot_band(session):
    """Screenshot the editor text band as a Pillow image."""
    return session.screenshot().crop(TEXT_BAND)


def _colour_changed(img_a, img_b, thresh=60):
    """Pixels whose colour differs by more than `thresh` in ANY channel.

    NOT the grayscale changed_pixels() helper: a highlight can change a glyph's
    HUE while barely moving its luminance (e.g. light-grey identifier -> white or
    yellow keyword differ by only ~44 in grey but strongly in colour), so a
    luminance diff misses it.  Per-channel max catches the hue shift.  Measured
    floor for two non-highlighted renders of the same text is 0, so any sizable
    count is real syntax colour."""
    from PIL import ImageChops
    diff = ImageChops.difference(img_a, img_b)
    r, g, b = diff.split()
    peak = ImageChops.lighter(ImageChops.lighter(r, g), b)   # per-pixel max channel
    return peak.point(lambda p: 255 if p > thresh else 0).histogram()[255]


def _close(session):
    """End xwpe through the owner AND wait for its window to leave the
    server, so the next launch's `xdotool search` cannot latch onto this
    dying window (a stale-window race that makes the before/after
    screenshots identical)."""
    session.owned.end()
    wait_for(lambda: not _window_ids(),
             "the ended editor's window to leave the display", WINDOW_GONE_S,
             state_fn=_window_ids)


def _assert_highlight(owner, request, tmp_path, text, tag):
    """Render `text` as .a68 (highlighted) and .txt (plain); the text band must
    differ, proving xwpe coloured the reserved words."""
    hl = _launch(owner, request, tmp_path / (tag + "a"), "hi.a68", text)
    try:
        band_a68 = _shoot_band(hl)
    finally:
        _close(hl)
    plain = _launch(owner, request, tmp_path / (tag + "b"), "hi.txt", text)
    try:
        band_txt = _shoot_band(plain)
    finally:
        _close(plain)
    diff = _colour_changed(band_a68, band_txt)
    assert diff > 80, (
        "Algol 68 (%s) keywords are not highlighted in xwpe: the .a68 text band "
        "is nearly identical to the plain .txt rendering (colour-changed pixels=%d, "
        "floor for no-highlight is 0)" % (tag, diff))


@pytest.fixture
def homes(tmp_path):
    """Two separate HOME dirs so the .a68 and .txt runs do not share state."""
    (tmp_path / "lowera").mkdir()
    (tmp_path / "lowerb").mkdir()
    (tmp_path / "uppera").mkdir()
    (tmp_path / "upperb").mkdir()
    return tmp_path


def test_highlight_ga68_lowercase(xserver, gui_owner, request, homes):
    """Modern (ga68) stropping: lowercase begin/int/puts highlight in Xft."""
    _assert_highlight(gui_owner, request, homes, LOWER, "lower")


def test_highlight_a68g_uppercase(xserver, gui_owner, request, homes):
    """Classic (a68g) stropping: UPPER BEGIN/INT highlight (case-insensitive)."""
    _assert_highlight(gui_owner, request, homes, UPPER, "upper")
