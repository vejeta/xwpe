"""Editing commands that must leave the text alone, or change it once.

Terminal layer.  Each row drives the keys a user presses and checks what the
user then sees: the file on disk (saved with F2, or the clipboard pasted into
a file that can be saved) and the window on the screen.

- A window the user cannot change -- a file opened read-only (0444) and the
  clipboard viewer (Alt-Y) -- keeps its text through every command that
  edits a block or a line (Ctrl-K I, U, D, F; Ctrl-O B, L, U, M, H, E;
  Search > Replace), and the status row says why.
- At the end of the text Ctrl-T deletes nothing and Ctrl-Right stays.
- The ten markers Ctrl-K 0-9 / Ctrl-O 0-9 each keep their own place.
- Ctrl-O E (the help file's end-of-page mark) is one Undo step.
"""
import os

import pytest

from wpe_driver import WpeSession, ALT

CTRL_K = "\x0b"
CTRL_O = "\x0f"
CTRL_U = "\x15"
CTRL_R = "\x12"
CTRL_T = "\x14"
CTRL_C = "\x03"          # Edit > Copy
CTRL_V = "\x16"          # Edit > Paste
CTRL_W = "\x17"          # close the window
ALT_Y = "\033y"          # Show Buffer: the clipboard viewer
ALT_1 = "\0331"          # window 1
ALT_N = "\033n"
ALT_P = "\033p"
ALT_A = "\033a"
ALT_U = "\033u"
DOWN = "\033[B"
UP = "\033[A"
RIGHT = "\033[C"
CTRL_RIGHT = "\033[1;5C"
HOME = "\033OH"
END = "\033OF"
ENTER = "\r"
F2 = "\033OQ"
F3 = "\033OR"            # File > Open: the File Manager
HFE = "\x06"             # the help file's end-of-page mark (Ctrl-F)
HBG = "\x01"             # the help file's button start (Ctrl-A)
READ_ONLY_SAID = "Read-only"

# The text every command below works on.  Its first line holds a help mark
# for Ctrl-O L to delete; the second is indented for Ctrl-K U and the
# beautifiers; "two" is what Replace changes.
LOCK_SEED = "o" + HBG + "ne\n  two\nthree\n"
LOCK_WORDS = ("ne", "two", "three")

MARK_ALL = (CTRL_K, "x")
# A block of the first two characters of the first line, for the help-file
# marks Ctrl-O U, M and H, which take a block inside one line.
MARK_IN_LINE = (HOME, CTRL_K, "b", RIGHT, RIGHT, CTRL_K, "k")

# Each command a locked window must refuse, with the keys that make it change
# the text of a window the user can change.
LOCK_COMMANDS = {
    "ctrl-k-i": MARK_ALL + (CTRL_K, "i"),
    "ctrl-k-u": MARK_ALL + (CTRL_K, "u"),
    "ctrl-k-d": MARK_ALL + (CTRL_K, "d", ALT_U, ENTER),
    "ctrl-k-f": MARK_ALL + (CTRL_K, "f"),
    "ctrl-o-b": (CTRL_O, "b", ENTER),
    "ctrl-o-l": (HOME, CTRL_O, "l"),
    "ctrl-o-u": MARK_IN_LINE + (CTRL_O, "u"),
    "ctrl-o-m": MARK_IN_LINE + (CTRL_O, "m"),
    "ctrl-o-h": MARK_IN_LINE + (CTRL_O, "h"),
    "ctrl-o-e": (CTRL_O, "e"),
    "replace": (ALT.SEARCH, "r", "two", ALT_N, "TWO", ALT_P, ALT_A, ENTER),
}


def _keys(s, keys, delay=0.3):
    """Send @keys; return whether the screen said the window is read-only
    after any of them (a box that closes on the last key may say it)."""
    said = False
    for k in keys:
        s.key(k, delay=delay)
        said = said or _status_says_read_only(s)
    s._drain(0.6)
    return said or _status_says_read_only(s)


def _text_rows(s):
    """The rows of the screen that show the seed's text, with their place,
    without the scroll bar on the right, which moves with the cursor."""
    return [(i, row[:-1].rstrip()) for i, row in enumerate(s.display(drain=0.3))
            if any(w in row for w in LOCK_WORDS)]


def _status_says_read_only(s):
    return READ_ONLY_SAID in "\n".join(s.display(drain=0.2))


def _open_read_only(s, tmp_path, name="ro.c"):
    """Open a 0444 file from the File Manager, over the writable window."""
    path = os.path.join(str(tmp_path), name)
    with open(path, "w") as fh:
        fh.write(LOCK_SEED)
    os.chmod(path, 0o444)
    s.key(F3, delay=1.0)
    s.key(name, delay=0.4)
    s.key(ENTER, delay=1.0)
    assert any(name in row for row in s.display(drain=0.3)[:3]), \
        "the read-only file did not open:\n" + "\n".join(s.display())
    return path


def _clipboard_on_disk(s):
    """Paste the clipboard into the writable window and save: its text."""
    s.key(CTRL_V, delay=0.8)
    s.key(F2, delay=0.8)
    return s.text()


