"""The pane's first row is the first line written into it, never a blank.

The AI pane is a window like any other, and a fresh window's buffer holds one
empty line.  Every writer appended AFTER that line, so the pane opened with a
blank row above its first line: the header in a chat, the "[AI edit] working"
line in an edit.  In a pane that shares the bottom strip with Messages and has
ten rows, that row is a tenth of what the user can read.

Both ways into the pane are checked, because they write through different
paths: the chat (the input region owns the bottom rows) and a background task
(the line is appended to the end).
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_rows

SEED = "int main(void){return 0;}\n"
HEADER = "[AI: mock"
EDIT_LINE = "[AI edit]"
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1") or 1)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, **more):
    e = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
         "XWPE_AI_MOCK_REPLY": "hello",
         "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}
    e.update(more)
    return e


def _seen_within(s, needle, timeout):
    end = time.time() + timeout * WAIT_SCALE
    while time.time() < end:
        s._drain(0.3)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _first_row(s):
    """The pane's first interior row, as the user reads it."""
    rows = pane_rows(s.display())
    assert rows, "the pane has no rows:\n" + "\n".join(s.display(drain=0))
    return rows[0].rstrip()


# @scenario SC-ai-chat-57
def test_chat_pane_opens_with_the_header_on_its_first_row(tmp_path):
    """Alt-G a: the header is the pane's first row, with no blank above it."""
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        s.key(ALT.AI)
        s.key("a", delay=0.8)
        s._drain(0.5)
        first = _first_row(s)
        rows = [r.rstrip() for r in pane_rows(s.display())]
    assert HEADER in first, \
        "the pane's first row is not the header (%r); rows:\n%s" % (first, "\n".join(rows[:5]))


# @scenario SC-ai-chat-58
def test_edit_pane_opens_with_its_first_line_on_the_first_row(tmp_path):
    """Alt-G e: the task's first line is the pane's first row, no blank above."""
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY=SEED,
               XWPE_AI_MOCK_DELAY_MS=str(int(2500 * WAIT_SCALE)))
    with WpeSession(str(tmp_path), SEED, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI)
        s.key("e", delay=0.6)
        s.key("tidy it")
        s.key("\r", delay=0.6)
        assert _seen_within(s, EDIT_LINE, 10), \
            "the edit never wrote into the pane:\n" + "\n".join(s.display(drain=0))
        first = _first_row(s)
        rows = [r.rstrip() for r in pane_rows(s.display())]
    assert first != "", \
        "the pane's first row is blank; rows:\n%s" % "\n".join(rows[:5])
