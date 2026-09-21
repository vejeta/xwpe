"""AI working mode persists automatically, and the quick toggle is discreet.

Cycling the permission level (Alt-G y) must:
  - not pop the AI pane up for a settings change (quiet: a pane line only if the
    pane is already open),
  - not nag about "Save Options",
  - stick across sessions on its own (no manual save).

HOME is redirected to a tmp dir so the config (~/.xwpe/xwperc) is isolated.
"""
import os
import subprocess
import tempfile

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

GEAR = "⚙"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_policy_toggle_is_quiet_and_persists(tmp_path):
    home = tempfile.mkdtemp()
    work = str(tmp_path)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "HOME": home}

    # session 1: cycle ask -> edits from the menu
    with WpeSession(work, "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        s.key(ALT.AI); s._drain(0.4)
        s.key("y"); s._drain(0.6)
        disp = s.display()
        # a one-shot flash on the bottom status line: visible, no "Save Options"
        # nag, and it opens neither the AI pane nor a modal box.
        assert "Permissions: edits" in disp[-1], \
            "the toggle did not flash a confirmation on the status line:\n" + "\n".join(disp)
        assert not any("Save Options" in r for r in disp), \
            "the toggle still nags about Save Options:\n" + "\n".join(disp)
        assert not any(GEAR in r for r in disp), \
            "the toggle opened the AI pane (should be a status-line flash):\n" + "\n".join(disp)
        # the next keystroke restores the key hints
        s.key("\033[B"); s._drain(0.4)
        assert "F1 Help" in s.display()[-1], \
            "the flash did not clear on the next key:\n" + "\n".join(s.display())

    # session 2: same HOME -> the change stuck without a manual save
    with WpeSession(work, "int main(void){return 0;}\n",
                    env_extra=dict(env), filename="t.c") as s:
        s.key(ALT.AI); s._drain(0.5)
        disp = "\n".join(s.display())
    assert "Permissions: edits" in disp, \
        "the permission level did not persist across sessions:\n" + disp
