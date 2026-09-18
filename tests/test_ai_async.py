"""AI assistant -- asynchronous / non-blocking behaviour, headless.

Uses the mock backend's XWPE_AI_MOCK_DELAY_MS knob to make the reply arrive
after a delay (a slow real model, faked deterministically), so the anti-clunky
contract can be checked in CI without a live model: while a background Edit
generates, the editor keeps accepting keystrokes, the spinner animates, and
Alt-B cancels the task.
"""
import os
import re
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"(async)" in out and b"Alt-B AI" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _spin_secs(disp):
    m = re.search(r"working .  (\d+)s", disp)
    return int(m.group(1)) if m else -1


def _start_edit(s):
    s.key(ALT.BLOCK); s.key("e", delay=0.8)   # Alt-B e -> prompt
    s.key("append a line")
    s.key("\r", delay=1.5)                     # submit -> async edit (delayed reply)


def test_editor_interactive_and_spinner_during_generation(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "int main(void){return 1;}",
           "XWPE_AI_MOCK_DELAY_MS": "4000"}
    with WpeSession(str(tmp_path), "AAA\n", filename="stack.c",
                    env_extra=env) as s:
        _start_edit(s)
        sp0 = _spin_secs("\n".join(s.display()))
        # the editor MUST accept typing while the model "generates"
        s.key("Z"); s.key("Z"); s.key("Z")
        s._drain(1.6)
        disp = "\n".join(s.display())
        sp1 = _spin_secs(disp)
        # cancel so we do not fall into the diff modal
        s.key(ALT.BLOCK); s._drain(0.8)
    assert sp1 > sp0 >= 0, "spinner did not advance (editor loop stalled):\n" + disp
    assert "ZZZ" in disp.replace(" ", ""), \
        "typing did not land while generating (editor was blocked):\n" + disp


def test_alt_b_cancels_a_running_task(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "int main(void){return 1;}",
           "XWPE_AI_MOCK_DELAY_MS": "4000"}
    with WpeSession(str(tmp_path), "AAA\n", filename="stack.c",
                    env_extra=env) as s:
        _start_edit(s)
        s._drain(1.0)                          # task is running (reply not in yet)
        s.key(ALT.BLOCK); s._drain(1.0)        # Alt-B cancels it
        disp = "\n".join(s.display())
    assert "cancelled" in disp, "Alt-B did not cancel the running task:\n" + disp
