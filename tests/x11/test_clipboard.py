"""X11 OS-clipboard integration: a plain Copy in xwpe owns both PRIMARY and
CLIPBOARD as UTF-8, so the text is pasteable in any other app (Ctrl-V in
GTK/Qt, middle-click for PRIMARY).  We drive a real xwpe under the headless X
server (the `xwpe` fixture) and read the selections back with `xclip`.

Skips cleanly when `xclip` is not installed.
"""
import os
import shutil
import subprocess
import time

import pytest

from conftest import DISPLAY

pytestmark = pytest.mark.skipif(shutil.which("xclip") is None,
                                reason="xclip not installed")


def _xclip(selection, target=None):
    cmd = ["xclip", "-selection", selection, "-o"]
    if target:
        cmd += ["-t", target]
    out = subprocess.run(cmd, env={**os.environ, "DISPLAY": DISPLAY},
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return out.stdout.decode("utf-8", "replace")


def _mark_whole_and_copy(xwpe):
    xwpe.key("ctrl+k")      # WordStar block prefix
    xwpe.key("x")           # ^K X -> Mark Whole
    xwpe.key("ctrl+c")      # Copy -> internal + OS clipboard (PRIMARY + CLIPBOARD)
    time.sleep(0.6)


def test_copy_owns_clipboard_and_primary(xwpe):
    """^C makes xwpe the owner of both CLIPBOARD and PRIMARY with the text."""
    _mark_whole_and_copy(xwpe)
    clip = _xclip("clipboard")
    prim = _xclip("primary")
    assert "int main" in clip, \
        "CLIPBOARD should hold the copied buffer, got %r" % clip
    assert clip == prim, \
        "PRIMARY and CLIPBOARD should carry the same text (%r vs %r)" % (prim, clip)


def test_copy_advertises_utf8_in_targets(xwpe):
    """TARGETS must list UTF8_STRING so GTK/Qt apps (which probe TARGETS first)
    can fetch the selection -- the gap that made the old STRING-only path fail."""
    _mark_whole_and_copy(xwpe)
    targets = _xclip("clipboard", target="TARGETS")
    assert "UTF8_STRING" in targets, \
        "TARGETS should advertise UTF8_STRING, got %r" % targets


def test_paste_pulls_from_os_clipboard(xwpe):
    """^V pastes what ANOTHER app put on the CLIPBOARD (the merged model).
    Seed the OS clipboard from outside xwpe, paste, then copy the buffer back
    out and read it -- if the seeded text is there, the paste landed."""
    subprocess.run(["xclip", "-selection", "clipboard"], input=b"OSPASTE_42",
                   env={**os.environ, "DISPLAY": DISPLAY})
    time.sleep(0.3)
    xwpe.key("ctrl+v")          # paste from the OS clipboard into the editor
    _mark_whole_and_copy(xwpe)  # copy the buffer back out
    got = _xclip("clipboard")
    assert "OSPASTE_42" in got, \
        "Paste should pull the external OS clipboard, got %r" % got
