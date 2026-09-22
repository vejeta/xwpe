"""Agent plan mode: the agent states a checklist before acting, and ticks it off.

For a multi-step task the agent can reply TOOL plan with one task per line; xwpe
shows it as a "[ ] task" checklist so the user sees the intended steps before any
change.  As the agent finishes a step it reports TOOL step_done, appending a
"[x] step" row -- a lightweight live TODO list in the transcript.
"""
import os
import time
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


def test_agent_plan_and_step_done(tmp_path):
    turns = ("TOOL plan\nInspect the file\nChange the return value\nVerify build\n@@END"
             "@@TURN@@TOOL step_done Inspect the file"
             "@@TURN@@DONE finished the task")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("do a multi step change"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            t = (tmp_path / "a.trace").read_text() if (tmp_path / "a.trace").exists() else ""
            if "agent done" in t:
                break
        disp = "\n".join(s.display())
    assert "[plan]" in disp, "the plan header was not shown:\n" + disp
    assert "[ ] Inspect the file" in disp and "[ ] Change the return value" in disp, \
        "the plan checklist was not rendered:\n" + disp
    assert "[x] Inspect the file" in disp, \
        "the completed step was not ticked off:\n" + disp
