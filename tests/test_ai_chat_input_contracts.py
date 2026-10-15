"""The chat input row: what the pane does with the keys it owns.

Three behaviours of the chat input row that the rest of the suite never
exercises, each pinned to the code that implements it:

* SC-ai-chat-05  a second Alt-G a while the pane is already open only
  refocuses it: the pane keeps exactly the lines it had -- no new header
  line, no new "You:" line -- and the trace shows the one stream the
  first send started, not a second one.  (we_ai_ui.c: e_ai_chat,
  "already open: just refocus it".)
* SC-ai-chat-19  Enter on an empty input row sends nothing: no
  "chat prompt=" and no "stream start" in the trace, the transcript
  untouched, the "> " row still armed.  (we_ai_ui.c: e_ai_chat_key,
  "if (g_ai_input_len > 0)".)
* SC-ai-chat-23  Backspace with the caret at the start of the row is a
  no-op: the row and the transcript are unchanged, and the editor is
  still taking keys.  (we_ai_ui.c: ai_in_backspace, "if (g_ai_input_pos
  <= 0) return".)

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


def _seen_within(s, needle, timeout):
    """Drain the screen until a row holds `needle` or `timeout` passes."""
    end = time.time() + timeout
    while time.time() < end:
        s._drain(0.25)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _arm_chat(s):
    """Open the chat pane straight into its input row (Alt-G, a)."""
    s.key(ALT.AI)
    s.key("a", delay=0.6)


# @scenario SC-ai-chat-05
def test_second_alt_g_a_refocuses_without_appending(tmp_path):
    """With the pane already open, Alt-G a refocuses it.

    The pane keeps exactly the lines it had: no second header line, no
    new "You:" line, and the trace shows the one stream the first send
    started -- not a second one.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        s.key("hi")
        s.key("\r")
        assert _seen_within(s, REPLY, 10), _screen(s)
        s._drain(0.5)

        # The pane is already open: this must only refocus it.  Refocus may
        # move the cursor glyph, so compare the transcript line counts, not
        # the raw screen: the same question, the same reply, one header, one
        # input row -- nothing appended.
        def _counts():
            rows = s.display()
            return {
                "you":    sum(1 for r in rows if "You:" in r),
                "header": sum(1 for r in rows if "[AI: mock" in r),
                "reply":  sum(1 for r in rows if REPLY in r),
                "input":  sum(1 for r in rows if r.lstrip("x").startswith(">")),
            }

        before = _counts()

        s.key(ALT.AI)
        s.key("a", delay=0.6)
        s._drain(0.5)

        after = _counts()
        assert after == before, \
            "refocus changed the transcript:\n--- before ---\n%s\n--- after ---\n%s" % (before, after)
    assert _trace(tmp_path).count("stream start") == 1, \
        "a second stream started:\n" + _trace(tmp_path)


# @scenario SC-ai-chat-19
def test_enter_on_empty_input_row_sends_nothing(tmp_path):
    """Enter with an empty input row is a no-op.

    No prompt is sent, so the trace has neither a "chat prompt=" line nor
    a "stream start", and the pane still shows its empty "> " row with
    the transcript exactly as it was.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        assert _rows_with(s, "> "), "no input row on screen:\n" + _screen(s)

        before = _screen(s)
        s.key("\r")
        s._drain(0.6)

        after = _screen(s)
        assert after == before, \
            "empty Enter changed the pane:\n--- before ---\n%s\n--- after ---\n%s" % (before, after)
        assert _rows_with(s, "> "), "the input row disappeared after an empty Enter"
    txt = _trace(tmp_path)
    assert "chat prompt=" not in txt, "an empty row was sent as a prompt:\n" + txt
    assert "stream start" not in txt, "an empty row started a stream:\n" + txt


# @scenario SC-ai-chat-23
def test_backspace_at_start_of_row_is_a_noop(tmp_path):
    """Backspace with the caret at the start of the row deletes nothing.

    Type "abc", move the caret to the start (Ctrl-A), press Backspace: the
    row still reads "abc" (there is no character before the caret, so
    nothing is deleted) and the transcript is untouched.  The editor is
    still taking keys: the next character lands in the row.
    """
    with WpeSession(str(tmp_path), SEED, env_extra=_env(tmp_path)) as s:
        _arm_chat(s)
        s.key("a")
        s.key("b")
        s.key("c")
        assert _rows_with(s, "abc"), "typed text not on screen:\n" + _screen(s)

        s.key("\x01")          # Ctrl-A: caret to the start of the line
        before = _screen(s)
        s.key("\x7f")          # Backspace at the start: must be a no-op
        s._drain(0.3)
        after = _screen(s)
        assert after == before, \
            "backspace at the start changed the pane:\n--- before ---\n%s\n--- after ---\n%s" % (before, after)
        assert _rows_with(s, "abc"), "the row lost a character:\n" + _screen(s)

        # The editor is still taking keys: the next character lands in the row.
        s.key("X")
        s._drain(0.3)
        assert _rows_with(s, "Xabc"), "the editor stopped taking keys:\n" + _screen(s)
