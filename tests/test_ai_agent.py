"""AI assistant -- Agent mode smoke test (deterministic mock backend).

The mock replies "DONE ..." so the agent loop terminates on the first turn,
exercising the task prompt -> loop -> DONE path without running any tool.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"autonomous coding agent" in out
    except Exception:
        return False


@pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")
def test_ai_agent_done(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "DONE agent completed the task",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)                 # Alt-B
        s.key("g")                       # aGent
        s.key("tidy up the file")        # task
        s.key("\r", delay=1.2)           # submit -> agent loop -> DONE
        s._drain(1.0)

    txt = trace.read_text() if trace.exists() else ""
    assert "agent task=tidy up the file" in txt, txt
    assert "agent done" in txt, txt
