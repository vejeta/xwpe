"""AI assistant -- Edit runs in the BACKGROUND (needs a real, slow-ish model).

Proves the anti-clunky contract: while the model generates an Edit reply the
editor is NOT blocked and the edit applies.  Self-skips unless the chosen provider is
usable (Ollama reachable by default).

More time for a slow model:  XWPE_TEST_WAIT_SCALE=3 (multiplies every wait).
Against Claude instead of Ollama:
  XWPE_LIVE_AI=claude ANTHROPIC_API_KEY=... CLAUDE_MODEL=... WPE_BIN=$PWD/wpe tests/.venv/bin/python -m pytest -q tests/test_ai_async_live.py
Provider choice and budgets live in tests/live_ai.py.
"""
import os
import subprocess
import pytest
import live_ai
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"(async)" in out and b"Alt-G AI" in out
    except Exception:
        return False


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    live_ai.SKIP_MARK,
]


REVIEW_MARK = "Proposed change"      # title of the hunk review box
BUSY_MARK = "working on this window"  # status row while the edit runs


def _review_up(disp):
    return REVIEW_MARK in disp


def test_ai_edit_is_background(tmp_path):
    """A real edit streams while the editor stays usable, and applies.

    A live model's speed is not under the test's control (a fast one has the
    review box up before the first look), so no timing is asserted here: the
    spinner advancing, the refusal of typing into the waiting window and Esc
    cancelling are proved deterministically by tests/test_ai_async.py with a
    delayed mock.  What only a real provider can show is that the edit
    completes with the editor responsive: while the operation runs a typed key
    is answered on the status row (or the review box is already up), the
    provider returns no error, and after accepting, the trace says the edit
    was applied.
    """
    trace = tmp_path / "ai.trace"
    env = live_ai.session_env(trace)
    with WpeSession(str(tmp_path), "AAA\n", filename="stack.c",
                    env_extra=env) as s:
        s.key(ALT.AI); s.key("e", delay=0.8)
        s.key("append a trailing comment line")
        s.key("\r", delay=1.0)                 # async edit starts
        disp = "\n".join(s.display())
        if not _review_up(disp):
            s.key("Z"); s.key("Z"); s.key("Z")
            s._drain(1.0)
            disp = "\n".join(s.display())
            assert _review_up(disp) or BUSY_MARK in disp, \
                "the editor neither answered the key nor showed the review:\n" + disp
        assert not live_ai.has_backend_error(disp, trace), \
            "the provider refused the request:\n" + disp
        # The window refuses keys until the review box is up, so "a" typed
        # earlier would be lost: wait for the review before accepting.
        assert live_ai.wait_for_trace(trace, "edit review", s, 180), \
            "the review never opened:\n" + (trace.read_text() if trace.exists() else "")
        assert not live_ai.has_backend_error("\n".join(s.display()), trace), \
            "the provider refused the request:\n" + "\n".join(s.display())
        s.key("a", delay=1.0)                  # accept all, now the box is up
        assert live_ai.wait_for_trace(trace, "edit applied", s, 180), \
            "edit never applied:\n" + (trace.read_text() if trace.exists() else "")
