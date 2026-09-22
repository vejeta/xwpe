"""The chat input caret lands AFTER an accented character, not on top of it.

The AI pane's input row stores UTF-8 bytes and is drawn by the ordinary editor
cursor code, which treats the caret x as a BYTE offset and collapses a
multi-byte glyph to its single display column itself (e_utf8_visual_step).  The
input caret must therefore be a byte offset too.  Returning a glyph count put
the caret on the trailing byte of an e-acute, so e_cursor drew it one column too
far left -- the cursor "stuck" on top of the accented character while typing.

Runs only against a --enable-ai build (Alt-G is compiled out otherwise).
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

E_ACUTE = "é"   # precomposed U+00E9, sent to the pty as UTF-8 c3 a9


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_input_caret_after_accented_char(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)      # Ask -> the "> " input row
        s.key("a" + E_ACUTE, delay=0.4)           # type 'a' then e-acute
        rows = s.display()
        cy, cx = s.screen.cursor.y, s.screen.cursor.x

    assert any(">" in r and E_ACUTE in r for r in rows), \
        "the accented char did not render in the input row:\n" + "\n".join(rows)
    # The caret sits just PAST the e-acute: the cell to its left is the glyph.
    assert cx >= 1 and rows[cy][cx - 1] == E_ACUTE, (
        "caret is not immediately after the e-acute (row=%r cx=%d):\n%s"
        % (rows[cy], cx, "\n".join(rows)))
    # And it is NOT parked on top of the e-acute (the pre-fix symptom).
    assert rows[cy][cx] != E_ACUTE, \
        "caret is sitting on top of the e-acute:\n" + "\n".join(rows)
