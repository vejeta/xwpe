"""The AI chat pane docks along the bottom, not over the editor.

Opening the chat used to drop the pane on top of the editor window.  It now
docks like the Messages window: the editor keeps the top of the screen and the
conversation reads down the bottom half, so the code stays visible while you
chat.
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


def test_ai_pane_docks_below_editor(tmp_path):
    src = "int UNIQUE_EDITOR_MARKER(void){\n  return 12345;\n}\n"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "DOCKREPLY",
    }
    with WpeSession(str(tmp_path), src, env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")
        s._drain(0.5)
        s.key("hi")
        s.key("\r", delay=1.2)
        s._drain(1.0)
        disp = s.display()
        s.key("\033", delay=0.4)

    def row(needle):
        for i, ln in enumerate(disp):
            if needle in ln:
                return i
        return -1

    ed = row("UNIQUE_EDITOR_MARKER")
    reply = row("DOCKREPLY")
    assert ed >= 0, "editor content is hidden (pane overlaps it):\n" + "\n".join(disp)
    assert reply >= 0, "AI reply not shown:\n" + "\n".join(disp)
    assert ed < reply, "editor is not above the AI pane (still overlapping):\n" \
        + "\n".join(disp)
    # The AI window has its own title bar between the two.
    assert any("AI" in ln and "q" in ln for ln in disp[ed:reply]), \
        "no AI pane frame between the editor and the reply:\n" + "\n".join(disp)
