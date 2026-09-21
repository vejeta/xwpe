"""The AI pane shows which backend/model/policy is active (discoverability).

The bottom bar is a fixed 80-column layout with no room for a variable model
name, so the "who am I talking to" indicator lives at the top of the AI pane:
backend, model and permission policy.  Shown for both chat and the agent.
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


def test_chat_header_shows_backend_and_policy(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok", "XWPE_AI_POLICY": "edits"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI); s.key("a"); s._drain(0.5)
        disp = "\n".join(s.display())
    assert "[AI:" in disp and "mock" in disp, \
        "the chat header does not show the backend:\n" + disp
    assert "policy: edits" in disp, \
        "the chat header does not show the permission policy:\n" + disp


def test_agent_header_shows_backend(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "DONE ok", "XWPE_AI_POLICY": "ask"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g")
        s.key("do it"); s.key("\r", delay=1.2)
        s._drain(1.0)
        disp = "\n".join(s.display())
    assert "[AI:" in disp and "mock" in disp, \
        "the agent header does not show the backend:\n" + disp
