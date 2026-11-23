"""A path the system refuses to let the editor read is said with its reason,
and nothing opens as if the path had answered.

When the user names a file or a folder and the system says no (closed to
reading, a name under a file, a loop of links), the editor used to say a
guessed cause ("Can't access file or directory", "not found!"), or nothing:
File > Open of a file the user could not read opened an empty window under
its name, and a Save over it destroyed the file.  Each row drives the real
editor and asserts what the user reads and what is left on disk.

A file closed to reading (0000) or to everything but writing (0200) needs a
user the system keeps out; mode_premise.file_mode_is_obeyed skips the row by
name for root and fails it in an unprivileged run.

Run: WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_read_refused.py
(under a sanitizer build, launch it as wpe-asan)
"""
import os
import re

import pytest

pytest.importorskip("pyte")

from mode_premise import file_mode_is_obeyed, folder_mode_is_obeyed   # noqa: E402
from wpe_driver import WpeSession, wait_screen, wait_until            # noqa: E402

F3 = "\033OR"
F2 = "\033OQ"
ESC = "\033"
CR = "\r"
BACKTAB = "\033[Z"
ALT_A = "\033a"        # Attributes
ALT_B = "\033b"        # the Block menu
ALT_C = "\033c"        # ChDir
ALT_P = "\033p"        # the Project menu
ALT_1 = "\0331"        # the first window
CTRL_O = "\x0f"        # the Show menu
ERASE = "\x7f" * 5     # the "*.prj" filter of the Name field

COLS_ROWS = dict(cols=200, rows=40)
SEED = "int t;\n"
SECRET = "secret text\n"
PRJ_TEXT = "CMP=\tgcc\nEXENAME=\td\nFILES=\textra.c t.c util.c\n"
# How long a row waits for a state the editor paints or writes: seconds on an
# idle machine, which the owner of the wait stretches by the wait scale.
STATE_WAIT_S = 8.0


class Squashed(str):
    """The screen's text without its spaces; a sentence the box wrapped has
    the box's side lines between its words, so `needle in squashed` allows a
    run of them between any two letters."""

    def __contains__(self, needle):
        pattern = "[xab]*".join(re.escape(c) for c in needle if not c.isspace())
        return re.search(pattern, str(self)) is not None


def _squash(w, drain=0.6):
    return Squashed(re.sub(r"[^\x21-\x7e]", "", "".join(w.display(drain=drain))))


def _typed(w, text):
    for ch in text:
        w.key(ch, delay=0.05)


def _put(folder, name, text, mode):
    path = os.path.join(str(folder), name)
    with open(path, "w") as fh:
        fh.write(text)
    os.chmod(path, mode)
    return path


def _titled(w, name, drain=0.6):
    """True when a window titled @name is drawn: its name sits between the
    frame's line characters, unlike the same word in a field of a dialog."""
    pattern = re.compile(r"q %s q" % re.escape(name))
    return any(pattern.search(row) for row in w.display(drain=drain))


def _said(w, *needles):
    """True when the screen's squashed text holds every one of @needles.  This
    is what a wait polls, so it reads with drain=0: the wait drains between two
    of its reads, and a drain of its own would make every turn of it slow."""
    screen = _squash(w, drain=0)
    return all(needle in screen for needle in needles)


def _wait_said(w, *needles):
    """The squashed screen once it says every one of @needles, or the last
    screen seen when STATE_WAIT_S runs out.

    A row never decides from one read after a key's fixed delay: under load the
    editor paints its box later than that delay, and the row reads a screen
    that is not there yet.  So a row waits for the state it wants and asserts
    on the screen the wait ended on."""
    wait_until(w, lambda: _said(w, *needles), STATE_WAIT_S)
    return _squash(w)


def _wait_titled(w, name):
    """True once a window titled @name is drawn, False when STATE_WAIT_S runs
    out."""
    return wait_until(w, lambda: _titled(w, name, drain=0), STATE_WAIT_S)


