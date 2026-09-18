"""AI assistant -- the bottom-bar entry and its action menu.

Parity with the LSP bottom bar: when the assistant is enabled the editor's
status bar gains a mouse-clickable "Alt-B AI" entry, and opening it (a click,
or Alt-B followed by any unbound key) pops up a menu listing every AI action.
When the assistant is off the entry is absent -- the classic bar is untouched.

A plain .txt file is used on purpose: a .c file gets the contextual LSP bar
instead, which (by design) keeps priority over the AI entry.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-B AI" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

SEED = "hello world\n"


def test_ai_bar_entry_shown_when_enabled(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "Alt-B AI" in disp, "AI bar entry not shown when enabled:\n" + disp


def test_ai_bar_entry_absent_when_disabled(tmp_path):
    # No XWPE_AI_ENABLE -> classic bar; the entry must not appear.
    env = {"XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "Alt-B AI" not in disp, "AI bar entry leaked while disabled:\n" + disp


def test_ai_menu_opens_and_lists_actions(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s.key(ALT.BLOCK)                 # Alt-B prefix
        s.key("?")                       # unbound key -> open the action menu
        s._drain(0.8)
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "menu open" in txt, "menu did not open:\n" + txt
    assert "Ask" in disp and "Plan" in disp, "menu actions not listed:\n" + disp
