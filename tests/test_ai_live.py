"""AI assistant -- LIVE cloud tests over real HTTPS (needs --enable-ai-tls).

These hit a real provider and cost a few tokens, so each self-skips unless it
is told where to go: the Claude test needs ANTHROPIC_API_KEY, the
OpenAI-compatible test needs OPENAI_BASE_URL and OPENAI_MODEL.  They exercise
the TLS handshake + the SSE streaming parse end to end through the editor.  Run:
  ANTHROPIC_API_KEY=... WPE_BIN=../wpe python -m pytest tests/test_ai_live.py -v
"""
import os
import subprocess
import pytest
import live_ai
from wpe_driver import WpeSession, ALT, WPE_BIN


def _tls_build():
    try:
        out = subprocess.run(["ldd", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"libssl" in out or b"libtls" in out
    except Exception:
        return False


REPLY_BUDGET = 150   # seconds at scale 1 for the model to finish its reply
PROMPT = live_ai.CHAT_PROMPT
_reply_shows_answer = live_ai.reply_shows_answer


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
        s.key(ALT.AI)                 # Alt-G
        s.key("a")                       # Ask
        s.key(PROMPT)
        s.key("\r", delay=2.0)           # submit -> real streaming reply
        # A local model may need to load and think before it streams: wait on
        # the clock, stretched by XWPE_TEST_WAIT_SCALE.
        live_ai.wait_for_trace(trace, "chat done", s, REPLY_BUDGET)
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "chat stream fd=" in txt, "stream never started:\n" + txt
    assert "chat done" in txt, "stream never completed:\n" + txt
    assert not live_ai.has_backend_error(disp, trace), \
        "the provider returned an error:\n" + disp + "\n--- trace ---\n" + txt
    return disp


def test_ai_live_openai_compatible(tmp_path):
    """Chat through the OpenAI backend against the server the user names.

    Endpoint and model come from OPENAI_BASE_URL and OPENAI_MODEL; the test
    skips when either is unset and never falls back to a remote host.  Against
    a local Ollama:
      OPENAI_BASE_URL=http://localhost:11434 OPENAI_MODEL=<model> \\
        WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q \\
        tests/test_ai_live.py -k openai
    OPENAI_API_KEY is optional (a local server needs none); the editor reads
    it from the environment itself when it is set.
    """
    endpoint, model, reason = live_ai.openai_compatible()
    if reason:
        pytest.skip(reason)
    if live_ai.endpoint_needs_tls(endpoint) and not _tls_build():
        pytest.skip("wpe built without TLS, endpoint is https")
    disp = _chat(tmp_path, "openai", endpoint, model)
    assert _reply_shows_answer(disp), "model reply not visible in pane:\n" + disp


@pytest.mark.skipif(not _tls_build(), reason="wpe built without TLS")
@pytest.mark.skipif(not os.environ.get("ANTHROPIC_API_KEY"),
                    reason="ANTHROPIC_API_KEY not set")
def test_ai_live_claude(tmp_path):
    # Override with a valid Anthropic model id, e.g.
    #   CLAUDE_MODEL=claude-3-5-sonnet-latest
    model = os.environ.get("CLAUDE_MODEL", live_ai.CLAUDE_DEFAULT_MODEL)
    disp = _chat(tmp_path, "claude", "https://api.anthropic.com", model)
    assert _reply_shows_answer(disp), "model reply not visible in pane:\n" + disp