def _manager_open(w):
    """True while the File Manager's own window is on the screen."""
    return "File-Manager" in "\n".join(w.display(drain=0))


def _open_with_f3(w, name):
    w.key(F3)
    _typed(w, name)
    w.key(CR, delay=1.0)


# @scenario SC-read-refused-01
def test_file_open_of_a_file_closed_to_reading_says_why_and_opens_no_window(tmp_path):
    """F3, locked.c (0000), Enter: the box names the file and says Permission
    denied, and no window titled locked.c opens.  The unfixed editor opened an
    empty padlocked window and said nothing."""
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    locked = _put(tmp_path, "locked.c", "you cannot read this\n", 0o000)
    try:
        with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
            _open_with_f3(w, "locked.c")
            screen = _wait_said(w, "Cannotopenfile", "locked.c", "Permissiondenied")
            assert "Cannotopenfile" in screen and "locked.c" in screen, "\n".join(w.display())
            assert "Permissiondenied" in screen, "\n".join(w.display())
            assert not _titled(w, "locked.c"), "\n".join(w.display())
            assert w.alive()
    finally:
        os.chmod(locked, 0o600)


# @scenario SC-read-refused-02
def test_a_name_under_a_file_from_the_command_line_says_not_a_directory(tmp_path):
    """wpe t.c/x t.c: x cannot be read because t.c is a file.  The box says
    Not a directory and no window named x opens; t.c opens after the box.
    The unfixed editor opened an empty window under that name, which a Save
    would then refuse."""
    _put(tmp_path, "t.c", SEED, 0o644)
    with WpeSession(str(tmp_path), SEED, filename="t.c", args=("t.c/x",), **COLS_ROWS) as w:
        screen = _wait_said(w, "Cannotopenfile", "Notadirectory")
        assert "Cannotopenfile" in screen and "Notadirectory" in screen, "\n".join(w.display())
        w.key(CR, delay=0.8)
        assert _wait_titled(w, "t.c"), \
            "t.c did not open once the box was answered:\n%s" % "\n".join(w.display())
        assert not _titled(w, "x"), "\n".join(w.display())
        assert w.alive()


# @scenario SC-read-refused-03
def test_file_open_of_a_loop_of_links_says_so_and_opens_no_window(tmp_path):
    """F3, loop (a link to itself), Enter: the box says Too many levels of
    symbolic links and no window titled loop opens."""
    os.symlink("loop", os.path.join(str(tmp_path), "loop"))
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        _open_with_f3(w, "loop")
        screen = _wait_said(w, "Cannotopenfile", "Toomanylevelsofsymboliclinks")
        assert "Cannotopenfile" in screen and "Toomanylevelsofsymboliclinks" in screen, \
            "\n".join(w.display())
        assert not _titled(w, "loop"), "\n".join(w.display())


# @scenario SC-read-refused-04
def test_block_read_of_a_file_closed_to_reading_says_why_and_leaves_the_text(tmp_path):
    """Block > Read of locked.c (0000): the box says the file and Permission
    denied instead of "Can't access file or directory", and the text of t.c
    is as it was."""
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    locked = _put(tmp_path, "locked.c", "you cannot read this\n", 0o000)
    try:
        with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
            w.key(ALT_B, delay=0.8)
            w.key("r", delay=1.0)
            _typed(w, "locked.c")
            w.key(CR, delay=1.0)
            screen = _wait_said(w, "Cannotopenfile", "Permissiondenied")
            assert "Cannotopenfile" in screen and "Permissiondenied" in screen, \
                "\n".join(w.display())
            assert "Can'taccess" not in screen, "\n".join(w.display())
            w.key(CR, delay=0.6)
            w.key(ESC, delay=0.6)
            assert wait_until(w, lambda: not _manager_open(w), STATE_WAIT_S), \
                "Esc should leave the File Manager:\n%s" % "\n".join(w.display())
            assert w.text() == SEED, w.text()
    finally:
        os.chmod(locked, 0o600)


