"""AI assistant -- Chat mode smoke test (deterministic mock backend).

Runs only against a --enable-ai build (the AI entry point is compiled out
otherwise, so Alt-G is a no-op and this test self-skips).  Uses the in-process
mock backend (XWPE_AI_BACKEND=mock) so no Ollama/model/network is needed: the
reply text is whatever XWPE_AI_MOCK_REPLY says, streamed through the full HTTP
framer + fd-loop path.  Must run the programming-mode `wpe` binary (Alt-G is
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
        s.key(ALT.AI)              # Alt-G: the AI prefix
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
        s.key(ALT.AI)
        s.key("a")
        s.key("anything")
        s.key("\r", delay=1.0)
        s._drain(1.2)
        disp = "\n".join(s.display())
    assert "gathering the answer" not in disp, "placeholder stuck:\n" + disp
    assert "(no answer)" in disp, "empty reply not handled:\n" + disp


def test_ai_reply_shares_the_AI_line(tmp_path):
    # The "AI:" speaker label and the answer must be on the SAME line
    # ("AI: <text>"), not the label alone on one line with the reply below it.
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "PONGMARKER on the same line",
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key("hi")
        s.key("\r", delay=1.0)
        s._drain(1.2)
        disp = s.display()
    joined = "\n".join(disp)
    assert "PONGMARKER" in joined, "reply not rendered:\n" + joined
    # find the line carrying the reply: it must also carry the "AI:" label.
    hit = [ln for ln in disp if "PONGMARKER" in ln]
    assert hit and any("AI:" in ln for ln in hit), \
        "reply is not on the 'AI:' line (label split from answer):\n" + joined


def test_ai_multiline_carries_over_typed_text(tmp_path):
    # Typing in the one-line Ask box then switching to the multi-line composer
    # (Alt-M) must KEEP what was typed, so the user does not retype it.  Esc
    # finishes the composer and 'y' confirms Send; the sent prompt must equal
    # the text seeded from the one-line field.
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "ok",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")
        s.key("carry me over")        # type into the one-line field
        s._drain(0.4)
        s.key("\033m", delay=0.8)     # Alt-M -> multi-line composer (seeded)
        s._drain(0.6)
        s.key("\033", delay=0.8)      # Esc -> finish composing
        s._drain(0.5)
        s.key("y", delay=1.0)         # confirm Send
        s._drain(1.2)
    txt = trace.read_text() if trace.exists() else ""
    prompts = [l for l in txt.splitlines() if l.startswith("chat prompt=")]
    assert prompts, "no chat was submitted:\n" + txt
    assert any("carry me over" in l for l in prompts), \
        "the composer lost the text typed in the one-line box:\n" + txt


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
        s.key(ALT.AI)
        s.key("a")
        s.key("what does notes.txt say")
        s.key("\r", delay=1.2)
        s._drain(1.5)
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "turn=1" in txt, "chat did not take a second (tool) turn:\n" + txt
    assert "ANSWERMARKER" in disp, "chat did not reach the answer after the tool:\n" + disp
