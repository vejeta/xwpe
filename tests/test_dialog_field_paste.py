"""A paste into a nearly full dialog field stops at a whole character.

Search > Replace's New Text field takes 128 bytes.  A paste that does not fit
used to be cut at the byte where the room ran out, which could leave the first
bytes of a multi-byte character at the end of the field and write text that is
not UTF-8.  The paste now stops before the last character that does not fit.
Each row copies the first line of the file into the clipboard, fills the field
to within a few bytes of full, pastes with Ctrl-V and checks the Replace result
on disk (the field's value actually used).
"""
import pytest
from wpe_driver import WpeSession, ALT

CTRL_K = "\x0b"
CTRL_C = "\x03"
CTRL_V = "\x16"
HOME, END = "\033[H", "\033[F"
FIELD_BYTES = 128          # the New Text field's size

EURO = "€"          # 3 bytes
EACUTE = "é"        # 2 bytes


def paste_into_field(tmp_path, copied, room):
    """Copy @copied, fill New Text to leave @room bytes, paste, Replace.

    Return the second line of the file as bytes after Replace "foo" with the
    field's value."""
    seed = copied + "\nfoo\n"
    with WpeSession(str(tmp_path), seed) as w:
        w.key(HOME, CTRL_K, "b", END, CTRL_K, "k", delay=0.3)
        w.key(CTRL_C, delay=0.5)
        w.key("\033[B", HOME, delay=0.3)         # start of the "foo" line
        w.menu(ALT.SEARCH, "r")
        w.key("foo")
        shot = "\n".join(w.display())
        w.key("\033n")                           # Alt-N: New Text
        w.key("a" * (FIELD_BYTES - room), delay=0.8)
        w.key(CTRL_V, delay=0.5)
        w.key("\033p")                           # Prompt on Replace off
        w.key("\033a")                           # Change All
        w.key("\r")                              # dismiss the "Replaced" note
        screen = "\n".join(w.display())
        w.save()
        assert w.alive()
        with open(tmp_path / "t.c", "rb") as fh:
            lines = fh.read().split(b"\n")
        assert lines[1] != b"foo", shot + screen
        return lines[1]


# @scenario SC-dialog-field-paste-01
ROWS = [
    # label, copied text, free bytes, what the field holds afterwards
    ("euro_does_not_fit_in_two", EURO + "x", 2, b"a" * 126),
    ("x_fits_after_a_wide_letter_is_cut", "x" + EURO, 2, b"a" * 126 + b"x"),
    ("e_acute_does_not_fit_in_one", "a" + EACUTE, 1, b"a" * 128),
    ("e_acute_first_does_not_fit_in_one", EACUTE + "a", 1, b"a" * 127),
    ("fits_exactly", EACUTE, 2, b"a" * 126 + EACUTE.encode()),
]


@pytest.mark.parametrize("label,copied,room,want", ROWS, ids=[r[0] for r in ROWS])
def test_paste_stops_before_the_character_that_does_not_fit(
        tmp_path, label, copied, room, want):
    got = paste_into_field(tmp_path, copied, room)
    assert got == want, (got[-8:], want[-8:])
    got.decode("utf-8")            # what the field holds is whole characters
