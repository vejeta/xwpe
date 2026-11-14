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

from conftest import changed_pixels, xtool_env
from test_selection_keys import (SAME_FIELD_PIXELS, close_dialog,
                                 copy_two_lines, field_image, open_find)

pytestmark = pytest.mark.skipif(shutil.which("xclip") is None,
                                reason="xclip not installed")


def _xclip(selection, target=None):
    cmd = ["xclip", "-selection", selection, "-o"]
    if target:
        cmd += ["-t", target]
    out = subprocess.run(cmd, env=xtool_env(),
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
                   env=xtool_env())
    time.sleep(0.3)
    xwpe.key("ctrl+v")          # paste from the OS clipboard into the editor
    _mark_whole_and_copy(xwpe)  # copy the buffer back out
    got = _xclip("clipboard")
    assert "OSPASTE_42" in got, \
        "Paste should pull the external OS clipboard, got %r" % got


# @scenario SC-line-endings-29
def test_paste_of_windows_line_breaks_leaves_no_carriage_returns(xwpe):
    """Text another application copied with CR LF line breaks arrives as
    lines: the paste reads each CR LF as one break, so no row of the text
    ends in a stray carriage return that would be saved with it."""
    subprocess.run(["xclip", "-selection", "clipboard"], input=b"X\r\nY\r\n",
                   env=xtool_env())
    time.sleep(0.3)
    xwpe.key("ctrl+v")
    xwpe.save()
    with open(xwpe.srcfile, "rb") as fh:
        saved = fh.read()
    assert saved.startswith(b"X\nYint main"), saved[:20]
    assert b"\r" not in saved[:12], saved[:20]


def _xclip_own(selection, text):
    """Make another client own `selection` with `text` (xclip stays behind as
    the owner until something else takes the selection)."""
    subprocess.run(["xclip", "-selection", selection], input=text.encode(),
                   env=xtool_env())


def _mark_first_word(xwpe):
    """Mark "int", the first three characters of the first line."""
    xwpe.key("ctrl+k"); xwpe.key("b")
    xwpe.key("Right"); xwpe.key("Right"); xwpe.key("Right")
    xwpe.key("ctrl+k"); xwpe.key("k")


# @scenario SC-display-02
def test_alt_delete_owns_primary_and_clipboard(xwpe):
    """Alt-Delete copies the block through the one shared copy: the editor
    owns PRIMARY and CLIPBOARD with the block's bytes.  A guard: the same
    before and after the key stopped having a copy of its own."""
    _mark_first_word(xwpe)
    xwpe.key("alt+Delete", delay=0.6)
    time.sleep(0.4)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Delete"
    assert _xclip("clipboard") == "int", \
        "CLIPBOARD should hold the block, got %r" % _xclip("clipboard")
    assert _xclip("primary") == "int", \
        "PRIMARY should hold the block, got %r" % _xclip("primary")


# @scenario SC-display-02
def test_alt_delete_of_whole_lines_exports_them_without_the_last_newline(xwpe):
    """A block that ends at the start of a line exports without that last
    line break, as Ctrl-Insert has always exported it.  Alt-Delete used to
    rebuild the text from the editor's buffer and kept the break:
    "int main(){\\n  int a = 1;\\n" became "int main(){\\n  int a = 1;"."""
    xwpe.key("ctrl+k"); xwpe.key("b")
    xwpe.key("Down"); xwpe.key("Down")
    xwpe.key("ctrl+k"); xwpe.key("k")
    xwpe.key("alt+Delete", delay=0.6)
    time.sleep(0.4)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Delete"
    assert _xclip("clipboard") == "int main(){\n  int a = 1;", \
        "CLIPBOARD should hold the two lines, got %r" % _xclip("clipboard")


# @scenario SC-display-02
def test_alt_insert_pastes_an_external_selection(xwpe):
    """Alt-Insert pastes what another application put on the selection."""
    _xclip_own("clipboard", "OSALTINSERT_7")
    _xclip_own("primary", "OSALTINSERT_7")
    time.sleep(0.3)
    xwpe.key("alt+Insert", delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on Alt-Insert"
    xwpe.save()
    assert xwpe.saved_text().startswith("OSALTINSERT_7int main"), \
        "Alt-Insert should paste the external selection, got %r" % \
        xwpe.saved_text()[:40]


# @scenario SC-display-03
def test_alt_insert_in_a_field_takes_an_external_selection_whole(xwpe):
    """The field pastes an external selection in full, whatever length the
    editor's own clipboard had: it used to read the length before fetching the
    selection and cut the text, or ran past it, at the old length."""
    external = "external text that is long"
    copy_two_lines(xwpe)               # the editor's clipboard: an 11 byte line
    _xclip_own("clipboard", external)
    _xclip_own("primary", external)
    time.sleep(0.3)
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
def test_paste_takes_a_copy_made_in_another_application_after_the_editors(
        xwpe, paste_key):
    """The editor copies, so it owns CLIPBOARD and PRIMARY.  A browser then
    copies with Ctrl-C and takes CLIPBOARD only; the editor still owns
    PRIMARY.  Paste inserts the browser's text, the newest copy, and not the
    editor's old one."""
    _mark_first_word(xwpe)
    xwpe.key("ctrl+Insert", delay=0.6)       # Copy: the editor owns both
    time.sleep(0.4)
    _xclip_own("clipboard", "BROWSERCOPY_9")
    time.sleep(0.4)
    xwpe.key("End", paste_key, delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on the paste"
    xwpe.save()
    line = xwpe.saved_text().splitlines()[0]
    assert line == "int main(){BROWSERCOPY_9", \
        "paste should insert the browser's text, the first line is %r" % line


# @scenario SC-display-02
@pytest.mark.parametrize("paste_key", ["ctrl+v", "alt+Insert"])
def test_paste_ignores_another_clients_primary_while_the_editor_owns_the_clipboard(
        xwpe, paste_key):
    """The editor copies, so it owns CLIPBOARD; another client then takes
    PRIMARY.  Paste uses the editor's own text, and not the other client's
    PRIMARY."""
    _mark_first_word(xwpe)
    xwpe.key("ctrl+Insert", delay=0.6)       # Copy: the editor owns both
    time.sleep(0.4)
    _xclip_own("primary", "OTHERPRIMARY")
    time.sleep(0.4)
    xwpe.key("End", paste_key, delay=0.8)
    assert xwpe.proc.poll() is None, "xwpe died on the paste"
    xwpe.save()
    line = xwpe.saved_text().splitlines()[0]
    assert line == "int main(){int", \
        "paste should use the editor's text, the first line is %r" % line
