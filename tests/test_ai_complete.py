"""Inline completion (Alt-G c): the model's suggestion is inserted at the cursor.

A low-ceremony alternative to chat/edit for "finish this": xwpe sends the code
around the cursor to the model and splices the reply in at the cursor through the
proven whole-file apply path, so it is one Ctrl-U undo step.  With the cursor at
the start of the buffer, the mock suggestion is inserted there.
"""
import os
import time
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


def test_complete_inserts_at_cursor(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "PREFIX", "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    with WpeSession(str(tmp_path), "ZZZ\n", env_extra=env, filename="t.c") as s:
        s.key(ALT.AI, delay=0.6); s.key("c", delay=0.8)   # Complete at cursor (0,0)
        end = time.time() + 12
        while time.time() < end:
            s._drain(0.5)
            if "PREFIX" in "\n".join(s.display()):
                break
        disp = "\n".join(s.display())
        s.save()
    assert "PREFIX" in disp, "the completion was not inserted on screen:\n" + disp
    with open(os.path.join(str(tmp_path), "t.c")) as fh:
        text = fh.read()
    # the suggestion is spliced in AT the cursor (start of buffer), before ZZZ
    assert text.startswith("PREFIX") and "ZZZ" in text, \
        "the completion did not land at the cursor -- disk=%r" % text
