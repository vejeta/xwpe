"""Options > Save into a file the system refuses.

The user who saves the options into a read-only file used to read "Can't open
Option-File": no path, no reason.  The write owner says the file and the
system's reason and that the options were not saved, and a write that fails
after the file was emptied says the file may be cut short.

Run: WPE_BIN=$PWD/wpe pytest tests/test_option_save.py (and wpe-asan).

Not drivable headlessly: the rewrite of a project's xwperc that takes the
user's own keys out (e_opt_move_user_only_keys) runs only after a successful
write of the same file in the same call, so no premise makes it fail alone.
Its sentence is proven by tests/test_write_owner.c (THEN_USER_KEYS_STAY) and
its wiring by the build rule that no stream result is dropped.
"""
import os
import re
import stat

import pytest

import wpe_driver
from wpe_driver import WpeSession

ALT_O = "\033o"
ENTER = "\r"
SEED = "int main(void){return 0;}\n"
COLS_ROWS = dict(cols=100, rows=30)


def _flat(text):
    """The words of a screen with the box borders gone and the white space
    collapsed: a box wraps its sentence at a width that depends on the length
    of the scratch path in it, so a phrase may be split over two rows."""
    rows = []
    for row in text.splitlines():
        row = re.sub(r"^(x\s+)+", "", row)
        row = re.sub(r"(\s+x)+(\s+\S)?\s*$", "", row)
        rows.append(row.strip())
    return re.sub(r"\s+", " ", " ".join(rows))


def _text(w):
    return _flat("\n".join(r.rstrip() for r in w.display() if r.strip()))


def _file_refuses_writing(folder):
    """Does the system refuse to open a 0400 file for writing?  Root, or a
    user whose file modes are overridden, opens it anyway and the row would
    fail for a reason that has nothing to do with the editor."""
    probe = os.path.join(folder, "probe_mode")
    with open(probe, "w") as fh:
        fh.write("x\n")
    os.chmod(probe, 0o400)
    try:
        os.close(os.open(probe, os.O_WRONLY))
        refuses = False
    except OSError:
        refuses = True
    os.chmod(probe, 0o600)
    os.remove(probe)
    return refuses


# @scenario SC-unsaved-text-17
def test_save_options_into_a_read_only_file_says_why(tmp_path):
    """The xwperc is 0400 after the editor started (it was read at start):
    Options > Save names the file and the reason and says the options were
    not saved, and the old words ("Can't open Option-File") are gone."""
    if not _file_refuses_writing(str(tmp_path)):
        pytest.skip("this user can write a 0400 file (root, or a capability "
                    "that overrides file modes)")
    folder = tmp_path / ".config" / "xwpe"
    folder.mkdir(parents=True)
    rc = folder / "xwperc"
    rc.write_text("[General]\n[Programming]\n")
    before = rc.read_text()
    with WpeSession(str(tmp_path), SEED, filename="t.c", wait=2.0, **COLS_ROWS) as w:
        os.chmod(str(rc), 0o400)
        try:
            w.key(ALT_O, delay=1.0)
            w.key("s", delay=1.5)
            w.key(ENTER, delay=1.5)
            text = _text(w)
        finally:
            os.chmod(str(rc), 0o600)
        assert w.alive(), text
    assert "Cannot write" in text and "Permission denied" in text, text
    assert "The options were not saved." in text, text
    assert "Option-File" not in text, text
    assert rc.read_text() == before


def _limited_editor(tmp_path, monkeypatch, kib):
    """The editor started under `ulimit -f <kib>`, still under its own name:
    a write past the limit then fails (SIGXFSZ is ignored by the owner)."""
    real = os.path.abspath(wpe_driver.WPE_BIN)
    name = os.path.basename(real)
    bindir = tmp_path / "limited"
    bindir.mkdir()
    wrapper = bindir / name
    wrapper.write_text("#!/bin/bash\nulimit -f %d\nexec -a %s %s \"$@\"\n" % (kib, name, real))
    wrapper.chmod(wrapper.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setattr(wpe_driver, "WPE_BIN", str(wrapper))
    return {"XWPE_LIB": os.path.dirname(real)}


# @scenario SC-unsaved-text-17
def test_save_options_that_stops_after_the_open_says_the_file_may_be_cut_short(
        tmp_path, monkeypatch):
    """The file opens (and is emptied) but no byte fits under the size limit:
    the box says the file may be cut short, not only that the options were not
    saved.  The save also stops there: nothing after it (the move of the
    user's own keys out of a project's xwperc) runs on a file that was not
    written."""
    folder = tmp_path / "home" / ".config" / "xwpe"
    folder.mkdir(parents=True)
    rc = folder / "xwperc"
    rc.write_text("[General]\n[Programming]\n")
    env = _limited_editor(tmp_path, monkeypatch, 0)
    with WpeSession(str(tmp_path / "home"), SEED, filename="t.c", wait=2.0, env_extra=env,
                    **COLS_ROWS) as w:
        w.key(ALT_O, delay=1.0)
        w.key("s", delay=1.5)
        w.key(ENTER, delay=1.5)
        text = _text(w)
        assert w.alive(), text
    assert "Cannot write" in text and "File too large" in text, text
    assert "may be cut short" in text, text
    assert "The options were not saved." in text, text
    assert rc.stat().st_size == 0
