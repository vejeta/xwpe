"""Typing UTF-8 across the key-code-overlap band works under a real X server.

The pyte suite proves the ncurses path; this proves the X11 backend end to end
with xdotool injecting real key events into a real xwpe window: a codepoint that
shares the numeric range of an xwpe key code (Latin Extended, the U+0130 that is
exactly Alt-N, Cyrillic, CJK) must be inserted as text, not swallowed as a key.
PIL is not needed here (no screenshot), so it runs where the pixel tests cannot.
"""
import pytest

CASES = [
    ("latinext_amacron", "ā"),   # 257
    ("dotted_capital_I", "İ"),   # 304 == Alt-N's key code
    ("cyrillic_de",      "д"),   # 1076
    ("cjk_ri",           "日"),   # 26085
]


@pytest.mark.parametrize("label,ch", CASES, ids=[c[0] for c in CASES])
def test_x11_type_utf8(xwpe, label, ch):
    marker = "X" + ch + "Y"
    # xdotool injects a non-ASCII glyph by remapping a spare keycode to its
    # keysym on the fly; typing it in the same burst as neighbouring keys races
    # that remap, so give the special character its own call and extra settle.
    xwpe.type("X", delay=0.4)
    xwpe.type(ch, delay=0.9)
    xwpe.type("Y", delay=0.4)
    xwpe.save()
    txt = xwpe.saved_text()
    assert marker in txt, \
        "X11: typed %r (U+%04X) not saved -- got %r" % (ch, ord(ch), txt[:24])
