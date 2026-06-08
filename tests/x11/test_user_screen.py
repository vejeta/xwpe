"""Alt-F5 "User Screen" tests for xwpe (X11/Xft) -- the embedded VT terminal.

The console backend (wpe) hands the real terminal back to the program for the
Borland Alt-F5 User Screen.  The X11 backend (xwpe) has no terminal to drop to,
so it INTERPRETS the captured program output with libvterm and paints the
resulting cell grid inside the xwpe window (e_x_vterm_user_screen, we_vterm.c).

Flow exercised here: open a program that PAINTS its screen (paint.c -- ANSI
colour + cursor addressing), Ctrl-F9 to build+run (which captures the output),
then Alt-F5.  The painted colours (pure red AND pure green, which never appear
in the dark-blue editor chrome) must show up, proving the VT terminal rendered
the program output rather than just focusing the line-oriented Messages panel.
A keypress must then restore the editor.

Requires libvterm (HAVE_VTERM); without it Alt-F5 falls back to the Messages
panel and this test is skipped.
"""
import os
import shutil
import subprocess
import time

import pytest

from conftest import (
    DISPLAY, WINDOW_NAME, XWPE_BIN, HERE,
    _spawn, XwpeSession, changed_pixels,
)

SAMPLE = os.path.normpath(os.path.join(HERE, "..", "samples", "paint.c"))


def _has_vterm():
    """True if the xwpe binary was linked against libvterm."""
    out = subprocess.run(["sh", "-c", "ldd %s 2>/dev/null" % XWPE_BIN],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return b"libvterm" in out.stdout


pytestmark = pytest.mark.skipif(
    not _has_vterm(), reason="xwpe not built with libvterm (HAVE_VTERM)")


def _count_hue(img, hue, step=2):
    """Count strongly red/green pixels (sampled every `step` px).

    Hue-based rather than exact-match: the program's ANSI red is mapped to the
    NEAREST xwpe palette entry (Red3 = (205,0,0), not pure (255,0,0)) and the
    glyphs are anti-aliased, so we look for a dominant channel with the other
    two suppressed.  Neither hue appears in the dark-slate-blue/grey editor
    chrome, so a non-zero count proves the program's screen was painted."""
    px = img.load()
    w, h = img.size
    n = 0
    for y in range(0, h, step):
        for x in range(0, w, step):
            r, g, b = px[x, y][:3]
            if hue == "red" and r > 120 and g < 80 and b < 80:
                n += 1
            elif hue == "green" and g > 120 and r < 80 and b < 80:
                n += 1
    return n


@pytest.fixture
def paint_xwpe(xserver, tmp_path):
    """Launch xwpe editing a copy of the ANSI-painting sample (paint.c)."""
    assert os.path.exists(SAMPLE), "missing sample %s" % SAMPLE
    src = tmp_path / "paint.c"
    shutil.copyfile(SAMPLE, src)
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
    assert win, "xwpe window did not appear"
    time.sleep(1.0)
    session = XwpeSession(proc, win)
    session.srcfile = str(src)
    session.focus()
    yield session
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def _run_program(sess):
    """Ctrl-F9: build + run paint.c, capturing its output.  Wait for the
    executable to land on disk so the run has actually happened."""
    d = os.path.dirname(sess.srcfile)
    exe = os.path.join(d, "paint.e")
    sess.key("ctrl+F9")
    deadline = time.time() + 25.0
    while time.time() < deadline:
        assert sess.proc.poll() is None, "xwpe died during Ctrl-F9 Run"
        if os.path.exists(exe):
            break
        time.sleep(0.25)
    assert os.path.exists(exe), "Ctrl-F9 should build+run paint.c (no paint.e)"
    time.sleep(1.5)   # let the pty drain into e_d_prog_output


def test_user_screen_renders_program_colours(paint_xwpe):
    """Alt-F5 paints the program's ANSI screen (red + green) via libvterm."""
    sess = paint_xwpe
    _run_program(sess)

    before = sess.screenshot()
    sess.key("alt+F5")               # User Screen
    time.sleep(1.0)
    after = sess.screenshot()
    assert sess.proc.poll() is None, "xwpe died during Alt-F5 User Screen"

    assert changed_pixels(before, after) > 3000, \
        "Alt-F5 should replace the editor with the program's screen"
    # The editor chrome (dark slate blue + grey + black) has zero red/green
    # hue pixels, so a clear non-zero count of BOTH proves the program's own
    # coloured screen was painted.  The "red" label is only three glyphs, so
    # the floor stays low while remaining far above the chrome's zero.
    red = _count_hue(after, "red")
    green = _count_hue(after, "green")
    assert red > 5 and green > 5, \
        "User Screen should show the program's ANSI red+green " \
        "(red=%d green=%d)" % (red, green)


def test_user_screen_key_returns_to_editor(paint_xwpe):
    """A keypress dismisses the User Screen and restores the editor."""
    sess = paint_xwpe
    _run_program(sess)

    editor = sess.screenshot()
    sess.key("alt+F5")
    time.sleep(1.0)
    user_screen = sess.screenshot()
    assert changed_pixels(editor, user_screen) > 3000, \
        "Alt-F5 should switch away from the editor"

    sess.key("Return")               # any key returns
    time.sleep(1.0)
    restored = sess.screenshot()
    assert sess.proc.poll() is None, "xwpe died returning from User Screen"
    assert changed_pixels(user_screen, restored) > 3000, \
        "A keypress should leave the User Screen"
    assert changed_pixels(editor, restored) < changed_pixels(editor, user_screen), \
        "The restored screen should resemble the editor again"


def _nonblack_in_band(img, y0, y1, floor=90, step=2):
    """Count pixels brighter than `floor` (R+G+B) in the horizontal band
    [y0,y1) across the full width.  Used to detect chrome that bled into a
    region the User Screen leaves blank."""
    px = img.load()
    w, h = img.size
    n = 0
    for y in range(max(0, y0), min(h, y1), step):
        for x in range(0, w, step):
            r, g, b = px[x, y][:3]
            if r + g + b > floor:
                n += 1
    return n


def test_user_screen_has_no_scrollbar_bleed(paint_xwpe):
    """The User Screen owns the whole window: the editor's and Messages' fluid
    scrollbars (drawn by wpe_render_chrome on every refresh) must NOT bleed on
    top of the program's painted screen.  paint.c only draws its demo in the
    top rows, so the band below it -- and above the bottom 'press any key'
    prompt -- is blank black.  A bled scrollbar (mid-screen horizontal thumb,
    right-edge vertical tracks, bottom Messages bar) lights up that band with
    hundreds-to-thousands of non-black pixels; suppressed, it is ~zero."""
    sess = paint_xwpe
    _run_program(sess)
    sess.key("alt+F5")
    time.sleep(1.0)
    us = sess.screenshot()
    assert sess.proc.poll() is None, "xwpe died during Alt-F5 User Screen"

    w, h = us.size
    bleed = _nonblack_in_band(us, 220, h - 30)
    assert bleed < 200, \
        "scrollbar/chrome bled into the User Screen (%d non-black px in the " \
        "blank band; expected ~0)" % bleed
