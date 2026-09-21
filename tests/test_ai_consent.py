"""First-use consent: state the trust model once before any AI action.

Before the first Alt-G action, xwpe shows a one-time notice (local by default,
opt-in, edits previewed and revertible) and requires an explicit go-ahead.
Declining does nothing; accepting proceeds and is remembered.

A dev/CI force-enable (XWPE_AI_ENABLE=1) implies consent so scripted runs are not
blocked; these tests set XWPE_AI_CONSENTED=0 to exercise the prompt explicitly.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
       "XWPE_AI_MOCK_REPLY": "ok", "XWPE_AI_CONSENTED": "0"}  # force the prompt


def test_first_use_prompt_and_decline(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(ENV)) as s:
        s.key(ALT.AI); s._drain(0.5)
        prompt = "\n".join(s.display())
        assert "first use" in prompt and "LOCALLY" in prompt, \
            "the first-use notice did not appear:\n" + prompt
        s.key("n", delay=0.4)                     # decline
        after = "\n".join(s.display())
    assert "not enabled" in after, "declining did not report the abort:\n" + after
    assert "Ask (chat)" not in after, \
        "the AI menu opened even though consent was declined:\n" + after


def test_first_use_accept_proceeds(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(ENV)) as s:
        s.key(ALT.AI); s._drain(0.5)
        s.key("y", delay=0.5)                     # accept -> the AI menu opens
        after = "\n".join(s.display())
    assert "Ask (chat)" in after and "Agent (tools)" in after, \
        "accepting consent did not open the AI menu:\n" + after
