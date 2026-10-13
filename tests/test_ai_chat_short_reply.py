"""A chat reply shorter than a tool line paints when the stream ends.

The streaming painter holds the first bytes of a reply until it can tell an
answer from a "TOOL ..." investigation line: five bytes or a newline decide.
A reply under five bytes with no newline ("ok", "Yes", "42") never reached
that point, and the finish path freed the held bytes without painting them,
so the pane stayed on "AI: (gathering the answer...)" after the trace said
"chat done".  Bytes still held when the stream ends are the answer and are
painted as such.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
GATHERING = "AI: (gathering the answer...)"
TRACE_POLL_S = 10


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "ask",
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }


def _trace(tmp_path):
    trace = tmp_path / "ai.trace"
    return trace.read_text() if trace.exists() else ""


def _traced_within(tmp_path, needle, timeout):
    """Wait until the trace holds `needle` or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return False


def _chat_screen_after(tmp_path, reply):
    """Ask one question, wait for the turn to end, return the screen rows."""
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path, reply)) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        s.key("hi"); s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "chat done", TRACE_POLL_S), \
            "the chat turn never finished:\n" + _trace(tmp_path)
        s._drain(1.0)
        return s.display()


# @scenario SC-ai-chat-short-reply
@pytest.mark.parametrize("reply", ["ok", "42"])
def test_chat_reply_under_five_bytes_paints(tmp_path, reply):
    rows = _chat_screen_after(tmp_path, reply)
    text = "\n".join(rows)
    assert "AI: " + reply in text, \
        "the %d-byte reply %r never painted:\n%s" % (len(reply), reply, text)
    assert GATHERING not in text, \
        "the pane is still gathering after the turn ended:\n" + text
