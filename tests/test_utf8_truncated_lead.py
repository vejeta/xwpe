"""A UTF-8 lead byte whose continuation bytes are missing is one byte.

A string that ends inside a character (a field ending in 0xE2, or 0xE2 and a
plain letter) must never make the editor read past its end: the character is
the lead byte alone, drawn as the replacement glyph.  These rows paste such
text into a nearly full or an empty dialog field and open, move over and save
a file whose last byte is a lead byte.  They are meant to run under
`wpe-asan`, where a read past the buffer aborts the editor.
"""
import builtins

import pytest
import wpe_driver
from wpe_driver import WpeSession, ALT

CTRL_K, CTRL_C, CTRL_V = "\x0b", "\x03", "\x16"
HOME, END, DOWN, RIGHT = "\033[H", "\033[F", "\033[B", "\033[C"
INSERT_KEY = "\033[2~"     # Insert: typing overwrites
FIELD_BYTES = 128          # the New Text field's size
LEAD = "\udce2"            # the raw byte 0xE2 once written with surrogateescape


@pytest.fixture(autouse=True)
def raw_bytes_in_seeds(monkeypatch):
    """Let a seed carry raw invalid bytes (written as surrogate escapes)."""
    def open_raw(path, mode="r", *a, **k):
        if "b" not in mode:
            k.setdefault("errors", "surrogateescape")
        return builtins.open(path, mode, *a, **k)
    monkeypatch.setattr(wpe_driver, "open", open_raw, raising=False)


REPLACEMENT = "�"     # what the field draws for the lone lead byte


def field_text(row):
    """The visible text of the New Text field on screen row @row."""
    return row.split("New Text:", 1)[1].split("v", 1)[0].strip()


def paste_and_replace(tmp_path, copied, room):
    """Copy the first line @copied, leave @room free bytes in New Text, paste,
    Replace "foo" with the field.  Return what the field showed after the
    paste and the line Replace wrote, as bytes; the editor must still be
    alive after drawing the field."""
    with WpeSession(str(tmp_path), copied + "\nfoo\n") as w:
        w.key(HOME, CTRL_K, "b", END, CTRL_K, "k", delay=0.3)
        w.key(CTRL_C, delay=0.5)
        w.key(DOWN, HOME, delay=0.3)
        w.menu(ALT.SEARCH, "r")
        w.key("foo")
        w.key("\033n")
        w.key("a" * (FIELD_BYTES - room), delay=0.8)
        w.key(CTRL_V, delay=0.5)
        screen = w.display()
        shot = "\n".join(screen)
        assert w.alive(), shot
        rows = [r for r in screen if "New Text:" in r]
        assert rows, shot
        w.key("\033p")
        w.key("\033a")
        w.key("\r")
        w.save()
        assert w.alive(), "\n".join(w.display())
        with open(tmp_path / "t.c", "rb") as fh:
            return field_text(rows[0]), fh.read().split(b"\n")[1]


# A lone lead byte that fits is pasted as one byte and drawn as the
# replacement glyph, with nothing read from beyond the end of the field.
# @scenario SC-utf8-truncated-lead-01
@pytest.mark.parametrize("copied,room,shown,written", [
    (LEAD + "x", 2, "a" + REPLACEMENT + "x", b"a" * 126 + b"\xe2x"),
    (LEAD + "x", 1, "a" + REPLACEMENT, b"a" * 127 + b"\xe2"),
    ("ab" + LEAD, 2, "aab", b"a" * 126 + b"ab"),
    ("ab" + LEAD, 1, "aaa", b"a" * 128),
], ids=["lead_and_letter_room2", "lead_and_letter_room1",
        "trailing_lead_room2", "trailing_lead_room1"])
def test_paste_of_a_cut_character_into_a_nearly_full_field(tmp_path, copied,
                                                           room, shown,
                                                           written):
    field, line = paste_and_replace(tmp_path, copied, room)
    assert field.endswith(shown), repr(field)
    assert line == written, line[-40:]


# @scenario SC-utf8-truncated-lead-02
@pytest.mark.parametrize("copied,shown,written", [
    (LEAD + "x", REPLACEMENT + "x", b"\xe2x"),
    ("ab" + LEAD, "ab" + REPLACEMENT, b"ab\xe2"),
], ids=["lead_and_letter", "trailing_lead"])
def test_paste_of_a_cut_character_into_an_empty_field(tmp_path, copied,
                                                      shown, written):
    field, line = paste_and_replace(tmp_path, copied, FIELD_BYTES)
    assert field == shown, repr(field)
    assert line == written, line


# @scenario SC-utf8-truncated-lead-03
def test_a_file_ending_in_a_lead_byte_is_shown_moved_over_and_saved(tmp_path):
    seed = "ab" + LEAD
    with WpeSession(str(tmp_path), seed) as w:
        w.key(END, delay=0.3)
        w.key(HOME, RIGHT, RIGHT, RIGHT, RIGHT, delay=0.3)
        shot = "\n".join(w.display())
        assert w.alive(), shot
        w.key("\033[D", END, delay=0.3)
        w.save()
        assert w.alive(), "\n".join(w.display())
        with open(tmp_path / "t.c", "rb") as fh:
            data = fh.read()
    # Saving ends the last line with a newline, as for any file; the bytes
    # before it, the cut character included, must come back unchanged.
    assert data == b"ab\xe2\n", data


# A Latin-1 letter is a byte that starts no whole UTF-8 character: a letter
# typed over it in overwrite mode replaces that one byte, never the two
# letters after it.
# @scenario SC-utf8-truncated-lead-04
def test_overwrite_over_a_latin1_letter_replaces_one_byte(tmp_path):
    seed = "a" + "\udce9" + "xyz"
    with WpeSession(str(tmp_path), seed) as w:
        w.key(INSERT_KEY, RIGHT, delay=0.3)
        w.key("z", delay=0.3)
        w.save()
        assert w.alive(), "\n".join(w.display())
        with open(tmp_path / "t.c", "rb") as fh:
            data = fh.read()
    assert data == b"azxyz\n", data
