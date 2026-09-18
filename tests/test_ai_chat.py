"""AI assistant -- Chat mode smoke test (deterministic mock backend).

Runs only against a --enable-ai build (the AI entry point is compiled out
otherwise, so Alt-B is a no-op and this test self-skips).  Uses the in-process
mock backend (XWPE_AI_BACKEND=mock) so no Ollama/model/network is needed: the
reply text is whatever XWPE_AI_MOCK_REPLY says, streamed through the full HTTP
framer + fd-loop path.  Must run the programming-mode `wpe` binary (Alt-B is
dispatched in e_prog_switch).
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    """True if WPE_BIN was built with --enable-ai (strings finds our marker)."""
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out   # from the AI system prompt
    except Exception:
        return False


@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_ai_chat_mock(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "PONGMARKER123",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)              # Alt-B: the AI prefix
        s.key("a")                    # a = Ask (chat)
        s.key("hello")                # type the prompt into the dialog
        s.key("\r", delay=1.0)        # submit; the mock streams asynchronously
        s._drain(1.2)                 # let the fd-loop deliver the reply
        disp = "\n".join(s.display())
        assert "PONGMARKER123" in disp, "reply not rendered:\n" + disp

    txt = trace.read_text() if trace.exists() else ""
    assert "chat prompt=hello" in txt, txt
    assert "chat done" in txt, txt
