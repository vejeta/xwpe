"""Typing (not just displaying) UTF-8 characters into the editor buffer.

test_utf8_border.py covers DISPLAY -- it opens files that already contain
accented / Cyrillic / CJK / emoji text and checks the screen.  This covers the
other half: characters TYPED (and pasted) at the keyboard.

xwpe's own key codes share the numeric range of Unicode codepoints -- the
Alt-<letter> codes are 271..305, the synthetics 2000..2006 -- so a typed
character whose codepoint lands there cannot be told from a key by value alone.
The backend that read the key now flags whether it decoded a character
(e_input_was_char), and character insertion trusts that flag for codepoints
>= 255 instead of guessing from the value.  These type one character of each
script -- including U+0130, whose codepoint (304) is exactly Alt-N's key code --
and assert the bytes reached disk.
"""
import pytest
from wpe_driver import WpeSession

TYPEABLE = [
    ("latin1_eacute",    "é"),      # 233    - control, always worked
    ("latinext_amacron", "ā"),      # 257    - just above the old ceiling
    ("dotted_capital_I", "İ"),      # 304    - collides EXACTLY with Alt-N
    ("greek_alpha",      "α"),      # 945
    ("cyrillic_de",      "д"),      # 1076
    ("hebrew_alef",      "א"),      # 1488
    ("arabic_beh",       "ب"),      # 1576
    ("cjk_ri",           "日"),      # 26085  - above the key-code band
    ("emoji_grin",       "\U0001F600"),  # 128512 - 4-byte
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
    # Bracketed paste (ESC[200~ ... ESC[201~) of characters across the range must
    # land in the buffer just like typing them -- each pasted char goes through
    # the same UTF-8 assembly and is-char flag.
    ESC = "\033"
    text = "AāдB日\U0001F600C"     # Latin-ext, Cyrillic, CJK, emoji
    with WpeSession(str(tmp_path), "", filename="t.txt") as s:
        s.key(ESC + "[200~" + text + ESC + "[201~", delay=0.5)
        s.save()
        disk = s.text()
    assert disk.startswith(text), \
        "pasted characters did not reach the buffer -- disk=%r" % disk
