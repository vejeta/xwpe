"""F2 writes into the file the window read, not into whatever has the name now.

An editor that saves by renaming (vim with backupcopy=no, sed -i, git
checkout, rsync) leaves a new file under the name of a file that is open in
xwpe.  Looking the name up again at save time would write the window's text
into that file, and keep its special bits if it were a program.  The window
remembers the device and inode it read instead:

 * a plain file of the same owner is asked about, and Yes writes the text;
 * a file with a special bit is refused, the file is left as it is, and Save
   As onto the same name is the way out.

The box is the editor's own e_message, the same for the terminal, X11 and
Wayland, so no twin of these rows is needed.  The editor is started as `wpe`
(WPE_BIN), the name that keeps the whole programming layer on.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_save_identity.py
"""
import os
import stat
import time

import pytest

pytest.importorskip("pyte")

from mode_premise import special_bits_settable, workdir_in_my_group   # noqa: E402
from wpe_driver import ALT, WpeSession, box_text                      # noqa: E402

F2 = "\033OQ"            # kf2 -> Save
SEED = "alpha\n"
THEIRS = "theirs\n"
QUESTION = "was replaced on disk since this window read it"
# The box wraps its sentence at a column that depends on the length of the
# path above it ("Save" and "As" can end one row and start the next), so a row
# reads the box's text joined across its rows (wpe_driver.box_text) and names
# short phrases; the C rows hold the sentence.
BOX_TITLE = "Message"
REFUSED_WORDS = ("leads", "different", "Nothing", "Save As")
SETTLE_S = 0.5


def _screen(s):
    return "\n".join(s.display())


def _read(path):
    with open(path) as fh:
        return fh.read()


def _another_file_takes_the_name(work, text, mode):
    """What vim, sed -i and git checkout do: a new inode renamed over the name."""
    incoming = os.path.join(work, "incoming")
    with open(incoming, "w") as fh:
        fh.write(text)
    os.chmod(incoming, mode)
    os.rename(incoming, os.path.join(work, "t.c"))


# @scenario SC-write-owner-F-11
def test_a_file_renamed_over_the_name_is_asked_about_and_yes_saves(tmp_path):
    """The question appears, Yes writes the text over the new file."""
    work = str(tmp_path)
    with WpeSession(work, SEED) as s:
        s.key("X")
        _another_file_takes_the_name(work, THEIRS, 0o644)
        s.key(F2)
        time.sleep(SETTLE_S)
        shown = _screen(s)
        assert QUESTION in shown, shown
        assert _read(s.path) == THEIRS, "nothing may be written before Yes"
        s.key("y")
        time.sleep(SETTLE_S)
        assert _read(s.path) == "Xalpha\n"
        assert stat.S_IMODE(os.stat(s.path).st_mode) & 0o7000 == 0


# @scenario SC-write-owner-F-12
def test_a_file_renamed_over_the_name_and_declined_is_left_alone(tmp_path):
    """No writes nothing: the other program's file is as it was."""
    work = str(tmp_path)
    with WpeSession(work, SEED) as s:
        s.key("X")
        _another_file_takes_the_name(work, THEIRS, 0o644)
        s.key(F2)
        time.sleep(SETTLE_S)
        assert QUESTION in _screen(s)
        s.key("n")
        time.sleep(SETTLE_S)
        assert _read(s.path) == THEIRS


# @scenario SC-write-owner-F-13
def test_a_replacement_with_a_special_bit_is_refused_and_save_as_works(tmp_path):
    """No question, nothing written, the file as it was; Save As onto the
    same name then writes the text."""
    work = str(tmp_path)
    workdir_in_my_group(work)
    special_bits_settable(work, stat.S_ISGID)
    with WpeSession(work, SEED) as s:
        s.key("X")
        _another_file_takes_the_name(work, THEIRS, 0o2755)
        s.key(F2)
        time.sleep(SETTLE_S)
        shown = _screen(s)
        boxed = box_text(s.display(), BOX_TITLE)
        for phrase in REFUSED_WORDS:
            assert phrase in boxed, (phrase, shown)
        assert "Overwrite" not in shown, shown
        assert _read(s.path) == THEIRS
        assert stat.S_IMODE(os.stat(s.path).st_mode) & 0o7000 == stat.S_ISGID
        s.key("\033", "\033")
        s.menu(ALT.FILE, "a")
        s.key("t.c", "\033s")
        time.sleep(SETTLE_S)
        s.key("y")
        time.sleep(SETTLE_S)
        assert _read(s.path) == "Xalpha\n"
        assert stat.S_IMODE(os.stat(s.path).st_mode) & 0o7000 == 0, "a copy has no special bit"


# @scenario SC-write-owner-F-16
def test_a_link_to_the_null_device_under_the_name_is_refused(tmp_path):
    """The name now leads to something that is not a regular file: F2 writes
    nothing into it and the text stays in the window, still modified."""
    work = str(tmp_path)
    with WpeSession(work, SEED) as s:
        s.key("X")
        os.unlink(s.path)
        os.symlink("/dev/null", s.path)
        s.key(F2)
        time.sleep(SETTLE_S)
        shown = _screen(s)
        boxed = box_text(s.display(), BOX_TITLE)
        for phrase in REFUSED_WORDS:
            assert phrase in boxed, (phrase, shown)
        assert "Overwrite" not in shown, shown
        s.key("\033", "\033")
        assert "Xalpha" in _screen(s), "the text left the window"


# @scenario SC-write-owner-F-17
def test_a_window_of_a_file_that_did_not_exist_saves_without_a_question(tmp_path):
    """A window gets its identity forgotten when it is made.  With memory
    filled by the allocator (MALLOC_PERTURB_) a window that skipped that would
    hold a made-up file and ask about a file it never read."""
    work = str(tmp_path)
    env = {"MALLOC_PERTURB_": "165"}
    with WpeSession(work, SEED, args=("new.c",), env_extra=env) as s:
        s.key("\0331")                       # window 1: new.c, which does not exist
        s.key("Z")
        with open(os.path.join(work, "new.c"), "w") as fh:
            fh.write("made by another program\n")
        s.key(F2)
        time.sleep(SETTLE_S)
        shown = _screen(s)
        assert QUESTION not in shown, shown
        assert "Overwrite" not in shown, shown
        assert "Z" in _read(os.path.join(work, "new.c"))
