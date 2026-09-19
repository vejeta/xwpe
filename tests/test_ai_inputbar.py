"""AI chat -- the fixed input row at the bottom of the pane (Cursor/Copilot-style).

Alt-G a opens the Ask popup; sending drops into a focused chat where the pane
keeps a "> " input row pinned at the bottom and the reply streams in ABOVE it.
Follow-ups are typed straight into the row -- no re-opening the popup -- and the
whole conversation accumulates in the transcript.  Esc leaves the chat.
"""
import os
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


def _rows_with(disp, needle):
    return [i for i, ln in enumerate(disp) if needle in ln]


def test_fixed_input_row_and_followups(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "FIRSTREPLY@@TURN@@SECONDREPLY",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("a")                       # popup
        s.key("first question")
        s.key("\r", delay=1.5)           # send -> enter the focused chat
        s._drain(1.2)
        d1 = s.display()

        # A "> " input row exists, at or below the reply.
        assert _rows_with(d1, "> "), "no input row shown:\n" + "\n".join(d1)
        assert _rows_with(d1, "FIRSTREPLY"), "first reply missing:\n" + "\n".join(d1)
        reply_row = _rows_with(d1, "FIRSTREPLY")[0]
        input_row = max(_rows_with(d1, "> "))
        assert input_row > reply_row, "input row is not below the reply"

        # Type a follow-up straight into the row (no popup) and send it.
        s.key("second question")
        s._drain(0.4)
        d_typing = s.display()
        assert any("second question" in ln for ln in d_typing), \
            "typing did not land in the input row:\n" + "\n".join(d_typing)

        s.key("\r", delay=1.5)
        s._drain(1.2)
        d2 = s.display()
        s.key("\033", delay=0.4)         # Esc leaves the chat

    joined = "\n".join(d2)
    # Both exchanges accumulated, input row still pinned at the bottom.
    assert "FIRSTREPLY" in joined and "SECONDREPLY" in joined, \
        "conversation did not accumulate:\n" + joined
    second_reply = _rows_with(d2, "SECONDREPLY")[0]
    input_row2 = max(_rows_with(d2, "> "))
    assert input_row2 > second_reply, "input row drifted above the last reply"

    txt = trace.read_text() if trace.exists() else ""
    prompts = [l for l in txt.splitlines() if l.startswith("chat prompt=")]
    assert any("first question" in l for l in prompts), "first turn not sent:\n" + txt
    assert any("second question" in l for l in prompts), "follow-up not sent:\n" + txt


def test_input_row_cursor_editing(tmp_path):
    # The input row is a small line editor: type "abc", move the caret left twice,
    # insert "X" -> the sent prompt is "aXbc" (inserted at the caret, not the end).
    # Also proves the entry hint shows and arrow keys reach the row.
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
        s._drain(0.5)
        hint = s.display()
        assert any("Enter=send" in ln for ln in hint), \
            "the entry hint was not shown:\n" + "\n".join(hint)
        s.key("abc")
        s.key("\033[D")               # Left
        s.key("\033[D")               # Left  -> caret is a|bc
        s.key("X")                    # insert at the caret -> aXbc
        s._drain(0.3)
        s.key("\r", delay=1.2)
        s._drain(0.6)
        s.key("\033", delay=0.4)
    txt = trace.read_text() if trace.exists() else ""
    prompts = [l for l in txt.splitlines() if l.startswith("chat prompt=")]
    assert any("aXbc" in l for l in prompts), \
        "the caret did not insert mid-line (arrow keys not reaching the row):\n" + txt
