"""AI assistant -- LIVE cloud tests over real HTTPS (needs --enable-ai-tls).

These hit a real provider and cost a few tokens, so each self-skips unless the
matching API key is in the environment.  They exercise the TLS handshake + the
SSE streaming parse end to end through the editor.  Run:
  OPENAI_API_KEY=... WPE_BIN=../wpe python -m pytest tests/test_ai_live.py -v
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _tls_build():
    try:
        out = subprocess.run(["ldd", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"libssl" in out or b"libtls" in out
    except Exception:
        return False


def _chat(tmp_path, backend, endpoint, model):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": backend,
        "XWPE_AI_ENDPOINT": endpoint,
        "XWPE_AI_MODEL": model,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)                 # Alt-B
        s.key("a")                       # Ask
        s.key("Reply with exactly the single word PONG")
        s.key("\r", delay=2.0)           # submit -> real streaming reply
        # give the network + fd-loop time to finish
        for _ in range(12):
            s._drain(1.5)
            if trace.exists() and "chat done" in trace.read_text():
                break
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "chat stream fd=" in txt, "stream never started:\n" + txt
    assert "chat done" in txt, "stream never completed:\n" + txt
    return disp


@pytest.mark.skipif(not _tls_build(), reason="wpe built without TLS")
@pytest.mark.skipif(not os.environ.get("OPENAI_API_KEY"),
                    reason="OPENAI_API_KEY not set")
def test_ai_live_openai(tmp_path):
    disp = _chat(tmp_path, "openai", "https://api.openai.com", "gpt-4o-mini")
    assert "pong" in disp.lower(), "model reply not visible in pane:\n" + disp


@pytest.mark.skipif(not _tls_build(), reason="wpe built without TLS")
@pytest.mark.skipif(not os.environ.get("ANTHROPIC_API_KEY"),
                    reason="ANTHROPIC_API_KEY not set")
def test_ai_live_claude(tmp_path):
    disp = _chat(tmp_path, "claude", "https://api.anthropic.com", "claude-sonnet-5")
    assert "pong" in disp.lower(), "model reply not visible in pane:\n" + disp
