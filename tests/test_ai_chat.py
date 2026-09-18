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


def test_ai_chat_empty_reply_clears_placeholder(tmp_path):
    # A reply that streams nothing must not leave the "gathering..." liveness
    # placeholder on screen -- it becomes "(no answer)".
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "",
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("a")
        s.key("anything")
        s.key("\r", delay=1.0)
        s._drain(1.2)
        disp = "\n".join(s.display())
    assert "gathering the answer" not in disp, "placeholder stuck:\n" + disp
    assert "(no answer)" in disp, "empty reply not handled:\n" + disp


def test_ai_chat_investigates_files(tmp_path):
    # headless proof of the read-only tool loop: turn 1 is a read_file tool call,
    # turn 2 (mock @@TURN@@) is the answer.  Chat must run the tool on the real
    # file and then reach the answer -- all without a live model.
    (tmp_path / "notes.txt").write_text("hello from notes\n")
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "TOOL read_file notes.txt@@TURN@@ANSWERMARKER here it is",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="stack.c", env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("a")
        s.key("what does notes.txt say")
        s.key("\r", delay=1.2)
        s._drain(1.5)
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "turn=1" in txt, "chat did not take a second (tool) turn:\n" + txt
    assert "ANSWERMARKER" in disp, "chat did not reach the answer after the tool:\n" + disp
