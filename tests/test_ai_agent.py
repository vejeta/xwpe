"""AI assistant -- Agent mode (deterministic mock backend).

The mock can script several turns (XWPE_AI_MOCK_REPLY split on @@TURN@@), so we
drive the full tool loop: a DONE, an approved write_file, and a denied
run_command.  Read-only tools run automatically; write/run need y/n approval.
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


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _agent(tmp_path, reply, task="do the task", approve=None):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": reply,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("g")                       # aGent
        s.key(task)
        s.key("\r", delay=1.6)           # submit -> policy prompt
        s.key("\r", delay=1.0)           # Enter = keep the configured policy (ask)
        s._drain(1.0)
        if approve is not None:
            s.key(approve, delay=1.2)    # answer the approval prompt
            s._drain(1.2)
    return trace.read_text() if trace.exists() else ""


def test_ai_agent_done(tmp_path):
    txt = _agent(tmp_path, "DONE agent completed the task", task="tidy up")
    assert "agent task=tidy up" in txt, txt
    assert "agent done" in txt, txt


def test_ai_agent_write_file_approved(tmp_path):
    reply = "TOOL write_file agent_out.txt\nhello agent\n@@END@@TURN@@DONE wrote it"
    txt = _agent(tmp_path, reply, task="write a file", approve="y")
    assert "agent tool=write_file" in txt, txt
    out = tmp_path / "agent_out.txt"
    assert out.exists(), "agent did not create the file"
    assert "hello agent" in out.read_text()
    assert "agent done" in txt, txt


def test_ai_agent_run_command_denied(tmp_path):
    sentinel = tmp_path / "SHOULD_NOT_EXIST"
    reply = ("TOOL run_command touch %s\n@@TURN@@DONE stopped" % sentinel)
    txt = _agent(tmp_path, reply, task="run something", approve="n")
    assert "agent tool=run_command" in txt, txt
    assert not sentinel.exists(), "denied command still ran!"
    assert "agent done" in txt, txt
