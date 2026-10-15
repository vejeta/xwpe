"""A chat turn is busy like every other assistant action.

While a chat reply streams, Esc cancels the turn with the shared
"[AI] cancelled" line and leaves the input armed; a second question typed
meanwhile is queued and runs when the turn ends, the running turn is not cut.
The waiting line "AI: (gathering the answer...)" carries the shared spinner
glyph, the seconds elapsed and "(Esc cancels)", rewritten by the heartbeat.
The header "[AI: <backend> | model: <m> | policy: <p>]" and the two hint lines
appear at the first arm whatever another action wrote to the pane before, and
a new header precedes the next turn when the backend, the model or the
permission dial changed since the last one.  Ctrl-W on the pane during a
reply ends the stream, asks no save question and no token re-creates the
pane.  A peer that closes with no bytes replaces the waiting line with the
transport-error line instead of leaving it standing.

The mock backend cannot fail below HTTP, so the transport test stands up the
same loopback fault server the agent gap tests use.
"""
import os
import re
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN
from test_ai_agent_gaps import _fault_server, _seed_openai_xwperc, _openai_env

SEED = "int main(void){return 0;}\n"
ESC = "\033"
CTRL_W = "\x17"

CANCELLED_LINE = "[AI] cancelled"
GATHERING = "gathering the answer"
TRANSPORT_LINE = "[AI: transport error]"
HEADER_ASK = "[AI: mock | model: mock-model | policy: ask]"
HEADER_EDITS = "[AI: mock | model: mock-model | policy: edits]"
HINT_KEYS = "[Enter=send  Ctrl-J=newline  arrows move  PgUp/PgDn scroll  Esc leaves]"
HINT_SLASH = "[Shortcuts: /explain  /fix  /test  /doc  /review]"
QUEUED_LINE = '[AI] queued "second" - it starts when the running one ends'

TRACE_POLL_S = 10
TURN_POLL_S = 20

# The driver scales its own key/drain waits by XWPE_TEST_WAIT_SCALE (the
# sanitizer build runs at 2), but a fixed XWPE_AI_MOCK_DELAY_MS does not: the
# mock's reply lands at the same wall-clock time whatever the factor.  Every
# delay below is scaled by hand so a key sent "during the turn" is still
# during the turn under the slower binary.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
SLOW_MS = int(4500 * WAIT_SCALE)
QUEUE_MS = int(2500 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path, reply, **more):
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_POLICY": "ask",
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    env.update(more)
    return env


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _seen_within(s, needle, timeout):
    """Drain the screen until `needle` shows or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _traced_within(tmp_path, needle, timeout):
    """Wait until the trace holds `needle` or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return False


def _row_index(rows, needle):
    return next((i for i, r in enumerate(rows) if needle in r), -1)


def _ask(s, question, delay=0.5):
    s.key(question); s.key("\r", delay=delay)


