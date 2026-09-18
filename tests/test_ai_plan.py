"""AI assistant -- PLAN mode (Alt-G p): multi-file study -> plan -> permission
-> apply.  Deterministic mock: the reply carries a PLAN over two files (the
open one and one only on disk).  Covers apply-all, cancel, and file-by-file.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"PROPOSE" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _plan(tmp_path, keys):
    t = tmp_path / "t.c"
    other = tmp_path / "other.c"
    other.write_text("int other(void){return 0;}\n")
    trace = tmp_path / "ai.trace"
    reply = ("PROPOSE %s\nint main(void){return 42;}\n@@END\n" % t
             + "PROPOSE %s\nint other(void){return 7;}\n@@END\n" % other
             + "@@PLAN-DONE two files fixed")
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("p")                       # Plan
        s.key("fix both files")
        s.key("\r", delay=1.6)           # study (none needed) -> plan pane
        s._drain(1.0)
        disp_plan = "\n".join(s.display())
        for k in keys:
            s.key(k, delay=0.9)
        s._drain(1.0)
        s.save()                         # saves the active (last applied) window
    txt = trace.read_text() if trace.exists() else ""
    return disp_plan, txt, t, other


def test_ai_plan_apply_all(tmp_path):
    disp, txt, t, other = _plan(tmp_path, ["a"])
    assert "proposes to change 2 files" in disp, disp
    assert "plan proposals=2" in txt, txt
    assert ("plan applied %s" % t) in txt and ("plan applied %s" % other) in txt, txt
    assert "plan done applied=2" in txt, txt
    # the last applied window (other.c) was saved to disk by the test
    assert "return 7" in other.read_text()


def test_ai_plan_cancel(tmp_path):
    disp, txt, t, other = _plan(tmp_path, ["q"])
    assert "plan cancelled" in txt, txt
    assert "return 0" in other.read_text() and "7" not in other.read_text()


def test_ai_plan_file_by_file(tmp_path):
    # f = file by file; then per-hunk: y for t.c, n for other.c
    disp, txt, t, other = _plan(tmp_path, ["f", "y", "n"])
    assert ("plan applied %s" % t) in txt, txt
    assert ("plan skipped %s" % other) in txt, txt
    assert "plan done applied=1" in txt, txt
