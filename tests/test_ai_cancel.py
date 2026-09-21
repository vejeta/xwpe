"""Cancelling a running AI task leaves a usable pane and centred dialogs.

Pressing Alt-G while a task runs cancels it (a one-key escape).  After that:
  - re-opening an AI action shows its prompt centred on the SCREEN, not squashed
    onto the small AI pane that happened to be the active window;
  - the pane is armed for input, so you can keep working (type a follow-up) right
    there instead of a dead "cancelled" pane.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SLOW = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": "a reply here", "XWPE_AI_MOCK_DELAY_MS": "4000"}


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_prompt_centered_after_cancel(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(SLOW), filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("task one"); s.key("\r", delay=0.8); s._drain(0.6)   # working
        s.key("\033"); s._drain(0.6)                                # Esc cancels
        s.key(ALT.AI); s.key("g"); s._drain(0.6)                    # agent again
        disp = s.display()
        row = next((r for r in disp if "AI agent task" in r), "")
        assert row, "the agent prompt did not reopen after cancel:\n" + "\n".join(disp)
        col = row.index("AI agent task")
        assert col > 20, \
            "the prompt is not centred on screen (col %d) -- squashed onto the pane:\n%s" \
            % (col, "\n".join(disp))
        s.key("SECONDTASK"); s._drain(0.4)
        assert any("SECONDTASK" in r for r in s.display()), \
            "could not type into the reopened prompt:\n" + "\n".join(s.display())


def test_pane_typeable_after_cancel(tmp_path):
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=dict(SLOW), filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("task one"); s.key("\r", delay=0.8); s._drain(0.6)   # working
        s.key("\033"); s._drain(1.0)                                # Esc cancels -> arms input
        s.key("CONTINUEHERE"); s._drain(0.4)                        # type in the pane
        s.key("\r", delay=0.6)                                      # send it as a chat turn
        s._drain(0.5)
        disp = "\n".join(s.display())
    assert "You: CONTINUEHERE" in disp, \
        "the pane was not typeable after cancel:\n" + disp
