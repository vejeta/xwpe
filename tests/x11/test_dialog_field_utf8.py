"""A dialog text field keeps what was typed before a character above Latin-1.

Twin of tests/test_dialog_field_utf8.py under a real X server: typing
ab<euro>cd into Search > Replace's New Text field used to leave "cd".  The
characters are typed one call each, because xdotool remaps a spare keycode for
a non-ASCII glyph and a burst races that remap.
"""
import pytest

EURO = "€"          # 3 bytes
CLEF = "\U0001d11e"      # 4 bytes
EACUTE = "é"        # 2 bytes


def replace_all_with(xwpe, steps):
    """Replace "int" with what the steps leave in New Text, Change All.

    A step is ("t", text) to type or ("k", key) to press."""
    xwpe.menu("s", "r")
    xwpe.type("int")
    xwpe.key("alt+n")
    for kind, what in steps:
        if kind == "t":
            xwpe.type(what, delay=0.9)
        else:
            xwpe.key(what, delay=0.4)
    xwpe.key("alt+p")
    xwpe.key("alt+a")
    xwpe.key("Return")
    xwpe.save()
    assert xwpe.proc.poll() is None
    return xwpe.saved_text()


def T(s):
    return ("t", s)


def K(k):
    return ("k", k)


CASES = [
    ("euro_inside", [T("a"), T("b"), T(EURO), T("c"), T("d")],
     "ab" + EURO + "cd"),
    ("clef_inside", [T("a"), T(CLEF), T("b")], "a" + CLEF + "b"),
    ("two_and_three", [T(EACUTE), T(EURO), T(EACUTE)],
     EACUTE + EURO + EACUTE),
    ("backspace_over_euro",
     [T("a"), T("b"), T(EURO), T("c"), T("d"), K("Left"), K("Left"),
      K("BackSpace")], "abcd"),
    ("delete_over_euro",
     [T("a"), T("b"), T(EURO), T("c"), T("d"), K("Left"), K("Left"),
      K("Left"), K("Delete")], "abcd"),
    ("backspace_over_clef",
     [T("a"), T(CLEF), T("b"), K("Left"), K("BackSpace")], "ab"),
    ("left_right_over_euro",
     [T("a"), T(EURO), T("b"), K("Left"), K("Left"), K("Right"), T("X")],
     "a" + EURO + "Xb"),
    ("home_then_right_over_clef",
     [T("a"), T(CLEF), T("b"), K("Home"), K("Right"), K("Right"), T("X")],
     "a" + CLEF + "Xb"),
]


@pytest.mark.parametrize("label,steps,want", CASES, ids=[c[0] for c in CASES])
def test_x11_replace_field_with_wide_characters(xwpe, label, steps, want):
    t = replace_all_with(xwpe, steps)
    assert want in t, "X11: New Text should be %r, saved text is %r" % (want, t[:60])