# @scenario SC-read-refused-05
def test_attributes_of_a_loop_of_links_says_why(tmp_path):
    """Alt-A on a link that leads to itself: the box says Too many levels of
    symbolic links, not "Can't access file or directory"."""
    os.symlink("loop", os.path.join(str(tmp_path), "loop"))
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        w.key(F3)
        w.key("\t")
        _typed(w, "loop")
        w.key(ALT_A, delay=1.0)
        screen = _wait_said(w, "Cannotopenfile", "Toomanylevelsofsymboliclinks")
        assert "Cannotopenfile" in screen and "Toomanylevelsofsymboliclinks" in screen, \
            "\n".join(w.display())
        assert "Can'taccess" not in screen, "\n".join(w.display())


def _change_folder_to(w, path):
    """F3, the Directory field cleared and @path typed, Alt-C."""
    w.key(F3)
    w.key(BACKTAB)
    for _ in range(80):
        w.key("\x7f", delay=0.02)
    _typed(w, path)
    w.key(ALT_C, delay=1.0)


# @scenario SC-read-refused-06
def test_change_folder_into_a_folder_closed_to_reading_says_why(tmp_path):
    """Alt-C into a 0000 folder: the box names the folder and says Permission
    denied, not "Can't open directory", and the File Manager is still open."""
    folder_mode_is_obeyed(str(tmp_path), 0o000)
    closed = os.path.join(str(tmp_path), "closed")
    os.mkdir(closed)
    os.chmod(closed, 0o000)
    try:
        with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
            _change_folder_to(w, closed)
            screen = _wait_said(w, "Cannotopenfolder", "Permissiondenied")
            assert "Cannotopenfolder" in screen and "Permissiondenied" in screen, \
                "\n".join(w.display())
            assert "Can'topendirectory" not in screen, "\n".join(w.display())
            w.key(CR, delay=0.6)
            assert wait_until(w, lambda: not _said(w, "Cannotopenfolder"), STATE_WAIT_S), \
                "the box should go once it is answered:\n%s" % "\n".join(w.display())
            assert "File-Manager" in "".join(w.display()), "\n".join(w.display())
            assert w.alive()
    finally:
        os.chmod(closed, 0o700)


# @scenario SC-read-refused-06
def test_change_folder_into_a_file_says_not_a_directory(tmp_path):
    """F3, t.c/x, Enter: the File Manager goes into t.c first, which is a file.
    The box says Not a directory and the File Manager is still open."""
    _put(tmp_path, "t.c", SEED, 0o644)
    with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
        _open_with_f3(w, "t.c/x")
        screen = _wait_said(w, "Cannotopenfolder", "Notadirectory")
        assert "Cannotopenfolder" in screen and "Notadirectory" in screen, "\n".join(w.display())
        w.key(CR, delay=0.6)
        assert wait_until(w, lambda: not _said(w, "Cannotopenfolder"), STATE_WAIT_S), \
            "the box should go once it is answered:\n%s" % "\n".join(w.display())
        assert "File-Manager" in "".join(w.display()), "\n".join(w.display())


def _open_d_prj(w):
    w.key(ALT_P, delay=1.0)
    w.key("p", delay=1.5)
    w.key(ERASE + "d.prj" + CR, delay=2.0)


