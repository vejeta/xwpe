"""AI assistant -- the permission dial (ask / edits / auto) in Agent mode.

policy=auto: a write_file passes WITHOUT the y/n prompt (and a checkpoint is
taken first); policy=ask: the prompt appears and 'y' is needed.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"auto-approved" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

REPLY = "TOOL write_file out.txt\nhello agent\n@@END@@TURN@@DONE wrote it"


def _agent(tmp_path, policy, extra_keys):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": REPLY,
        "XWPE_AI_TRACE": str(trace),
    }
    if policy:
        env["XWPE_AI_POLICY"] = policy
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s.key("g")                       # aGent
        s.key("write a file")
        s.key("\r", delay=1.2)           # submit -> agent loop (policy from env)
        for k in extra_keys:
            s.key(k, delay=1.2)
        s._drain(1.5)
    return trace.read_text() if trace.exists() else ""


def test_ai_policy_auto_no_prompt(tmp_path):
    # 'a' answers the end-of-run changeset review (keep all): the snapshot
    # checkpoint now detects the NEW out.txt, so the review loop asks.
    txt = _agent(tmp_path, "auto", ["a"])
    assert "agent policy=auto" in txt, txt
    assert "checkpoint" in txt, "auto must take a checkpoint first:\n" + txt
    assert "agent auto-approve" in txt, txt
    assert (tmp_path / "out.txt").exists() and "hello agent" in (tmp_path / "out.txt").read_text()
    assert "agent done" in txt, txt
    assert "changeset n=1" in txt, "snapshot mode must detect the new file:\n" + txt
    assert "changeset keep all" in txt, txt


def test_ai_policy_auto_revert_new_file(tmp_path):
    # 'r' reverts all: the file the agent CREATED (snapshot mode) is deleted.
    txt = _agent(tmp_path, "auto", ["r"])
    assert "changeset n=1" in txt, txt
    assert "changeset revert all" in txt, txt
    assert not (tmp_path / "out.txt").exists(), "reverting must delete the new file"


def test_ai_policy_ask_prompts(tmp_path):
    txt = _agent(tmp_path, None, ["y"])          # approve when asked
    assert "agent policy=ask" in txt, txt
    assert "auto-approve" not in txt, "ask must not auto-approve:\n" + txt
    assert "checkpoint" not in txt, "ask mode takes no checkpoint:\n" + txt
    assert (tmp_path / "out.txt").exists()
    assert "agent done" in txt, txt
