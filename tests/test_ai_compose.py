"""AI assistant -- the multi-line prompt composer.

The prompt dialog has a "Multi-line" button (Alt-M) that opens a real editor
window as a text area: you write several lines with the full editor, finish
with Esc, and confirm Send.  This drives that flow and checks a two-line prompt
reaches the backend whole.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Send this prompt to the AI?" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_ai_compose_multiline(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok", "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="stack.c", env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.8)   # Ask -> wide prompt dialog
        s.key("\033m", delay=1.0)                  # Alt-M -> multi-line composer
        s.key("FIRSTLINE_MARKER"); s.key("\r")     # line 1 + newline (Enter edits)
        s.key("SECONDLINE_MARKER")                 # line 2
        s.key("\033", delay=1.0)                   # Esc -> finish editing
        s.key("y", delay=1.2)                      # confirm Send
        s._drain(1.0)
    txt = trace.read_text() if trace.exists() else ""
    assert "FIRSTLINE_MARKER" in txt and "SECONDLINE_MARKER" in txt, \
        "the multi-line prompt did not reach the backend whole:\n" + txt
