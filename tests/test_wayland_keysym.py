"""Wayland key translation: typed multibyte characters decode to their codepoint
AND are flagged as characters (e_input_was_char), even when the codepoint equals
an xwpe key code by value (U+0130 == 304 == Alt-N).

The X11 and ncurses typing paths are covered end to end by driving a real key
event (tests/x11/test_utf8_typing.py, tests/test_utf8_typing.py).  The Wayland
GUI harness cannot: xdotool injects a non-ASCII glyph by remapping a spare
keycode, and that remap does not survive weston's own xkb keymap -- the key
reaches xwpe as sym 0 / u8len 0, so no character is ever delivered.  So the
Wayland peer is checked through xwpe's built-in, compositor-free keysym selftest
(XWPE_WL_KEYTEST), which drives keysym_to_xwpe directly and exits non-zero on any
mismatch -- including the is-char flag for the multibyte and Alt-key cases.
"""
import os
import subprocess
import pytest
from wpe_driver import WPE_BIN

# The keysym selftest lives on the graphical startup path, so it runs only when
# xwpe is invoked under its X-window name (`xwpe`), not the console `wpe`.
XWPE_BIN = os.path.join(os.path.dirname(os.path.abspath(WPE_BIN)), "xwpe")


def _wayland_build():
    """True if the binary carries the Wayland backend (the selftest string)."""
    try:
        out = subprocess.run(["strings", XWPE_BIN],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"keysym_to_xwpe selftest" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _wayland_build(),
                                reason="built without the Wayland backend")


def test_wayland_keysym_selftest():
    # Force the Wayland path so WpeWaylandInit runs the selftest; it _exit()s
    # before touching a compositor, so no WAYLAND_DISPLAY is required.
    env = dict(os.environ, XWPE_BACKEND="wayland", XWPE_WL_KEYTEST="1")
    env.pop("WAYLAND_DISPLAY", None)
    r = subprocess.run([XWPE_BIN], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=30)
    out = r.stdout.decode("utf-8", "replace")
    assert r.returncode == 0, "keysym selftest failed:\n" + out
    # Prove the multibyte + collision cases actually ran and passed.
    for needle in ("U+0130 (=Alt-N)", "CJK U+65E5", "Alt+n is not a char"):
        assert needle in out and "FAIL" not in out, \
            "selftest missing/failed case %r:\n%s" % (needle, out)
