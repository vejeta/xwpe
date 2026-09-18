"""AI assistant -- the model picker is a navigable radio list.

Alt-G m opens the standard dialog radio list (arrows move, Enter confirms), the
same widget every LSP picker uses, instead of a blind "type the name" prompt.
The mock backend advertises one model, so Enter on the open list selects it and
the choice is recorded (trace: "model set mock-model").
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-G AI" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_ai_model_picker_selects(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("m", delay=1.0)            # pick Model -> radio list opens
        disp = "\n".join(s.display())
        s.key("\r", delay=1.0)           # Enter confirms the focused radio
        s._drain(0.8)
    txt = trace.read_text() if trace.exists() else ""
    assert "mock-model" in disp, "picker did not list the model:\n" + disp
    assert "model set mock-model" in txt, "model not selected:\n" + txt
