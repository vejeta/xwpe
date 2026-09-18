"""AI assistant -- Edit mode smoke test (deterministic mock backend).

The mock returns the whole modified file (XWPE_AI_MOCK_REPLY); the Edit flow
diffs it against the buffer, previews, and on Enter applies it with one undo
snapshot.  Verifies the prompt->generate->diff->accept->apply path ran.
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


@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_ai_edit_mock(tmp_path):
    trace = tmp_path / "ai.trace"
    new_content = "int main(void){return 42;}"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": new_content,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)                 # Alt-B
        s.key("e")                       # Edit
        s.key("return 42 instead")       # instruction
        s.key("\r", delay=1.2)           # submit -> generate -> diff preview
        s._drain(1.0)
        disp_preview = "\n".join(s.display())
        s.key("\r", delay=0.8)           # accept the diff
        s._drain(0.6)

    txt = trace.read_text() if trace.exists() else ""
    assert "edit instr=return 42 instead" in txt, txt
    assert "edit applied" in txt, txt
    assert "42" in disp_preview, "diff preview did not show the new content"
