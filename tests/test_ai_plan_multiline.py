"""AI PLAN mode -- a proposal whose file content is MULTI-LINE must be applied
with its line structure intact (regression for the newline-collapse the live
Ollama test exposed).  Deterministic mock.
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


def test_ai_plan_multiline_preserved(tmp_path):
    t = tmp_path / "t.c"
    new = '#include <stdio.h>\nint main(void)\n{\n    return 42;\n}\n'
    reply = "PROPOSE %s\n%s@@END\n@@PLAN-DONE fixed" % (t, new)
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(tmp_path / "ai.trace"),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n", filename="t.c",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("f")
        s.key("rewrite it")
        s.key("\r", delay=1.6)
        s.key("a", delay=1.0)            # apply all
        s._drain(1.0)
        s.save()
        disk = s.text()
    # must be several lines, not one joined line
    assert disk.count("\n") >= 4, "newlines were collapsed:\n" + repr(disk)
    assert "#include <stdio.h>" in disk
    assert "return 42;" in disk
    r = subprocess.run(["gcc", "-fsyntax-only", str(t)], stderr=subprocess.PIPE)
    assert r.returncode == 0, r.stderr.decode()
