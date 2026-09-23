"""The AI task popup's button accelerators must fire on their shown letter.

In the "AI agent task" popup the buttons underline Send=S, Multi-line=M and
Cancel=C, and each must fire on Alt-<that letter> (they previously showed one
letter but fired on a different key, so Alt-S / Alt-i did nothing). Driven with
the mock backend so the popup opens without a real model.
"""
import os
import subprocess
import tempfile

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

ALT_M = "\033m"
ALT_C = "\033c"


def _open_agent_prompt(s):
    # Alt-G g == Agent; with the mock backend preflight passes and the
    # "AI agent task" prompt popup opens.
    s.key(ALT.AI, delay=0.5)
    s.key("g", delay=0.7)


def test_altm_opens_multiline_composer(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok"}
    # A SHORT workdir: the composer window title is the scratch path + our hint,
    # and a long pytest tmp path would push "Esc = done" off the frame width.
    with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        _open_agent_prompt(s)
        s.key("hello", delay=0.5)
        s.key(ALT_M, delay=1.2)          # Alt-M -> Multi-line composer (editor opens)
        disp = "\n".join(s.display())
    # The composer window's title carries the finish hint; its presence proves
    # Alt-M fired the Multi-line button (not typed 'm' into the field).
    assert "Esc = done" in disp, \
        "Alt-M did not open the multi-line composer:\n" + disp


def test_altc_cancels_prompt(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        _open_agent_prompt(s)
        before = "\n".join(s.display())
        assert "AI agent task" in before, "the agent prompt did not open:\n" + before
        s.key("hello", delay=0.3)
        s.key(ALT_C, delay=0.6)          # Alt-C -> Cancel
        after = "\n".join(s.display())
    # The popup is gone (cancelled) and nothing was sent.
    assert "AI agent task" not in after, \
        "Alt-C did not cancel the prompt popup:\n" + after
