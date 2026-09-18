"""AI assistant -- the runtime toggle and prefix discoverability.

Proves the anti-AI contract at runtime: with the assistant disabled, Alt-G says
only how to enable it; when enabled, Alt-G opens the action menu.  Alt-G is a
free Alt-letter (the menu-bar Block accelerator stays Alt-B), so the assistant
never shadows an existing key.
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


def test_ai_disabled_shows_hint(tmp_path):
    # No XWPE_AI_ENABLE -> the assistant is OFF.  Alt-G explains how to enable it.
    env = {"XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)                    # Alt-G with AI disabled
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "is off" in disp, "disabled hint not shown:\n" + disp


def test_altb_is_still_the_block_menu(tmp_path):
    # The assistant lives on Alt-G, so Alt-B must remain the Block menu whether
    # the assistant is on or off -- it is never shadowed.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)                 # Alt-B -> Block menu, not the AI
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "is off" not in disp, "Alt-B wrongly reached the AI:\n" + disp
    assert "Mark" in disp, "Alt-B did not open the Block menu:\n" + disp


def test_ai_prefix_opens_menu(tmp_path):
    # Alt-G opens the action menu straight away (like Alt-F opens File).
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "Ask" in disp and "Edit" in disp, "Alt-G did not open the menu:\n" + disp
