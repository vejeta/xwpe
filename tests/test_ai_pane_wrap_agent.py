"""Pane output wraps to a readable measure, not the full width of a wide pane.

The chat reply already word-wraps as it streams; the agent/multi-file answer used
a different append path.  Both now share one wrap width, and that width is capped
at a readable prose measure (~80 columns) even when the pane is much wider -- long
unbroken lines of prose are hard to read, so the pane caps the measure the way
editors and chat UIs do rather than filling the whole window.

Driven in a WIDE (120-column) terminal: the answer must wrap well before the pane
edge.  Red/green: without the cap the words run to ~col 114 (the wide pane width);
with it they stop by ~col 80.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

MARK = "zebraend"
REPLY = "DONE\n" + ("alpha bravo charlie delta echo " * 16) + MARK
WORDS = ["alpha", "bravo", "charlie", "delta", "echo", MARK]


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_long_answer_wraps_to_readable_measure(tmp_path):
    trace = tmp_path / "a.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": REPLY,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c", cols=120, rows=32) as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("tell me a lot"); s.key("\r", delay=1.0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.5)
            if "agent done" in (trace.read_text() if trace.exists() else ""):
                break
        rows = s.display()
    ans_rows = [r for r in rows if "alpha" in r or MARK in r]
    assert ans_rows, "answer not found on screen:\n" + "\n".join(rows)
    for r in ans_rows:
        rl = r.lower()
        ends = [rl.rfind(w) + len(w) for w in WORDS if w in rl]
        assert ends and max(ends) <= 88, \
            "answer text reached column %d in a 120-col pane -- not wrapped to a " \
            "readable measure:\n%s" % (max(ends), r)
    assert MARK in "\n".join(rows).lower(), "the answer's tail never rendered"
