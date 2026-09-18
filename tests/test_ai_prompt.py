"""AI assistant -- the prompt dialog accepts a long instruction.

The old shared input dialog capped at 128 characters; the AI prompt uses a wide
field that holds a real instruction.  This drives a ~195-char prompt and checks
it reaches the backend intact.
"""
import os
import re
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-B AI" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_ai_prompt_accepts_long_text(tmp_path):
    trace = tmp_path / "ai.trace"
    longp = "please refactor the parser and " + ("x" * 160) + " end"   # 195 chars
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok", "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="stack.c", env_extra=env) as s:
        s.key(ALT.BLOCK); s.key("a", delay=0.8)
        s.key(longp)
        s.key("\r", delay=1.5)
        s._drain(1.0)
    txt = trace.read_text() if trace.exists() else ""
    assert longp in txt, "the long prompt was truncated (128-char cap regressed)"
