"""A second AI request while one runs is QUEUED, not a cancel.

Alt-G no longer cancels a running task; it opens the menu, and starting another
action queues it (depth 1) to run when the current one finishes.  Esc is the
explicit cancel, and it also clears the queue.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SLOW = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "DONE finished", "XWPE_AI_MOCK_DELAY_MS": "2500"}


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _start_agent(s, task):
    # space the menu -> agent -> prompt steps: while a task runs, the menu needs
    # a beat to render before the 'g' shortcut and the prompt before typing.
    s.key(ALT.AI); s._drain(0.5)
    s.key("g"); s._drain(0.6)
    s.key(task); s._drain(0.3)
    s.key("\r", delay=0.8); s._drain(0.5)


def test_second_request_queues_and_runs(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(SLOW), filename="t.c") as s:
        _start_agent(s, "TASKONE")
        assert any("working" in r for r in s.display()), "task one did not start"
        _start_agent(s, "TASKTWO")                       # while busy -> queue
        assert not any("cancelled" in r for r in s.display()), \
            "the second request cancelled the first:\n" + "\n".join(s.display())
        ran = False
        for _ in range(12):
            s._drain(1.0)
            if any("task: TASKTWO" in r for r in s.display()):
                ran = True
                break
    assert ran, "the queued request never ran after the first finished"


def test_esc_cancels_running_task(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(SLOW), filename="t.c") as s:
        _start_agent(s, "TASKONE")
        s.key("\033", delay=0.6)                          # Esc -> cancel
        s._drain(0.5)
        disp = "\n".join(s.display())
    assert "cancelled" in disp, "Esc did not cancel the running task:\n" + disp


def test_esc_clears_the_queue(tmp_path):
    # task one must still be running when Esc is pressed, so it runs long enough
    # that the enqueue + Esc happen well within its window.
    env = dict(SLOW); env["XWPE_AI_MOCK_DELAY_MS"] = "12000"
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        _start_agent(s, "TASKONE")
        assert any("working" in r for r in s.display()), "task one did not start"
        _start_agent(s, "TASKTWO")                       # queued (task one still busy)
        # cancel (Esc) tears down task one AND clears the queue; task two must
        # therefore never run.
        s.key("\033", delay=0.8); s._drain(0.6)
        ran = False
        for _ in range(6):
            s._drain(1.0)
            if any("task: TASKTWO" in r for r in s.display()):
                ran = True
                break
    assert not ran, "Esc did not clear the queued request (it still ran)"
