"""AI assistant -- Edit mode (deterministic mock backend).

The mock returns the whole modified file; Edit diffs it, previews per hunk, and
on 'y' applies it with one undo snapshot, then returns focus to the file window
so a Save writes the change.  Covers accept-writes-file and reject-keeps-file.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"precise code editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _edit(tmp_path, decision):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "int main(void){return 42;}",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("e")                       # Edit
        s.key("return 42 instead")
        s.key("\r", delay=1.3)           # submit -> generate -> per-hunk preview
        s._drain(1.0)
        disp = "\n".join(s.display())
        s.key(decision, delay=0.9)       # 'y' accept / 'n' reject the hunk
        s._drain(0.6)
        s.save()
        disk = s.text()
    return disp, disk, (trace.read_text() if trace.exists() else "")


def test_ai_edit_accept_writes_file(tmp_path):
    disp, disk, txt = _edit(tmp_path, "y")
    assert "42" in disp, "diff preview did not show the new content:\n" + disp
    assert "edit applied" in txt, txt
    assert "return 42" in disk, "file was not modified on disk:\n" + disk


def test_ai_edit_reject_keeps_file(tmp_path):
    disp, disk, txt = _edit(tmp_path, "n")
    assert "edit discarded" in txt, txt
    assert "return 0" in disk and "42" not in disk, "file changed despite reject:\n" + disk
