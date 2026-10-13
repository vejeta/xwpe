"""An agent reply that lands while a dialog is open waits for the dialog to
close, exactly as the Edit kind does (see test_ai_edit_reply_modal.py).

Alt-G g starts an agent run; its tool loop is driven from the fd-loop, which
is pumped from inside the key read of every dialog and prompt.  A reply that
arrives while the Alt-G g task prompt is open for a second, unrelated request
is held rather than acted on there: the tool it calls for ("list_dir") must
not run behind the modal prompt, where its result could be mistaken for
something the prompt itself produced.  The hold is generic across every AI
op kind (ai_op_hold_reply / ai_op_deliver_held), so the agent's own trace
and pane lines read "agent reply held (dialog open)" / "agent reply
delivered" where the Edit kind reads "edit reply held" / nothing (Edit
delivers straight into its own review).

This test starts an agent run against the mock backend with a slow first
turn, opens the task prompt before that reply lands, and asserts that the
tool call is held behind the prompt and only runs once the prompt closes.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
PROMPT = "AI agent task"
HELD_LINE = "[AI agent] reply ready - shown when the dialog closes"

# The driver scales its own key/drain waits by XWPE_TEST_WAIT_SCALE (the
# sanitizer build runs at 2, the Debian autopkgtest at 3); a fixed
# XWPE_AI_MOCK_DELAY_MS does not, since the mock's reply lands at the same
# wall-clock time regardless of that factor, so the delay itself is scaled by
# hand (as tests/test_ai_queue.py and tests/test_ai_pending_lock.py do).
WAIT_SCALE = float(os.environ.get("XWPE_TEST_WAIT_SCALE", "1"))
MOCK_DELAY_MS = int(3000 * WAIT_SCALE)


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _trace(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(s, path, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.3)
        if needle in _trace(path):
            return True
    return False


def _on_screen(s, needle):
    return any(needle in r for r in s.display())


def _screen(s):
    return "\n".join(s.display())


def _env(trace):
    return {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "TOOL list_dir .@@TURN@@DONE listed",
        "XWPE_AI_MOCK_DELAY_MS": str(MOCK_DELAY_MS),
        "XWPE_AI_TRACE": str(trace),
    }


def _start_agent(s, task, trace):
    s.key(ALT.AI); s.key("g")
    s.key(task); s.key("\r", delay=1.0)
    assert _wait_trace(s, trace, "agent task=", 8), \
        "the agent run never started:\n" + _trace(trace)


def _outlive_the_reply(s):
    """Wait until the mock has certainly written its (held) reply."""
    s._drain(MOCK_DELAY_MS / 1000.0 + 1.0)


# @scenario SC-ai-agent-43
def test_reply_behind_task_prompt_waits_for_it(tmp_path):
    trace = tmp_path / "agent.trace"
    with WpeSession(str(tmp_path), SEED, env_extra=_env(trace), filename="t.c") as s:
        _start_agent(s, "list the directory then finish", trace)

        # Open the Alt-G g task prompt before the first (slow) reply lands,
        # as a second, unrelated request would; the reply then arrives while
        # the editor is not idle.
        s.key(ALT.AI, delay=0.3); s.key("g", delay=0.3)
        assert _on_screen(s, PROMPT), "the task prompt did not open:\n" + _screen(s)
        _outlive_the_reply(s)

        txt = _trace(trace)
        assert "agent reply held (dialog open)" in txt, \
            "the reply was not held behind the prompt:\n" + txt
        assert "agent tool=list_dir" not in txt, \
            "the tool ran while the prompt was still open:\n" + txt
        assert _on_screen(s, PROMPT), "the task prompt went away:\n" + _screen(s)

        # Close the prompt (Esc, empty goal -> nothing new is started): the
        # held reply is delivered and the tool call it named now runs.
        s.key("\033", delay=0.8)
        assert _wait_trace(s, trace, "agent tool=list_dir arg=.", 8), \
            "the tool did not run once the prompt closed:\n" + _trace(trace)
        txt = _trace(trace)
        assert "agent reply delivered" in txt, txt
        assert not _on_screen(s, PROMPT), "the prompt is still up:\n" + _screen(s)
        assert _on_screen(s, HELD_LINE), \
            "the pane does not show the held-reply line once the prompt closed:\n" + _screen(s)
        assert s.alive(), "wpe died after the deferred reply"
