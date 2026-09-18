"""AI assistant -- the runtime toggle and prefix discoverability.

Proves the anti-AI contract at runtime: with the assistant disabled, Alt-B does
nothing but explain how to enable it; and when enabled, an unknown mode key
shows the prefix hint (a=Ask e=Edit g=aGent m=Model).
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI assistant is off" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_ai_disabled_altb_is_block_menu(tmp_path):
    # No XWPE_AI_ENABLE -> the assistant is OFF.  Alt-B is the menu-bar "Block"
    # accelerator, so with the assistant off it must open the Block menu, NOT
    # trigger the AI: the assistant never shadows an existing key.
    env = {"XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)                 # Alt-B with AI disabled -> Block menu
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "is off" not in disp, "Alt-B wrongly triggered the AI while disabled:\n" + disp
    assert "Mark" in disp, "Alt-B did not open the Block menu when AI is off:\n" + disp


def test_ai_prefix_opens_menu(tmp_path):
    # Alt-B alone opens the action menu straight away (like Alt-F opens File),
    # no invisible second-key prefix.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "Ask" in disp and "Edit" in disp, "Alt-B did not open the menu:\n" + disp
