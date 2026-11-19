"""AI assistant -- LIVE test of the `claudecli` backend (the `claude` CLI as a
subprocess, using the user's own Claude Code login -- no API key, no TLS, no
extra dependency).  Self-skips unless the `claude` binary is on PATH.
"""
import os
import subprocess
import pytest
import live_ai
from host_tools import needs
from live_gate import require_live_gate
from wpe_driver import WpeSession, ALT, WPE_BIN

require_live_gate()


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    needs("claude"),
]


def test_ai_claudecli_chat(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_TRACE": str(trace),
        # This row talks to the real CLI: only a live file may keep the
        # machine's PATH (tests/engine_guard.py).
        "XWPE_TEST_REAL_ENGINE": "1",
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, real_login=True,
                    inherit=live_ai.KEY_INHERIT) as s:
        s.key(ALT.AI)
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
    # The editor resumes a conversation only with a session id of the shape
    # 8-4-4-4-12 hex digits (wpe_ai_session_id_valid) and says "resume id
    # refused" when the CLI answers another one: this row is where a change in
    # the CLI's id shape shows, since the stub rows can only assume it.
    assert "resume id refused" not in txt, \
        "the real CLI answered a session id of another shape:\n" + txt


def test_ai_claudecli_agent_uses_tool_protocol(tmp_path):
    """policy=auto must still drive OUR tool protocol via the claude CLI.

    The CLI is kept text-only so it replies "TOOL list_dir ." etc. and xwpe runs
    each tool.  The old dial mapping turned auto into --dangerously-skip-
    permissions, letting the CLI use its own tools and never emitting a TOOL
    line -- this asserts a TOOL line appears.  Self-skips if this HOME has no
    claude login (the test asks the driver to link ~/.claude in, but CI may
    have none)."""
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "claudecli",
        "XWPE_AI_POLICY": "auto",
        "XWPE_AI_TRACE": str(trace),
        # The real CLI again: see the first row.
        "XWPE_TEST_REAL_ENGINE": "1",
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c", real_login=True,
                    inherit=live_ai.KEY_INHERIT) as s:
        s.key(ALT.AI); s.key("g")
        s.key("Run list_dir on the current directory, then reply DONE. Change nothing.")
        s.key("\r", delay=2.0)
        waited = 0.0
        while waited < 150:
            s._drain(2.0); waited += 2.0
            t = trace.read_text() if trace.exists() else ""
            if "agent tool=" in t or "conv error" in t or "agent done" in t:
                break
        txt = trace.read_text() if trace.exists() else ""
    if "conv error" in txt or "Not logged in" in txt or "could not start" in txt:
        pytest.skip("claude CLI not logged in for this HOME")
    assert "agent tool=list_dir" in txt, \
        "claudecli agent did not run our TOOL protocol (dial->own-tools regression?):\n" + txt
