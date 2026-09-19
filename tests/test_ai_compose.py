"""AI assistant -- a multi-line chat prompt.

The chat pane is a normal window with a fixed input row: Enter sends, Ctrl-J
starts a new line.  This drives a two-line prompt (line one, Ctrl-J, line two,
Enter) and checks it reaches the backend whole.
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


def test_ai_compose_multiline(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok", "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="stack.c", env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.8)       # Ask -> arm the chat pane
        s.key("FIRSTLINE_MARKER")                   # line 1
        s.key("\x0a", delay=0.3)                    # Ctrl-J -> newline (Enter would send)
        s.key("SECONDLINE_MARKER")                  # line 2
        s.key("\r", delay=1.0)                      # Enter -> send
        s._drain(1.0)
    txt = trace.read_text() if trace.exists() else ""
    assert "FIRSTLINE_MARKER" in txt and "SECONDLINE_MARKER" in txt, \
        "the multi-line prompt did not reach the backend whole:\n" + txt