# @scenario SC-ai-chat-21
def test_esc_cancels_a_streaming_chat_turn(tmp_path):
    env = _env(tmp_path, "NEVERSHOWN", XWPE_AI_MOCK_DELAY_MS=str(SLOW_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        _ask(s, "hi")
        assert _seen_within(s, GATHERING, 3), _screen(s)
        s.key(ESC, delay=0.8)
        s._drain(0.5)
        after_esc = s.display()
        s._drain(SLOW_MS / 1000.0 + 1.5)          # the mock's reply would land now
        s.key("again"); s._drain(0.4)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    assert CANCELLED_LINE in "\n".join(after_esc), \
        "Esc did not cancel the chat turn:\n" + "\n".join(after_esc)
    assert "NEVERSHOWN" not in text, "the cancelled reply was painted:\n" + text
    assert any("> again" in r for r in rows), \
        "the input row was not armed after the cancel:\n" + text
    assert "chat cancelled" in txt, "the trace does not record the cancel:\n" + txt


# @scenario SC-ai-chat-29
def test_second_question_is_queued_and_runs_after_the_turn(tmp_path):
    env = _env(tmp_path, "REPLYONE@@TURN@@REPLYTWO", XWPE_AI_MOCK_DELAY_MS=str(QUEUE_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        _ask(s, "first")
        assert _seen_within(s, GATHERING, 3), _screen(s)
        _ask(s, "second")
        assert _seen_within(s, QUEUED_LINE, 3), \
            "the pane did not say the second question was queued:\n" + _screen(s)
        assert _seen_within(s, "REPLYTWO", TURN_POLL_S + 2 * QUEUE_MS / 1000.0), _screen(s)
        s._drain(0.5)
        rows = s.display()
    txt = _trace(tmp_path)
    text = "\n".join(rows)
    one = _row_index(rows, "AI: REPLYONE")
    you2 = _row_index(rows, "You: second")
    two = _row_index(rows, "AI: REPLYTWO")
    assert one >= 0, "the first reply never completed on screen:\n" + text
    assert you2 > one, "the second question did not follow the first reply:\n" + text
    assert two > you2, "the second reply did not follow its question:\n" + text
    assert CANCELLED_LINE not in text, "the second question cut the running turn:\n" + text
    assert txt.find("chat prompt=first") < txt.find("chat prompt=second"), txt


# @scenario SC-ai-chat-12
def test_waiting_line_spins_and_counts_seconds(tmp_path):
    env = _env(tmp_path, "late", XWPE_AI_MOCK_DELAY_MS=str(SLOW_MS))
    samples = []
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        _ask(s, "wait", delay=0.3)
        end = time.time() + 4.0 * WAIT_SCALE
        while time.time() < end:
            s._drain(0.3)
            line = next((r for r in s.display(drain=0) if GATHERING in r), None)
            if line:
                samples.append(line.strip())
        s._drain(1.5)
    uniq = sorted(set(samples))
    assert samples, "no waiting line was ever on screen"
    assert any("(Esc cancels)" in x for x in samples), uniq
    assert len(uniq) >= 3, "the waiting line did not change over 4 s: %r" % uniq
    secs = sorted({int(m.group(1)) for x in samples for m in [re.search(r"(\d+)s", x)] if m})
    assert len(secs) >= 2, "the seconds counter did not advance: %r" % secs


# @scenario SC-ai-chat-10
def test_header_and_hints_appear_after_an_edit_opened_the_pane(tmp_path):
    env = _env(tmp_path, "int main(void){return 1;}\n", XWPE_AI_MOCK_DELAY_MS=str(SLOW_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("e", delay=0.6)
        s.key("do it"); s.key("\r", delay=0.8)        # the Edit's line opens the pane
        assert _seen_within(s, "[AI edit] working", 3), _screen(s)
        s.key(ESC, delay=1.0)                          # cancel: the chat is armed
        s._drain(0.5)
        rows = s.display()
    text = "\n".join(rows)
    cancelled = _row_index(rows, CANCELLED_LINE)
    header = _row_index(rows, HEADER_ASK)
    assert cancelled >= 0, text
    assert header > cancelled, "no header after the pane was opened by an Edit:\n" + text
    assert _row_index(rows, HINT_KEYS) == header + 1, "no key-hint line:\n" + text
    assert _row_index(rows, HINT_SLASH) == header + 2, "no slash-hint line:\n" + text


# @scenario SC-ai-chat-10b
def test_header_follows_the_dial(tmp_path):
    env = _env(tmp_path, "one@@TURN@@two")
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        _ask(s, "first", delay=1.0)
        assert _traced_within(tmp_path, "chat done", TRACE_POLL_S), _trace(tmp_path)
        s.key(ALT.AI); s.key("y", delay=0.6)           # ask -> edits
        assert _traced_within(tmp_path, "policy set edits", TRACE_POLL_S), _trace(tmp_path)
        _ask(s, "second", delay=1.0)
        assert _seen_within(s, "AI: two", TRACE_POLL_S), _screen(s)
        rows = s.display()
    text = "\n".join(rows)
    first_hdr = _row_index(rows, HEADER_ASK)
    you1 = _row_index(rows, "You: first")
    new_hdr = _row_index(rows, HEADER_EDITS)
    you2 = _row_index(rows, "You: second")
    assert first_hdr >= 0 and you1 > first_hdr, "the first header is gone:\n" + text
    assert new_hdr > you1, "no new header after the dial changed:\n" + text
    assert you2 == new_hdr + 1, "the new header does not precede the second turn:\n" + text


# @scenario SC-ai-chat-08
def test_closing_the_pane_ends_the_stream(tmp_path):
    env = _env(tmp_path, "LATEREPLY", XWPE_AI_MOCK_DELAY_MS=str(SLOW_MS))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        s.key(ALT.AI); s.key("a", delay=0.6)
        _ask(s, "hi")
        assert _seen_within(s, GATHERING, 3), _screen(s)
        s.key(CTRL_W, delay=0.8)
        assert _traced_within(tmp_path, "chat target gone", 3), \
            "closing the pane did not end the stream:\n" + _trace(tmp_path)
        s._drain(SLOW_MS / 1000.0 + 1.5)          # the mock's reply would land now
        rows = s.display()
    text = "\n".join(rows)
    assert "NOT saved" not in text, "a save question was asked for the pane:\n" + text
    assert "LATEREPLY" not in text, "a token re-created the pane:\n" + text
    assert "You: hi" not in text and GATHERING not in text, \
        "the pane is still on screen after Ctrl-W:\n" + text
    assert "t.c" in rows[1], "the file window is gone:\n" + text


# @scenario SC-ai-chat-41
def test_transport_error_replaces_the_waiting_line(tmp_path):
    captured = []
    srv, port = _fault_server(captured, ["never"], fault_for_turn=lambda n: "close")
    try:
        _seed_openai_xwperc(tmp_path, port, "ask")
        with WpeSession(str(tmp_path), SEED, env_extra=_openai_env(tmp_path)) as s:
            s.key(ALT.AI, delay=0.6); s.key("a", delay=0.8)
            _ask(s, "hi", delay=1.0)
            assert _seen_within(s, TRANSPORT_LINE, TRACE_POLL_S), _screen(s)
            s._drain(0.5)
            rows = s.display()
    finally:
        srv.shutdown()
        srv.server_close()
    text = "\n".join(rows)
    assert captured, "the chat never reached the server"
    assert GATHERING not in text, "the waiting line is still standing:\n" + text
    assert _row_index(rows, TRANSPORT_LINE) == _row_index(rows, "You: hi") + 1, \
        "the transport line is not where the waiting line was:\n" + text
