"""The editor's text is painted in the colormap its colours were allocated in.

With the pcmap resource on, the editor makes a private colormap for its 16
colours.  The glyph surface (the XftDraw) must be made in that same colormap,
or on a palette display the text comes out in the wrong colours.

On the 24-bit test display the two colormaps paint identical pixels, so no
screenshot can tell them apart: the strongest assertion available is the one
the editor writes to $XWPE_UI_TRACE each time it makes the glyph surface, tied
to the colormap the server reports for the window (xwininfo).  The rows launch
their own editor because the shared fixture cannot set a resource.
"""
import contextlib
import os
import re
import subprocess

import pytest

import conftest
from conftest import _spawn, _find_xwpe_window, _sleep

TRACE = "ui.trace"
LINE = re.compile(r"^xftdraw colormap=(\d+) private=(\d)$", re.M)
WININFO_CMAP = re.compile(r"Colormap:\s+(0x[0-9a-fA-F]+)")


def _stop(proc):
    """End the editor and collect it, whatever state it is in."""
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
        proc.wait()


@contextlib.contextmanager
def _editor(xserver, tmp_path, resources):
    """Run xwpe with `resources` in its ~/.Xdefaults; yield its window.

    The editor is ended and collected on the way out, also when its window
    never appears."""
    src = tmp_path / "t.c"
    src.write_text("int main(void){return 0;}\n")
    (tmp_path / ".Xdefaults").write_text("xwpe.altMask: mod1\n" + resources)
    env = {**os.environ, "DISPLAY": conftest.DISPLAY, "HOME": str(tmp_path),
           "XWPE_LSP_NO_EAGER": "1", "XWPE_FONT_SIZE": "10",
           "XWPE_BACKEND": "x11",
           "XWPE_UI_TRACE": str(tmp_path / TRACE)}
    proc = _spawn([conftest.XWPE_BIN, str(src)], env=env, cwd=str(tmp_path))
    try:
        win = _find_xwpe_window()
        assert win, "xwpe window did not appear"
        _sleep(1.0)
        yield win
    finally:
        _stop(proc)


def _glyph_surfaces(tmp_path):
    try:
        text = (tmp_path / TRACE).read_text()
    except OSError:
        return []
    return [(int(m.group(1)), int(m.group(2))) for m in LINE.finditer(text)]


def _window_colormap(win):
    out = subprocess.run(["xwininfo", "-id", win],
                         env={**os.environ, "DISPLAY": conftest.DISPLAY},
                         stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL).stdout.decode()
    m = WININFO_CMAP.search(out)
    assert m, "xwininfo gave no colormap for the window: %r" % out
    return int(m.group(1), 16)


# @scenario SC-display-05
def test_text_is_painted_in_the_private_colormap(xserver, tmp_path):
    with _editor(xserver, tmp_path, "xwpe.pcmap: on\n") as win:
        surfaces = _glyph_surfaces(tmp_path)
        assert surfaces, "the editor never said which colormap its text uses"
        assert all(private == 1 for _, private in surfaces), \
            "text was made in the default colormap: %r" % surfaces
        window_cmap = _window_colormap(win)
        assert all(cmap == window_cmap for cmap, _ in surfaces), \
            "text colormap %r is not the window's 0x%x" % (surfaces, window_cmap)


# @scenario SC-display-05
def test_text_is_painted_in_the_default_colormap_without_pcmap(xserver, tmp_path):
    with _editor(xserver, tmp_path, ""):
        surfaces = _glyph_surfaces(tmp_path)
        assert surfaces, "the editor never said which colormap its text uses"
        assert all(private == 0 for _, private in surfaces), \
            "text got a private colormap nobody asked for: %r" % surfaces
