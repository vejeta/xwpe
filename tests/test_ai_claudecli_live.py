"""AI assistant -- LIVE test of the `claudecli` backend (the `claude` CLI as a
subprocess, using the user's own Claude Code login -- no API key, no TLS, no
extra dependency).  Self-skips unless the `claude` binary is on PATH.
"""
import os
import shutil
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


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(shutil.which("claude") is None, reason="no `claude` CLI"),
]


def test_ai_claudecli_chat(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("a")
        s.key("Reply with exactly the single word PONG")
        s.key("\r", delay=2.0)
        # claude -p takes a while; wait for completion
        waited = 0.0
        while waited < 120:
            s._drain(2.0)
            waited += 2.0
            if trace.exists() and "chat done" in trace.read_text():
                break
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "backend=claudecli" in txt, txt
    assert "chat done" in txt, "claude CLI never completed:\n" + txt
    assert "pong" in disp.lower(), "reply not visible in pane:\n" + disp
