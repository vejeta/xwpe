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

from conftest import changed_pixels
from test_selection_keys import (SAME_FIELD_PIXELS, close_dialog,
                                 copy_two_lines, field_image, open_find)

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


# @scenario SC-line-endings-30
def test_paste_of_windows_line_breaks_leaves_no_carriage_returns(xwpe):
    """Text another client copied with CR LF line breaks arrives as lines,
    with no stray carriage return at the end of a row."""
    _wl_copy(xwpe.env, "X\r\nY\r\n")
    time.sleep(0.6)
    xwpe.key("ctrl+v")
    xwpe.save()
    with open(xwpe.srcfile, "rb") as fh:
        saved = fh.read()
    assert saved.startswith(b"X\nYint main"), saved[:20]
    assert b"\r" not in saved[:12], saved[:20]


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


def _mark_first_word(xwpe):
    """Mark "int", the first three characters of the first line."""
    xwpe.key("ctrl+k"); xwpe.key("b")
    xwpe.key("Right"); xwpe.key("Right"); xwpe.key("Right")
    xwpe.key("ctrl+k"); xwpe.key("k")


def _own_both(env, text):
    """Make another client own the selection, and PRIMARY too where the
    compositor has one, so the editor owns neither."""
    _wl_copy(env, text)
    if _primary_supported(env):
        _wl_copy_primary(env, text)
    time.sleep(0.4)


# @scenario SC-display-02
def test_alt_delete_owns_the_wayland_selection(xwpe):
    """Alt-Delete copies the marked block and owns the Wayland selection with
    it; the editor does not end (the key used to call a slot Wayland left
    empty)."""
    _mark_first_word(xwpe)
    xwpe.key("alt+Delete", delay=0.6)
    time.sleep(0.4)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Delete"
    assert _wl_paste(xwpe.env) == "int", \
        "wl-paste should read the block, got %r" % _wl_paste(xwpe.env)


# @scenario SC-display-02
def test_alt_insert_pastes_an_external_selection(xwpe):
    """Alt-Insert pastes what another client put on the selection."""
    _own_both(xwpe.env, "WLALTINSERT_7")
    xwpe.key("alt+Insert", delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Insert"
    xwpe.save()
    assert xwpe.saved_text().startswith("WLALTINSERT_7int main"), \
        "Alt-Insert should paste the external selection, got %r" % \
        xwpe.saved_text()[:40]


# @scenario SC-display-03
def test_alt_insert_in_a_field_takes_an_external_selection_whole(xwpe):
    """The field pastes an external selection in full, whatever length the
    editor's own clipboard had."""
    external = "external text that is long"
    copy_two_lines(xwpe)               # the editor's clipboard: an 11 byte line
    _own_both(xwpe.env, external)
    dialog = open_find(xwpe)
    xwpe.key("alt+Insert", delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Insert in a field"
    pasted = field_image(xwpe, dialog)
    close_dialog(xwpe)
    open_find(xwpe)
    xwpe.type(external, delay=0.6)
    typed = field_image(xwpe, dialog)
    close_dialog(xwpe)
    assert changed_pixels(pasted, typed) < SAME_FIELD_PIXELS, \
        "the field should hold all of %r" % external


# @scenario SC-display-02
@pytest.mark.parametrize("paste_key", ["ctrl+v", "alt+Insert"])
def test_paste_ignores_another_clients_primary_while_the_editor_owns_the_clipboard(
        xwpe, paste_key):
    """The editor copies, so it owns the CLIPBOARD; another client then takes
    PRIMARY.  Paste uses the editor's own text, as it does on X11, and not
    the other client's PRIMARY."""
    if not _primary_supported(xwpe.env):
        pytest.skip("compositor lacks primary selection (zwp_primary_selection_v1)")
    _mark_first_word(xwpe)
    xwpe.key("ctrl+Insert", delay=0.6)       # Copy: the editor owns CLIPBOARD
    time.sleep(0.4)
    _wl_copy_primary(xwpe.env, "OTHERPRIMARY")
    time.sleep(0.4)
    xwpe.key("End", paste_key, delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on the paste"
    xwpe.save()
    line = xwpe.saved_text().splitlines()[0]
    assert line == "int main(){int", \
        "paste should use the editor's text, the first line is %r" % line


# @scenario SC-display-02
@pytest.mark.parametrize("paste_key", ["ctrl+v", "alt+Insert"])
def test_paste_takes_a_copy_made_in_another_application_after_the_editors(
        xwpe, paste_key):
    """The editor copies, then a browser copies with Ctrl-C and takes
    CLIPBOARD; where the compositor has a primary selection the editor still
    owns PRIMARY.  Paste inserts the browser's text, the newest copy.  A
    compositor without a primary selection (the headless weston) cannot put
    the editor in the state that failed; running the editor under gdb with
    g_pclip_we_own set to 1 at the start of e_clip_w_get shows it there."""
    _mark_first_word(xwpe)
    xwpe.key("ctrl+Insert", delay=0.6)       # Copy: the editor owns CLIPBOARD
    time.sleep(0.4)
    _wl_copy(xwpe.env, "BROWSERCOPY_9")      # another client takes CLIPBOARD
    time.sleep(0.4)
    xwpe.key("End", paste_key, delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on the paste"
    xwpe.save()
    line = xwpe.saved_text().splitlines()[0]
    assert line == "int main(){BROWSERCOPY_9", \
        "paste should insert the browser's text, the first line is %r" % line