# @scenario SC-read-refused-07
def test_show_definition_with_a_project_closed_to_reading_says_so(tmp_path):
    """Show Definition of ZZ while the open project's .prj is 0000: the box says
    the project file cannot be opened and why, not "ZZ not found!", which
    sent the user looking for a typo that is not there."""
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    for name in ("extra.c", "util.c"):
        _put(tmp_path, name, "int x;\n", 0o644)
    prj = _put(tmp_path, "d.prj", PRJ_TEXT, 0o644)
    try:
        with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c",
                        wait=2.0, **COLS_ROWS) as w:
            _open_d_prj(w)
            w.key(ALT_1, delay=1.0)         # back to t.c: the Show menu is its
            os.chmod(prj, 0o000)
            w.key(CTRL_O, delay=0.6)
            w.key("s", delay=1.2)
            w.key("ZZ", delay=0.6)
            w.key(CR, delay=2.5)
            screen = _wait_said(w, "Cannotopenprojectfile", "Permissiondenied")
            assert "Cannotopenprojectfile" in screen and "Permissiondenied" in screen, \
                "\n".join(w.display())
            assert "notfound!" not in screen, "\n".join(w.display())
    finally:
        os.chmod(prj, 0o600)


# @scenario SC-read-refused-08
def test_a_file_the_system_refuses_to_read_is_not_opened_from_the_command_line(tmp_path):
    """wpe locked.c t.c: locked.c (0000) gives the box with its path and the
    system's reason and opens no window; t.c opens after the box is answered.
    Replaces the 0000 padlock row: a window with none of the file's text
    offered a Save As that copied nothing."""
    file_mode_is_obeyed(str(tmp_path), 0o000, os.O_RDONLY)
    locked = _put(tmp_path, "locked.c", "you cannot read this\n", 0o000)
    try:
        with WpeSession(str(tmp_path), SEED, filename="t.c", args=("locked.c",),
                        **COLS_ROWS) as w:
            assert wait_screen(w, "Cannot open file", STATE_WAIT_S), "\n".join(w.display())
            disp = "\n".join(w.display())
            assert "Cannot open file" in disp and "Permission denied" in disp, disp
            assert "\U0001F512" not in disp and " locked.c " not in w.display()[1], disp
            w.key(CR, delay=0.8)
            assert _wait_titled(w, "t.c"), \
                "t.c did not open once the box was answered:\n%s" % "\n".join(w.display())
            disp = w.display()
            assert not any(" locked.c " in row for row in disp), "\n".join(disp)
            assert " t.c " in "\n".join(disp[:3]), "\n".join(disp)
    finally:
        os.chmod(locked, 0o600)


# @scenario SC-read-refused-09
def test_a_write_only_file_is_refused_and_keeps_its_text(tmp_path):
    """F3, secret.c (0200), Enter: the box says the path and Permission denied,
    no window opens, and a key then F2 cannot reach the file.  On the
    unfixed tree an editable empty window opened and F2 left "\\na\\n" in the
    file: its text was gone."""
    file_mode_is_obeyed(str(tmp_path), 0o200, os.O_RDONLY)
    secret = _put(tmp_path, "secret.c", SECRET, 0o200)
    try:
        with WpeSession(str(tmp_path), SEED, filename="t.c", **COLS_ROWS) as w:
            _open_with_f3(w, "secret.c")
            assert wait_screen(w, "Cannot open file", STATE_WAIT_S), "\n".join(w.display())
            disp = "\n".join(w.display())
            assert "Cannot open file" in disp and "Permission denied" in disp, disp
            w.key(CR, delay=0.6)            # answer the box
            w.key(ESC, delay=0.6)           # leave the File Manager, still open after a refusal
            assert wait_until(w, lambda: not _manager_open(w), STATE_WAIT_S), \
                "Esc should leave the File Manager:\n%s" % "\n".join(w.display())
            w.key("a", delay=0.3)           # lands in t.c, the window on top
            w.key(F2, delay=1.0)            # saves t.c
            assert wait_until(w, lambda: w.text() == "a" + SEED, STATE_WAIT_S), \
                "F2 should have saved the key into t.c:\n%s" % "\n".join(w.display())
            assert not _titled(w, "secret.c"), "\n".join(w.display())
            os.chmod(secret, 0o600)         # the row cannot read a 0200 file otherwise
            with open(secret) as fh:
                assert fh.read() == SECRET
    finally:
        os.chmod(secret, 0o600)
