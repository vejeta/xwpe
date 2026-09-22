"""Typing (not just displaying) UTF-8 characters into the editor buffer.

test_utf8_border.py covers DISPLAY -- it opens files that already contain
accented / Cyrillic / CJK / emoji text and checks the screen.  This covers the
other half: characters TYPED at the keyboard.

Background.  The terminal (ncurses) input path assembles a typed multi-byte
character into its Unicode codepoint.  A codepoint >= 256 used to be mistaken
for an ncurses KEY_ code and swallowed by the function-key switch, so a typed
CJK / emoji character never reached the buffer (only Latin-1, <= U+00FF, could
be typed).  e_t_getch now flags a decoded character so it bypasses that switch.

Known remaining gap (see the xfail below): xwpe's own keycodes overlap the
Unicode range -- the Alt-<letter> codes are 271..305 (AltF == 288), right on top
of Latin Extended-A -- so a codepoint in roughly U+0100..U+07D6 cannot be told
apart from a real keycode by value alone, and the insert guard floors at U+00FF
to stay below the keycodes.  Characters ABOVE that band (CJK, emoji) are
unaffected and must type; closing the band needs a cross-backend "this getch was
a character" signal and is tracked separately.
"""
import pytest
from wpe_driver import WpeSession

# Characters that MUST type: Latin-1 (the control, always worked) and the wide
# glyphs above the keycode band (CJK, emoji).
TYPEABLE = [
    ("latin1_eacute", "é"),      # 233     - control (< 256)
    ("cjk_ri",        "日"),      # 26085   - 3-byte, above the keycode band
    ("emoji_grin",    "\U0001F600"),  # 128512  - 4-byte
]


@pytest.mark.parametrize("label,ch", TYPEABLE, ids=[c[0] for c in TYPEABLE])
def test_type_utf8_into_editor(tmp_path, label, ch):
    with WpeSession(str(tmp_path), "", filename="t.txt") as s:
        s.key("A" + ch + "B", delay=0.25)   # type around it so a drop is visible
        s.save()
        disk = s.text()
    assert disk.startswith("A" + ch + "B"), (
        "typed char %r (U+%04X) did not reach the buffer -- disk=%r"
        % (ch, ord(ch), disk))


def test_paste_wide_chars_into_editor(tmp_path):
    # Bracketed paste (ESC[200~ ... ESC[201~) of wide characters must land in the
    # buffer, the same as typing them -- each pasted char goes through the same
    # UTF-8 assembly.  This is the copy/paste half of the fix.
    ESC = "\033"
    text = "A日B\U0001F600C"          # A CJK B emoji C
    with WpeSession(str(tmp_path), "", filename="t.txt") as s:
        s.key(ESC + "[200~" + text + ESC + "[201~", delay=0.5)
        s.save()
        disk = s.text()
    assert disk.startswith(text), \
        "pasted wide characters did not reach the buffer -- disk=%r" % disk


@pytest.mark.xfail(reason="U+0100..U+07D6 collides with xwpe keycodes "
                          "(Alt-letters 271..305); needs the is-char signal",
                   strict=True)
def test_type_latin_extended_into_editor(tmp_path):
    # a-macron (U+0101 = 257) sits in the keycode band and is dropped today.
    # When the cross-backend is-char signal lands this XPASSes and must be
    # promoted into TYPEABLE above.
    ch = "ā"
    with WpeSession(str(tmp_path), "", filename="t.txt") as s:
        s.key("A" + ch + "B", delay=0.25)
        s.save()
        disk = s.text()
    assert disk.startswith("A" + ch + "B")
