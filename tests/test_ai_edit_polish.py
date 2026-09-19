"""Edit review polish: new-file line numbers, and an "N of M" applied tally.

Two separated changes make two hunks; accepting both reports "applied 2 of 2
hunks" and the trace records 2/2.  The overlay also carries new-file line numbers
so the reviewer can name the line.
"""
import os
import re
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


def test_edit_line_numbers_and_apply_tally(tmp_path):
    trace = tmp_path / "e.trace"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": NEW,
           "XWPE_AI_TRACE": str(trace)}
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        s.key("bump a and e"); s.key("\r", delay=0.8)
        assert _wait(s, "Proposed change 1/2"), "first of two hunks not shown"
        # a numbered row is present (e.g. "   1 +int a = 11;" or a context line)
        rows = [ln for ln in s.display() if re.search(r"\d+\s*[+ ]int ", ln)]
        assert rows, "no line-numbered rows in the overlay:\n" + "\n".join(s.display())
        s.key("y", delay=0.6)                       # accept hunk 1
        assert _wait(s, "Proposed change 2/2"), "second hunk not shown"
        s.key("y", delay=0.6); s._drain(0.8)         # accept hunk 2
        disp = "\n".join(s.display())
    txt = trace.read_text()
    # both hunks applied to the buffer, and the message tallies them
    assert "edit applied 2/2" in txt, "tally wrong:\n" + txt
    assert "int a = 11;" in disp and "int e = 55;" in disp, \
        "both hunks not applied to the buffer:\n" + disp
