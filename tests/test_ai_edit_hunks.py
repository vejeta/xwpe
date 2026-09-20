"""Structured range edits: each diff hunk is an independent, accept/reject range.

The model's reply changes two separated lines (line 1 and line 5), which the diff
engine splits into two hunks.  Rejecting the first and accepting the second must
apply ONLY the second range: line 1 keeps its original text, line 5 changes, and
everything between is untouched.  This is the range-granular guarantee -- an Edit
is a set of independent (old-range -> new-lines) edits, applied selectively under
one undo, not an all-or-nothing whole-file swap.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
NEW = "int a = 11;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _wait(s, needle, tmo=12):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in "\n".join(s.display()):
            return True
    return False


def test_reject_one_hunk_accept_the_other(tmp_path):
    trace = tmp_path / "e.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": NEW,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        s.key("bump a and e"); s.key("\r", delay=0.8)
        assert _wait(s, "Proposed change 1/2"), "first hunk not shown"
        s.key("n", delay=0.6)                          # REJECT hunk 1 (line a)
        assert _wait(s, "Proposed change 2/2"), "second hunk not shown"
        s.key("y", delay=0.6); s._drain(0.8)           # ACCEPT hunk 2 (line e)
        s.save()
        out = s.text()
    txt = trace.read_text()
    assert "edit applied 1/2" in txt, "should apply exactly one of two hunks:\n" + txt
    # accepted range applied ...
    assert "int e = 55;" in out and "int e = 5;" not in out, \
        "accepted hunk (line e) not applied:\n" + out
    # ... rejected range left verbatim, and the untouched middle preserved
    assert "int a = 1;" in out and "int a = 11;" not in out, \
        "rejected hunk (line a) was applied anyway:\n" + out
    for keep in ("int b = 2;", "int c = 3;", "int d = 4;"):
        assert keep in out, "an untouched line was disturbed (%r):\n%s" % (keep, out)
