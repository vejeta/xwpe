"""The history a chat turn sends: measured in bytes, and said when cut.

A chat turn sends the stored messages that fit 64 KiB of content, counted
from the newest backwards, whole messages only.  When a turn leaves earlier
messages out, one pane line says how many and why; the line comes back only
when that count changes.  (we_ai_ws.c: wpe_ai_session_window; we_ai_ui.c:
ai_history_take, ai_say_history_cut.)

The mock backend answers every turn, and the XWPE_AI_TRACE log records the
start of each message a turn sent ("ai history msg ... head=") and every
pane line about unsent history ("ai history cut said:"), so the tests read what was
sent from the request itself, never from a live provider.
"""
import glob
import json
import os
import re
import subprocess
import time
from pathlib import Path

import pytest
from wpe_driver import ALT, WPE_BIN, WpeSession

SEED = "int main(void){return 0;}\n"
REPLY = "noted"
PASTE_BEG = "\033[200~"
PASTE_END = "\033[201~"
QUESTION_FILL = 20000          # four of these cross the 65536-byte budget
STORE_MAX = 200                # WS_SESS_MAX in we_ai_ws.c
CUT_WORDS = re.compile(r"\[AI\] (\d+) earlier messages? not sent")
SAID = "ai history cut said:"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": REPLY,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _done_count(tmp_path):
    return _trace(tmp_path).count("chat done")


def _wait_done(tmp_path, count, timeout=20):
    """Wait until the trace holds `count` finished chat turns."""
    end = time.time() + timeout
    while time.time() < end:
        if _done_count(tmp_path) >= count:
            return True
        time.sleep(0.2)
    return False


def _arm_chat(s):
    """Open the chat pane straight into its input row (Alt-G, a)."""
    s.key(ALT.AI)
    s.key("a", delay=0.6)


def _ask(s, tmp_path, text, turn):
    """Paste `text` as one question, send it and wait for turn `turn` to end."""
    s.key(PASTE_BEG + text + PASTE_END, delay=0.8)
    s._drain(0.3)
    s.key("\r", delay=0.8)
    assert _wait_done(tmp_path, turn), "turn %d did not finish:\n%s" % (
        turn, _trace(tmp_path)[-2000:])


def _big_question(i):
    return "Q%dMARK " % i + "x" * QUESTION_FILL


def _turn_heads(tmp_path):
    """The message heads each turn sent, one list per turn, in order."""
    turns = []
    for line in _trace(tmp_path).splitlines():
        if line.startswith("ai history window "):
            turns.append([])
        elif line.startswith("ai history msg ") and turns:
            turns[-1].append(line)
    return turns


def _said(tmp_path):
    return [l for l in _trace(tmp_path).splitlines() if l.startswith(SAID)]


def _screen(s):
    return "\n".join(s.display(drain=0.3))


# @scenario SC-ai-chat-66
def test_a_long_conversation_sends_the_newest_and_says_so_once(tmp_path):
    """The fourth 20000-byte question leaves the first one out, and says so.

    The request of turn four carries the second, third and fourth questions
    and not the first.  The pane shows "1 earlier message not sent (history
    over 64 KiB)" once; a fifth, short question leaves the same one message
    out and the line is not said again.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        for i in range(1, 5):
            _ask(s, tmp_path, _big_question(i), i)
        after_four = _screen(s)
        said_four = _said(tmp_path)
        _ask(s, tmp_path, "Q5MARK short", 5)
        said_five = _said(tmp_path)
    turns = _turn_heads(tmp_path)
    assert len(turns) == 5, "expected five sent turns, got %d" % len(turns)
    fourth = "\n".join(turns[3])
    for kept in ("Q2MARK", "Q3MARK", "Q4MARK"):
        assert kept in fourth, "turn four did not send %s:\n%s" % (kept, fourth)
    assert "Q1MARK" not in fourth, "turn four still sent the first question:\n" + fourth
    third = "\n".join(turns[2])
    assert "Q1MARK" in third, "turn three should still send the first question:\n" + third
    assert "mock request bytes=%d " % len(_big_question(4)) in _trace(tmp_path)
    assert len(said_four) == 1, "the cut was not said exactly once: %r" % said_four
    m = CUT_WORDS.search(said_four[0])
    assert m and m.group(1) == "1" and "history over 64 KiB" in said_four[0], said_four
    assert "earlier message not sent" in after_four, \
        "the pane does not show the line:\n" + after_four
    assert said_five == said_four, \
        "the line was said again with the count unchanged: %r" % said_five
    fifth = "\n".join(turns[4])
    assert "Q1MARK" not in fifth and "Q2MARK" in fifth, fifth


# @scenario SC-ai-chat-66
def test_a_short_conversation_says_nothing(tmp_path):
    """Three short turns send everything and no line about history appears."""
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        for i in range(1, 4):
            _ask(s, tmp_path, "Q%dMARK short" % i, i)
        screen = _screen(s)
    turns = _turn_heads(tmp_path)
    assert len(turns) == 3, "expected three sent turns, got %d" % len(turns)
    assert "Q1MARK" in "\n".join(turns[2]), turns[2]
    assert not _said(tmp_path), _said(tmp_path)
    assert "earlier message" not in screen, screen


def _session_file(tmp_path):
    files = sorted(glob.glob(str(tmp_path / ".local" / "state" / "xwpe" / "ai" / "*.session")))
    assert files, "no session file written"
    return Path(files[0])


def _full_session():
    """A saved session already at the store's size, short messages."""
    msgs = []
    for i in range(STORE_MAX):
        role = "user" if i % 2 == 0 else "assistant"
        msgs.append({"role": role, "content": "OLD%03d" % i})
    return {"cli_session": "", "messages": msgs}


# @scenario SC-ai-chat-66
def test_messages_dropped_from_the_store_are_said(tmp_path):
    """A full saved session drops its oldest message, and the pane says so.

    The session file holds 200 short messages; the next question pushes the
    oldest out of the store, and the pane reads "1 earlier message not sent
    (older than the saved session)".
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        _ask(s, tmp_path, "FIRST short", 1)
    path = _session_file(tmp_path)
    path.write_text(json.dumps(_full_session()))
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        _ask(s, tmp_path, "NEWMARK short", 2)
        screen = _screen(s)
    said = _said(tmp_path)
    assert len(said) == 1, said
    assert "1 earlier message not sent (older than the saved session)" in said[0], said
    assert "older than the saved session" in screen, screen
    last = "\n".join(_turn_heads(tmp_path)[-1])
    assert "OLD000" not in last and "OLD001" in last and "NEWMARK" in last, last
