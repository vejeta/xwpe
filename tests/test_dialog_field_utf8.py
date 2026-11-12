"""A dialog text field keeps what was typed before a character above Latin-1.

Typing ab<euro>cd into Search > Replace's New Text field used to leave only
"cd": a character of 3 or 4 bytes in UTF-8 (a codepoint of 255 or more) made
the field hand the key back to its dialog as if it were a command, and the
dialog reopened the field with its first-key rule, which replaces the text.
These rows type such characters into the field, move over them with Left,
Right, Home, End, Backspace and Delete, and check the Replace result on disk
(the field's value actually used) and a Find that must hit the typed text.
"""
import pytest
from wpe_driver import WpeSession, ALT

LEFT, RIGHT = "\033[D", "\033[C"
HOME, END = "\033[H", "\033[F"
BACKSPACE = "\x7f"
DELETE = "\033[3~"

EURO = "€"          # 3 bytes
CLEF = "\U0001d11e"      # 4 bytes
EACUTE = "é"        # 2 bytes
ABAR = "ā"          # 2 bytes, codepoint 257 (above Latin-1)


def replace_foo_with(tmp_path, typed_keys):
    """Run Replace foo -> (what typed_keys leave in New Text), Change All."""
    with WpeSession(str(tmp_path), "foo\n") as w:
        w.menu(ALT.SEARCH, "r")
        w.key("foo")
        w.key("\033n")                     # Alt-N: New Text
        for k in typed_keys:
            w.key(k, delay=0.3)
        w.key("\033p")                     # Prompt on Replace off
        w.key("\033a")                     # Change All
        w.key("\r")                        # dismiss the "Replaced" note
        w.save()
        assert w.alive()
        return w.text()


# @scenario SC-dialog-field-utf8-01
TYPED = [
    ("euro_inside", ["a", "b", EURO, "c", "d"], "ab" + EURO + "cd"),
    ("clef_inside", ["a", CLEF, "b"], "a" + CLEF + "b"),
    ("two_and_three", [EACUTE, EURO, EACUTE], EACUTE + EURO + EACUTE),
    ("above_latin1_2byte", ["x", ABAR, "y"], "x" + ABAR + "y"),
    ("euro_last", ["a", EURO], "a" + EURO),
]


@pytest.mark.parametrize("label,keys,want", TYPED, ids=[t[0] for t in TYPED])
def test_typing_keeps_the_text_before_a_wide_character(tmp_path, label, keys, want):
    assert replace_foo_with(tmp_path, keys).startswith(want)


# @scenario SC-dialog-field-utf8-02
EDITED = [
    ("backspace_over_euro", ["a", "b", EURO, "c", "d", LEFT, LEFT, BACKSPACE],
     "abcd"),
    ("delete_over_euro", ["a", "b", EURO, "c", "d", LEFT, LEFT, LEFT, DELETE],
     "abcd"),
    ("backspace_over_clef", ["a", CLEF, "b", LEFT, BACKSPACE], "ab"),
    ("delete_over_clef", ["a", CLEF, "b", LEFT, LEFT, DELETE], "ab"),
    ("left_right_over_euro", ["a", EURO, "b", LEFT, LEFT, RIGHT, "X"],
     "a" + EURO + "Xb"),
    ("home_then_right_over_clef", ["a", CLEF, "b", HOME, RIGHT, RIGHT, "X"],
     "a" + CLEF + "Xb"),
    ("end_after_left", ["a", EURO, "b", LEFT, LEFT, END, "X"],
     "a" + EURO + "bX"),
    ("left_over_euro_then_insert", ["a", EURO, LEFT, "X"], "aX" + EURO),
]


@pytest.mark.parametrize("label,keys,want", EDITED, ids=[t[0] for t in EDITED])
def test_cursor_keys_move_over_a_wide_character(tmp_path, label, keys, want):
    assert replace_foo_with(tmp_path, keys).startswith(want)


# @scenario SC-dialog-field-utf8-03
def test_find_field_takes_a_wide_character(tmp_path):
    """Find a typed euro sign: the match is where the cursor lands.

    A "c" stands before the euro sign, so a field that kept only the text
    typed after the euro sign would find that first "c" instead."""
    with WpeSession(str(tmp_path), "c " + EURO + "c d\n") as w:
        w.menu(ALT.SEARCH, "f")
        w.key(EURO, "c", delay=0.3)
        w.key("\r")
        w.key("X")
        w.save()
        t = w.text()
    assert "X" in t and t.index("X") > t.index(EURO), t


def _sgr_click(col, row):
    """An SGR 1006 left press and release at 0-based screen (col, row)."""
    return ("\x1b[<0;%d;%dM" % (col + 1, row + 1),
            "\x1b[<0;%d;%dm" % (col + 1, row + 1))


# @scenario SC-dialog-field-utf8-04
def test_click_after_wide_characters_puts_the_caret_between_letters(tmp_path):
    """A click on the second of three euro signs puts the caret before it.

    The click used to set the caret at the column number taken as a byte
    count, inside the first euro sign's bytes, and the next letter typed
    split it into bytes that are not UTF-8."""
    with WpeSession(str(tmp_path), "foo\n", env_extra={"TERM": "vt220"}) as w:
        w.menu(ALT.SEARCH, "r")
        w.key("foo")
        w.key("\033n")
        for k in (EURO, EURO, EURO):
            w.key(k, delay=0.3)
        rows = w.display()
        y = [i for i, r in enumerate(rows) if EURO * 3 in r][0]
        x = rows[y].index(EURO * 3) + 1
        w.key(*_sgr_click(x, y), delay=0.5)
        w.key("X", delay=0.3)
        w.key("\033p")
        w.key("\033a")
        w.key("\r")
        w.save()
        with open(tmp_path / "t.c", "rb") as fh:
            raw = fh.read()
    assert raw.startswith((EURO + "X" + EURO + EURO).encode()), raw


# @scenario SC-dialog-field-utf8-05
def test_go_to_line_drops_a_wide_character(tmp_path):
    """Go to Line takes digits: 2, a euro sign, 3 goes to line 23.

    The euro sign used to leave the number field, and the field came back
    with its first-key rule, so the 3 replaced the 2."""
    seed = "".join("L%d\n" % i for i in range(1, 40))
    with WpeSession(str(tmp_path), seed) as w:
        w.menu(ALT.SEARCH, "g")
        for k in ("2", EURO, "3"):
            w.key(k, delay=0.3)
        w.key("\r", delay=0.8)
        w.key("X")
        w.save()
        lines = w.text().split("\n")
    assert [i + 1 for i, line in enumerate(lines) if "X" in line] == [23], lines[:25]
