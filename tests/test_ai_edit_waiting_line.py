"""An Edit's waiting line ends with the Edit, replaced by its result line.

While the model works, the AI pane shows "[AI edit] working - 5s (Esc
cancels)" and the status row says "The assistant is working on this window".
When the review ends, whether every hunk is kept with `a` or the review is
left with Esc, the pane's result line ("[AI edit] applied 2 of 2 hunks -
Ctrl-U to undo", "[AI edit] discarded") takes the waiting line's place: no
line still says "working", frozen at the seconds it had when the review
opened, and the status row's sentence is gone at the same moment.

The contract is the one the chat keeps for its own waiting line
(SC-ai-chat-12: the answer replaces "AI: (gathering the answer...)"), applied
to the Edit.  The mock backend answers with a small delay so the waiting
line is really on screen, spinning, before the review opens.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, pane_text_with_history

ESC = "\033"
ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
NEW = "int a = 1;\nint b = 22;\nint c = 3;\nint d = 44;\nint e = 5;\n"
REVIEW_BOX = "Proposed change"
WAITING = "working"
APPLIED = "applied"
DISCARDED = "discarded"
LOCKED_ROW = "working on this window"

# The mock's delay is wall-clock: scale it like the driver scales its own
# waits, so the waiting line has ticked before the review opens under the
# slower sanitizer build as well.
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
REPLY_DELAY_MS = int(1500 * WAIT_SCALE)
REVIEW_POLL_S = 12
RESULT_POLL_S = 10


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _env(tmp_path):
    return {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
            "XWPE_AI_MOCK_REPLY": NEW,
            "XWPE_AI_MOCK_DELAY_MS": str(REPLY_DELAY_MS),
            "XWPE_AI_TRACE": str(tmp_path / "ai.trace")}


def _trace(tmp_path):
    t = tmp_path / "ai.trace"
    return t.read_text() if t.exists() else ""


def _wait_screen(s, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in "\n".join(s.display(drain=0)):
            return True
    return False


def _wait_trace(s, tmp_path, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.4)
        if needle in _trace(tmp_path):
            return True
    return False


def _start_edit(s, tmp_path):
    """Ask for the Edit, see the waiting line spin, wait for the review box."""
    s.key(ALT.AI); s.key("e"); s._drain(0.4)
    s.key("change b and d"); s.key("\r", delay=0.6)
    assert _wait_screen(s, "[AI edit] " + WAITING, REVIEW_POLL_S), \
        "the waiting line never showed:\n" + "\n".join(s.display(drain=0))
    assert _wait_screen(s, REVIEW_BOX, REVIEW_POLL_S), \
        "no review box:\n" + "\n".join(s.display(drain=0))


def _lines_without(lines, word):
    return [l for l in lines if word in l]


def _check_pane_after(s, result_word, pane_lines):
    """The result line is there, no waiting line stands, the status row is free."""
    text = "\n".join(pane_lines)
    assert any(result_word in l for l in pane_lines), \
        "the pane has no '%s' line:\n%s" % (result_word, text)
    standing = _lines_without(pane_lines, WAITING)
    assert standing == [], \
        "the waiting line is still standing above the result: %r\n%s" % (standing, text)
    bottom = s.display(drain=0)[-1]
    assert LOCKED_ROW not in bottom, \
        "the status row still says the assistant is working: %r" % bottom


# @scenario SC-ai-edit-12
def test_accepted_edit_replaces_the_waiting_line_with_the_applied_line(tmp_path):
    """Keeping every hunk with `a`: "applied" stands where "working" stood."""
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path), filename="t.c") as s:
        _start_edit(s, tmp_path)
        s.key("a", delay=0.7)
        assert _wait_trace(s, tmp_path, "edit applied", RESULT_POLL_S), \
            "the accepted edit was not applied:\n" + _trace(tmp_path)
        s._drain(0.6)
        pane = pane_text_with_history(s, [APPLIED])
        _check_pane_after(s, APPLIED, pane)


# @scenario SC-ai-edit-12
def test_cancelled_edit_replaces_the_waiting_line_with_the_discarded_line(tmp_path):
    """Leaving the review with Esc: "discarded" stands where "working" stood."""
    with WpeSession(str(tmp_path), ORIG, env_extra=_env(tmp_path), filename="t.c") as s:
        _start_edit(s, tmp_path)
        s.key(ESC, delay=0.7)
        assert _wait_trace(s, tmp_path, "edit discarded", RESULT_POLL_S), \
            "the cancelled edit was not reported:\n" + _trace(tmp_path)
        s._drain(0.6)
        pane = pane_text_with_history(s, [DISCARDED])
        _check_pane_after(s, DISCARDED, pane)
