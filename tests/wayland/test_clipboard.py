"""Native-Wayland OS-clipboard integration (wl_data_device).

A plain Copy in xwpe owns the Wayland selection as UTF-8, so the text is
pasteable in any other Wayland client; a Paste pulls in whatever another client
put on the selection.  Unlike the X11 twin (tests/x11/test_clipboard.py, which
reads the X selections with xclip), this drives the selection with wl-clipboard
(wl-copy / wl-paste) against the same headless weston the `xwpe` fixture uses.

Skips cleanly when wl-clipboard is not installed."""
import shutil
import subprocess
import time

import pytest

pytestmark = pytest.mark.skipif(
    shutil.which("wl-paste") is None or shutil.which("wl-copy") is None,
    reason="wl-clipboard (wl-copy/wl-paste) not installed")


def _wl_paste(env):
    out = subprocess.run(["wl-paste", "-n"], env=env, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, timeout=5)
    return out.stdout.decode("utf-8", "replace")


def _wl_copy(env, text):
    subprocess.run(["wl-copy", text], env=env, stderr=subprocess.DEVNULL, timeout=5)


def _wl_paste_primary(env):
    out = subprocess.run(["wl-paste", "--primary", "-n"], env=env,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=5)
    return out.stdout.decode("utf-8", "replace")


def _wl_copy_primary(env, text):
    subprocess.run(["wl-copy", "--primary", text], env=env,
                   stderr=subprocess.DEVNULL, timeout=5)


def _primary_supported(env):
    """True if the compositor exposes the primary selection (middle-click).
    Some compositors -- notably the headless weston used in CI -- ship without
    zwp_primary_selection_v1; xwpe degrades gracefully there (it just owns the
    CLIPBOARD), and the middle-click path is exercised on a real desktop."""
    r = subprocess.run(["wl-copy", "--primary", "--clear"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=5)
    return r.returncode == 0 and b"does not seem to support primary" not in r.stderr


def test_copy_owns_wayland_selection(xwpe):
    """^K X (Mark Whole) + ^C makes xwpe own the wl selection with the buffer text,
    readable by any other Wayland app (wl-paste)."""
    xwpe.key("ctrl+k")
    xwpe.key("x")            # Mark Whole
    xwpe.key("ctrl+c")       # Copy -> internal clipboard + wl selection
    time.sleep(0.6)
    got = _wl_paste(xwpe.env)
    assert xwpe.proc.poll() is None, "xwpe died on Copy"
    assert "int main" in got, \
        "wl-paste should read xwpe's copied buffer text, got %r" % got


def test_paste_pulls_external_selection(xwpe):
    """A wl-copy from another client -> ^V inserts that text into the buffer."""
    _wl_copy(xwpe.env, "WL_EXTERNAL_PASTE_MARKER")
    time.sleep(0.6)
    xwpe.key("ctrl+v")       # Paste reads the external wl selection
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on Paste"
    assert "WL_EXTERNAL_PASTE_MARKER" in xwpe.saved_text(), \
        "^V should insert the external wl selection, got %r" % xwpe.saved_text()


def test_copy_owns_primary_selection(xwpe):
    """^K X (Mark Whole) + ^C also makes xwpe own the PRIMARY selection, so a
    middle-click paste in a Wayland app gets the text -- matching the X11 backend
    which owns PRIMARY and CLIPBOARD together.  wl-paste --primary reads it."""
    if not _primary_supported(xwpe.env):
        pytest.skip("compositor lacks primary selection (zwp_primary_selection_v1)")
    xwpe.key("ctrl+k")
    xwpe.key("x")            # Mark Whole
    xwpe.key("ctrl+c")       # Copy -> internal + wl CLIPBOARD + wl PRIMARY
    time.sleep(0.6)
    got = _wl_paste_primary(xwpe.env)
    assert xwpe.proc.poll() is None, "xwpe died on Copy"
    assert "int main" in got, \
        "wl-paste --primary should read xwpe's copied text, got %r" % got


def test_paste_pulls_external_primary(xwpe):
    """Text placed only on the PRIMARY selection by another client is pasted by
    ^V: the reader falls back to PRIMARY after CLIPBOARD, like the X11 twin."""
    if not _primary_supported(xwpe.env):
        pytest.skip("compositor lacks primary selection (zwp_primary_selection_v1)")
    _wl_copy_primary(xwpe.env, "WL_PRIMARY_PASTE_MARKER")
    time.sleep(0.6)
    xwpe.key("ctrl+v")       # no CLIPBOARD owner -> reader falls back to PRIMARY
    xwpe.save()
    assert xwpe.proc.poll() is None, "xwpe died on Paste"
    assert "WL_PRIMARY_PASTE_MARKER" in xwpe.saved_text(), \
        "^V should fall back to the PRIMARY selection, got %r" % xwpe.saved_text()
