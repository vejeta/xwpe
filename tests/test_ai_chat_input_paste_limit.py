"""The chat input row under a paste and at its size limit.

Two contracts of the input row that the rest of the suite never exercises,
each pinned to the code that implements it:

* SC-ai-chat-27  text pasted under bracketed paste (ESC[200~ ... ESC[201~)
  enters the row literally: every pasted line break is a line break in the
  input, a CR LF pair is one line break, and nothing is sent until Enter.
  (we_ai_ui.c: e_ai_chat_key, ai_in_paste_break; we_term.c: e_t_getch,
  e_t_paste_swallows_lf.)
* SC-ai-chat-26  a key that would take the input past AI_PROMPT_MAX is
  refused and the status row says so; a paste that crosses the limit keeps
  what fits and the same message is shown.  (we_ai_ui.c: ai_in_insert,
  ai_in_refuse_at_limit.)

The mock backend makes every reply deterministic, so the tests assert on
the pane's screen text and on the XWPE_AI_TRACE log, never on a live
provider.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import ALT, WPE_BIN, WpeSession

SEED = "int main(void){return 0;}\n"
REPLY = "REPLYONE"
PASTE_BEG = "\033[200~"
PASTE_END = "\033[201~"
PROMPT_MAX = 32768                      # AI_PROMPT_MAX in we_ai_ui.c
LIMIT_WORDS = "Input at its limit (%d bytes)" % PROMPT_MAX


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


def _screen(s, drain=0.2):
    return "\n".join(s.display(drain=drain))


def _rows_with(s, needle):
    return [r for r in s.display() if needle in r]


def _interior(row):
    """A pane row without its two border columns, trailing blanks dropped."""
    return row[1:-1].rstrip()


def _row_index(rows, needle):
    return next((i for i, r in enumerate(rows) if needle in r), -1)


def _traced_within(tmp_path, needle, timeout):
    """Wait until the trace holds `needle` or `timeout` seconds pass."""
    end = time.time() + timeout
    while time.time() < end:
        if needle in _trace(tmp_path):
            return True
        time.sleep(0.2)
    return False


def _sent_prompts(tmp_path):
    """The prompts the chat sent, in order, each with its continuation lines."""
    out = []
    for line in _trace(tmp_path).splitlines():
        if line.startswith("chat prompt="):
            out.append(line[len("chat prompt="):])
        elif out and not _is_trace_event(line):
            out[-1] += "\n" + line
    return out


def _is_trace_event(line):
    """A trace event starts with a known word; a prompt's extra lines do not."""
    heads = ("chat ", "stream ", "agent ", "session ", "project ", "menu ",
             "policy ", "ai ", "tool ", "mock ")
    return line.startswith(heads)


def _arm_chat(s):
    """Open the chat pane straight into its input row (Alt-G, a)."""
    s.key(ALT.AI)
    s.key("a", delay=0.6)


def _type_bytes(s, count):
    """Type `count` ASCII bytes into the row in chunks the pty keeps up with."""
    chunk = "a" * 100
    whole, rest = divmod(count, len(chunk))
    for _ in range(whole):
        s.key(chunk, delay=0.15)
    if rest:
        s.key("a" * rest, delay=0.15)
    s._drain(1.0)


# @scenario SC-ai-chat-27
def test_bracketed_paste_with_newlines_is_literal(tmp_path):
    """A multi-line paste lands in the row as typed; nothing is sent.

    "first line\\nsecond line\\r\\nthird line" between the paste markers
    gives a three-line input: the LF is a line break, the CR LF pair is one
    line break, and the trace has no "chat prompt=" until Enter.  Enter
    then sends the three lines as one prompt.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        s.key(PASTE_BEG + "first line\nsecond line\r\nthird line" + PASTE_END, delay=0.8)
        s._drain(0.5)
        rows = [_interior(r) for r in s.display()]
        before_enter = _trace(tmp_path)
        y = _row_index(rows, "> first line")

        s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "chat done", 10), _trace(tmp_path)
        after = _screen(s)
    assert "chat prompt=" not in before_enter, \
        "the paste sent something before Enter:\n" + before_enter
    assert y >= 0, "the pasted first line is not in the input row:\n" + "\n".join(rows)
    assert rows[y + 1] == "  second line", \
        "the pasted LF is not a line break in the input: %r" % rows[y + 1]
    assert rows[y + 2] == "  third line", \
        "the pasted CR LF is not one line break in the input: %r" % rows[y + 2]
    sent = _sent_prompts(tmp_path)
    assert sent == ["first line\nsecond line\nthird line"], \
        "Enter did not send the three pasted lines as one prompt: %r" % sent
    assert "You: first line" in after, after


# @scenario SC-ai-chat-26
def test_input_at_the_limit_is_announced(tmp_path):
    """The key that would pass the limit is refused and the status row says so.

    With the row full, one more character does not appear in the pane and
    the bottom row reads "Input at its limit (32768 bytes)".  The next key
    restores the key hints, and Enter sends what fitted.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        _type_bytes(s, PROMPT_MAX)
        s.key("Z", delay=0.4)                   # one more: must be refused
        s._drain(0.5)
        rows = s.display()
        status = rows[-1]

        s.key("\r", delay=1.0)
        assert _traced_within(tmp_path, "chat done", 10), _trace(tmp_path)
        after = s.display()
    assert "Z" not in "".join(_interior(r) for r in rows), "the key past the limit was taken"
    assert LIMIT_WORDS in status, \
        "the status row does not say the input is at its limit: %r" % status
    assert "F1 Help" in after[-1], "the next key did not restore the key hints: %r" % after[-1]
    sent = _sent_prompts(tmp_path)
    assert len(sent) == 1 and set(sent[0]) == {"a"}, "Enter did not send the full row: %r" % sent


# @scenario SC-ai-chat-26
def test_paste_crossing_the_limit_keeps_what_fits(tmp_path):
    """A paste longer than the row keeps the part that fits and says so.

    The bytes that fit are in the row, the ones past the limit are not,
    and the status row carries the limit message once the paste is over.
    """
    marker = "TAIL"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        _type_bytes(s, PROMPT_MAX - 10)
        s.key(PASTE_BEG + "b" * 20 + marker + PASTE_END, delay=0.8)
        s._drain(0.5)
        rows = s.display()
        status = rows[-1]
        pane = "".join(_interior(r) for r in rows)
    assert marker not in pane, "bytes past the limit were taken from the paste"
    assert LIMIT_WORDS in status, \
        "the status row does not say the paste hit the limit: %r" % status
