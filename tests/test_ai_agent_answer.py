"""Agent: a DONE reply's answer/summary is shown in full, not just its first line.

When an agent task is really a question, the model has nothing to edit and answers
directly.  It replies with a DONE line followed by the answer; the pane used to
show only the first line ("DONE - answering directly"), dropping the answer.  The
whole DONE reply is now rendered, so the user sees the answer even though this is
not the chat.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

REPLY = ("DONE answering your question directly\n"
         "Capability ALPHA: it can read files.\n"
         "Capability BETA: it can run the build.")


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_agent_done_shows_full_answer(tmp_path):
    trace = tmp_path / "a.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": REPLY,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("what can you do"); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.5)
            if "agent done" in (trace.read_text() if trace.exists() else ""):
                break
        disp = "\n".join(s.display())
    # both answer lines that follow the DONE marker are visible, not just line one
    assert "Capability ALPHA" in disp and "Capability BETA" in disp, \
        "the agent's multi-line answer was truncated to its first line:\n" + disp
