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
    DISPLAY, WINDOW_NAME, XWPE_BIN, HERE,
    _spawn, XwpeSession,
)

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


def _launch(xserver, tmp_path, fname, text):
    """Start xwpe editing tmp_path/fname containing `text`; return the session."""
    _install_syntax(str(tmp_path))
    src = tmp_path / fname
    src.write_text(text)
    proc = _spawn([XWPE_BIN, str(src)],
                  env={**os.environ, "DISPLAY": DISPLAY, "HOME": str(tmp_path)},
                  cwd=str(tmp_path))
    win = None
    for _ in range(40):
        out = subprocess.run(["xdotool", "search", "--name", WINDOW_NAME],
                             env={**os.environ, "DISPLAY": DISPLAY},
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        ids = out.stdout.decode().split()
        if ids:
            win = ids[0]
            break
        time.sleep(0.25)
    assert win, "xwpe window did not appear for %s" % fname
    time.sleep(1.0)
    session = XwpeSession(proc, win)
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


def _windows():
    out = subprocess.run(["xdotool", "search", "--name", WINDOW_NAME],
                         env={**os.environ, "DISPLAY": DISPLAY},
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return out.stdout.decode().split()


def _close(session):
    """Terminate xwpe AND wait for its window to leave the server, so the next
    launch's `xdotool search` cannot latch onto this dying window (a stale-window
    race that makes the before/after screenshots identical)."""
    try:
        session.proc.terminate()
        session.proc.wait(timeout=5)
    except Exception:
        session.proc.kill()
    for _ in range(40):
        if not _windows():
            return
        time.sleep(0.1)


def _assert_highlight(xserver, tmp_path, text, tag):
    """Render `text` as .a68 (highlighted) and .txt (plain); the text band must
    differ, proving xwpe coloured the reserved words."""
    hl = _launch(xserver, tmp_path / (tag + "a"), "hi.a68", text)
    try:
        band_a68 = _shoot_band(hl)
    finally:
        _close(hl)
    plain = _launch(xserver, tmp_path / (tag + "b"), "hi.txt", text)
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


def test_highlight_ga68_lowercase(xserver, homes):
    """Modern (ga68) stropping: lowercase begin/int/puts highlight in Xft."""
    _assert_highlight(xserver, homes, LOWER, "lower")


def test_highlight_a68g_uppercase(xserver, homes):
    """Classic (a68g) stropping: UPPER BEGIN/INT highlight (case-insensitive)."""
    _assert_highlight(xserver, homes, UPPER, "upper")
