"""AI Edit scopes to the marked block: only the selection is sent and changed.

Mark lines 2-3 (Begin Mark, move down, End Mark), then Alt-G e.  The mock backend
replies with the modified REGION only (two lines) -- the way a real model answers a
selection edit.  The change must be spliced back into just those lines: the rest of
the file (lines 1, 4, 5) must survive verbatim.

This is the red/green guard for selection scoping.  Without it -- the old whole-file
path -- a region-only reply would REPLACE the entire buffer with those two lines and
lines 1/4/5 would be lost; here they must remain.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
# what the model returns for the SELECTED region (lines 2-3) only:
REGION = "int b = 22;\nint c = 33;\n"


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


def test_edit_scoped_to_selection(tmp_path):
    trace = tmp_path / "e.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": REGION,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s._drain(0.6)
        s.key("\033[B")                       # cursor -> line 2 ("int b = 2;")
        s.key(ALT.BLOCK, "b", delay=0.4)      # Block -> Begin Mark
        s.key("\033[B"); s.key("\033[B")      # cursor -> line 4 (col 0)
        s.key(ALT.BLOCK, "e", delay=0.4)      # Block -> End Mark  => region = lines 2-3
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        s.key("double the middle values"); s.key("\r", delay=0.8)
        assert _wait(s, "Proposed change"), \
            "no diff overlay for the selection edit:\n" + "\n".join(s.display())
        s.key("y", delay=0.6); s._drain(0.8)  # accept the hunk
        s.save()
        out = s.text()
    txt = trace.read_text()
    assert "sel=1..2" in txt, "selection range not detected (should be lines 1..2):\n" + txt
    assert "edit applied" in txt, "edit was not applied:\n" + txt
    # the selected region changed ...
    assert "int b = 22;" in out and "int c = 33;" in out, \
        "selected region not edited:\n" + out
    # ... and the rest of the file survived verbatim (the scoping guarantee)
    for keep in ("int a = 1;", "int d = 4;", "int e = 5;"):
        assert keep in out, "scoping lost a line outside the selection (%r):\n%s" % (keep, out)
