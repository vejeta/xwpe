"""The AI reply wraps to the pane width instead of scrolling off to the right.

The model streams a paragraph as one long line (no newline of its own); the pane
must soft-wrap it at word boundaries so it reads DOWN the window, like a real
chat, rather than forcing the reader to scroll horizontally.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

# One logical line, ~240 columns, no embedded newline.
PARA = " ".join("word%02d" % i for i in range(40))


def test_long_reply_is_wrapped_across_lines(tmp_path):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": PARA,
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key("hi")
        s.key("\r", delay=1.2)
        s._drain(1.2)
        disp = s.display()
    text_lines = [ln for ln in disp if "word" in ln]
    # The paragraph must occupy several rows, not one 240-column line.
    assert len(text_lines) >= 3, \
        "reply was not wrapped across lines:\n" + "\n".join(disp)
    # No visible row is anywhere near the 240-column logical length.
    assert max(len(ln) for ln in text_lines) < 120, \
        "a wrapped line is still too wide:\n" + "\n".join(text_lines)
    # The first and last words landed on different rows (a real wrap, not a
    # truncation that dropped the tail).
    first = next(i for i, ln in enumerate(disp) if "word00" in ln)
    last = next(i for i, ln in enumerate(disp) if "word39" in ln)
    assert last > first, "the tail of the reply is missing after wrapping"