@pytest.mark.parametrize("command", sorted(LOCK_COMMANDS))
def test_each_lock_command_changes_a_window_the_user_can_change(tmp_path, command):
    """The keys each row below sends do change the text of an ordinary
    file, so a row that sees no change in a locked window tests a refusal,
    not keys that do nothing."""
    with WpeSession(str(tmp_path), LOCK_SEED) as s:
        before = _text_rows(s)
        _keys(s, LOCK_COMMANDS[command])
        s.key(F2, delay=0.8)
        assert s.text() != LOCK_SEED or _text_rows(s) != before, \
            "%s changed nothing in a writable file:\n%s" % (command, "\n".join(s.display()))
        assert s.alive(), "wpe died"


@pytest.mark.parametrize("command", sorted(LOCK_COMMANDS))
def test_a_read_only_file_keeps_its_text_through_each_command(tmp_path, command):
    """A file opened read-only: each block and line command leaves its text
    as it was, the window does not read as changed, and the status row says
    the window is read-only.  Copied out and pasted into a writable file,
    the text is the file's own."""
    with WpeSession(str(tmp_path), "") as s:
        path = _open_read_only(s, tmp_path)
        before = _text_rows(s)
        said = _keys(s, LOCK_COMMANDS[command])
        assert _text_rows(s) == before, \
            "%s changed a read-only window:\n%s" % (command, "\n".join(s.display()))
        assert said, \
            "%s was refused without a word:\n%s" % (command, "\n".join(s.display()))
        _keys(s, MARK_ALL + (CTRL_C,))
        s.key(ALT_1, delay=0.8)                     # back to the writable t.c
        assert _clipboard_on_disk(s) == LOCK_SEED, \
            "after %s the read-only window held %r" % (command, s.text())
        with open(path) as fh:
            assert fh.read() == LOCK_SEED, "the read-only file changed on disk"
        assert s.alive(), "wpe died"


@pytest.mark.parametrize("command", sorted(LOCK_COMMANDS))
def test_the_clipboard_viewer_keeps_its_text_through_each_command(tmp_path, command):
    """The clipboard shown with Alt-Y: each block and line command leaves
    what was copied as it was, and the status row says the viewer is
    read-only; pasted afterwards, the clipboard gives the copied text."""
    copied = LOCK_SEED.rstrip("\n")
    with WpeSession(str(tmp_path), LOCK_SEED) as s:
        _keys(s, MARK_ALL + (CTRL_C,))
        s.key(ALT_Y, delay=0.8)
        assert " Buffer " in "\n".join(s.display()), "Alt-Y did not show the clipboard"
        _keys(s, (UP,) * LOCK_SEED.count("\n") + (HOME,))   # the viewer opens at the end
        before = _text_rows(s)
        said = _keys(s, LOCK_COMMANDS[command])
        assert _text_rows(s) == before, \
            "%s changed the clipboard viewer:\n%s" % (command, "\n".join(s.display()))
        assert said, \
            "%s was refused without a word:\n%s" % (command, "\n".join(s.display()))
        _keys(s, MARK_ALL)                          # Paste takes what the viewer marks
        s.key(CTRL_W, delay=0.8)
        assert _clipboard_on_disk(s) == copied + LOCK_SEED, \
            "after %s the clipboard held %r" % (command, s.text())
        assert s.alive(), "wpe died"


# Search > Replace, Change All, with nothing to put in: in a window the user
# can change, each occurrence goes and the search goes on where it stood.
REPLACE_WITH_NOTHING = (ALT.SEARCH, "r", "two", ALT_N, ALT_P, ALT_A)
FOUND_BOX = "Replaced 0"     # the box Change All ends with, before Enter closes it


@pytest.mark.parametrize("where", ["read-only-file", "clipboard-viewer"])
def test_replace_all_with_nothing_in_a_locked_window_ends(tmp_path, where):
    """Change All with an empty replacement in a locked window: every
    occurrence stays, so the search must go on behind each one; the box
    that counts them comes up and the text is as it was."""
    with WpeSession(str(tmp_path), "" if where == "read-only-file" else LOCK_SEED) as s:
        if where == "read-only-file":
            path = _open_read_only(s, tmp_path)
        else:
            _keys(s, MARK_ALL + (CTRL_C,))
            s.key(ALT_Y, delay=0.8)
            _keys(s, (UP,) * LOCK_SEED.count("\n") + (HOME,))
        before = _text_rows(s)
        for k in REPLACE_WITH_NOTHING:
            s.key(k, delay=0.3)
        screen = "\n".join(s.display(drain=1.0))
        assert FOUND_BOX in screen and READ_ONLY_SAID in screen, \
            "Change All did not come back:\n" + screen
        s.key(ENTER, delay=0.6)
        assert _text_rows(s) == before, "the locked window changed:\n" + "\n".join(s.display())
        if where == "read-only-file":
            with open(path) as fh:
                assert fh.read() == LOCK_SEED, "the read-only file changed on disk"
        assert s.alive(), "wpe died"


END_SEED = "one\ntwo\n"


def _to_the_end_of_the_text(s):
    s.key(DOWN, DOWN, delay=0.2)
    s.key(END, delay=0.3)


