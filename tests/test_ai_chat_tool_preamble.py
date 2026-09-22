"""Chat runs a TOOL investigation even when the model prefaces it with prose.

A code model often answers a chat question by first saying what it will do
("Let me look at the current code") and THEN emitting the tool line
(``TOOL read_file: <path>``).  The chat loop must find that line past the
preamble, run the read-only tool, feed the result back, and continue to the real
answer -- not stall showing the raw TOOL line.  It must also tolerate the
``read_file:`` colon models commonly write.
"""
import os
import time
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


def test_chat_tool_after_preamble(tmp_path):
    src = os.path.join(str(tmp_path), "t.c")
    seed = "int meaning(void){return 42;}\n"
    # Turn 1: a reasoning sentence THEN a TOOL line that carries a colon.
    # Turn 2: the answer, which only appears if turn 1's tool actually ran and
    # the conversation continued.
    reply = ("Let me look at the current code\n"
             "TOOL read_file: %s"
             "@@TURN@@Based on the file, the answer is ANSWERMARK99." % src)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": reply}
    with WpeSession(str(tmp_path), seed, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("a", delay=0.6)      # Alt-G a: Ask
        s.key("what does this do", delay=0.1)
        s.key("\r", delay=1.0)
        time.sleep(3)
        disp = "\n".join(s.display())
    assert "ANSWERMARK99" in disp, \
        "chat did not run the tool past the preamble and reach the answer:\n" + disp
    # The raw protocol line is replaced by a dim "  . reading <path>" status, so
    # the model's reasoning shows but "TOOL read_file:" is never echoed as text.
    assert "TOOL read_file" not in disp, \
        "the raw TOOL protocol line was shown instead of a status:\n" + disp
    assert "reading" in disp, \
        "the investigation status was not shown:\n" + disp