def test_ctrl_t_at_the_end_of_the_text_deletes_nothing(tmp_path):
    """At the end of the last line Ctrl-T has no word and no line to its
    right: the text keeps its last line break and the cursor stays at the
    end, however often it is pressed, as in the Borland editors."""
    with WpeSession(str(tmp_path), END_SEED) as s:
        _to_the_end_of_the_text(s)
        s.key(CTRL_T, delay=0.5)
        s.key(CTRL_T, delay=0.5)
        s.key(F2, delay=0.8)
        assert s.text() == END_SEED, "Ctrl-T at the end of the text gave %r" % s.text()
        s.key("X", delay=0.4)
        s.key(F2, delay=0.8)
        assert s.text() == "one\ntwoX\n", "the cursor left the end of the text: %r" % s.text()
        assert s.alive(), "wpe died"


def test_ctrl_t_at_the_end_of_a_line_joins_the_next(tmp_path):
    """Ctrl-T at the end of a line that is not the last joins the next line
    to it, as before."""
    with WpeSession(str(tmp_path), END_SEED) as s:
        s.key(END, delay=0.3)
        s.key(CTRL_T, delay=0.5)
        s.key(F2, delay=0.8)
        assert s.text() == "onetwo\n", "Ctrl-T at the end of the first line gave %r" % s.text()
        assert s.alive(), "wpe died"


def test_ctrl_right_at_the_end_of_the_text_stays_there(tmp_path):
    """Ctrl-Right at the end of the text has no word to go to: the cursor
    stays, and what is typed next goes at the end."""
    with WpeSession(str(tmp_path), END_SEED) as s:
        _to_the_end_of_the_text(s)
        s.key(CTRL_RIGHT, delay=0.4)
        s.key("X", delay=0.4)
        s.key(F2, delay=0.8)
        assert s.text() == "one\ntwoX\n", "after Ctrl-Right, X went to %r" % s.text()
        assert s.alive(), "wpe died"


MARKER_SEED = "".join("line%d\n" % i for i in range(10))


MARKER_COLUMN = 2


def _set_markers_from_the_last_line_up(s):
    """Marker n on column 2 of line n, the last line's marker 9 set first:
    every later Ctrl-K n moves the cursor and the search mark, so a marker
    that shared memory with them would be lost."""
    for _ in range(9):
        s.key(DOWN, delay=0.1)
    s.key(HOME, RIGHT, RIGHT, delay=0.2)
    for n in "9876543210":
        s.key(CTRL_K, n, delay=0.2)
        s.key(UP, delay=0.1)


@pytest.mark.parametrize("marker", ["0", "5", "9"])
def test_each_marker_keeps_its_own_place(tmp_path, marker):
    """Ctrl-K 0 to Ctrl-K 9 set ten markers, one per line; Ctrl-O <n> goes
    back to marker n, where X is typed: each of the ten is a marker of its
    own, as Borland's ten bookmarks are."""
    line = int(marker)
    want = MARKER_SEED.replace("line%d" % line,
                               ("line%d" % line)[:MARKER_COLUMN] + "X" +
                               ("line%d" % line)[MARKER_COLUMN:], 1)
    with WpeSession(str(tmp_path), MARKER_SEED) as s:
        _set_markers_from_the_last_line_up(s)
        s.key(CTRL_O, marker, delay=0.4)
        s.key("X", delay=0.4)
        s.key(F2, delay=0.8)
        assert s.text() == want, \
            "Ctrl-O %s did not go back to marker %s: %r" % (marker, marker, s.text())
        assert s.alive(), "wpe died"


HELP_SEED = "one\ntwo\nthree\n"


def test_ctrl_o_e_is_one_undo_and_one_redo(tmp_path):
    """Ctrl-O E puts the end-of-page mark on a line of its own above the
    cursor's line; one Ctrl-U takes the line away, one Ctrl-R brings it
    back, and the user's lines stay as they were: a second Ctrl-U has
    nothing left to undo."""
    marked = HFE + "\n" + HELP_SEED
    with WpeSession(str(tmp_path), HELP_SEED, filename="t.txt") as s:
        s.key(CTRL_O, "e", delay=0.5)
        s.key(F2, delay=0.8)
        assert s.text() == marked, "Ctrl-O E gave %r" % s.text()
        s.key(CTRL_U, delay=0.6)
        s.key(F2, delay=0.8)
        assert s.text() == HELP_SEED, "one Ctrl-U gave %r" % s.text()
        s.key(CTRL_R, delay=0.6)
        s.key(F2, delay=0.8)
        assert s.text() == marked, "one Ctrl-R gave %r" % s.text()
        s.key(CTRL_U, delay=0.6)
        s.key(CTRL_U, delay=0.6)
        assert "No more Undo" in "\n".join(s.display()), \
            "a second Ctrl-U found something to undo:\n" + "\n".join(s.display())
        s.key(ENTER, delay=0.6)                 # the "No more Undo" box
        s.key(F2, delay=0.8)
        assert s.text() == HELP_SEED, "a second Ctrl-U took the user's text: %r" % s.text()
        assert s.alive(), "wpe died"
