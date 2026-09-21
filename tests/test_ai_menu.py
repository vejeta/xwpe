"""AI assistant -- the bottom-bar entry and its action menu.

Parity with the LSP bottom bar: when the assistant is enabled the editor's
status bar gains a mouse-clickable "Alt-G AI" entry, and opening it (a click,
or Alt-G followed by any unbound key) pops up a menu listing every AI action.
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
        return b"Alt-G AI" in out
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
    assert "Alt-G AI" in disp, "AI bar entry not shown when enabled:\n" + disp


def test_ai_bar_entry_absent_when_disabled(tmp_path):
    # No XWPE_AI_ENABLE -> classic bar; the entry must not appear.
    env = {"XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "Alt-G AI" not in disp, "AI bar entry leaked while disabled:\n" + disp


def test_ai_bar_entry_on_code_file(tmp_path):
    # On a .c file (which gets the contextual LSP bar) enabling AI shows the
    # COMBINED bar: the "Alt-Q ? <server>" LSP hint stays AND the "Alt-G AI"
    # entry is added -- both features are usable at once, neither hint is lost.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    filename="prog.c", env_extra=env) as s:
        s._drain(0.6)
        disp = "\n".join(s.display())
    assert "Alt-G AI" in disp, "AI bar entry missing on a code file:\n" + disp
    assert "Alt-Q" in disp, "LSP hint lost on a code file when AI is on:\n" + disp


def test_ai_bar_refreshes_when_toggled_in_options(tmp_path):
    # Start disabled -> no entry; enable via Options>AI (where the toggle now
    # lives) -> the bar must gain the entry WITHOUT reopening the file.
    env = {"XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), "hello\n", filename="notes.txt",
                    env_extra=env) as s:
        s._drain(0.5)
        before = "\n".join(s.display())
        s.key("\033o", delay=0.8); s.key("i", delay=1.0)       # Options>AI
        s.key(" ", delay=0.7)                                  # toggle Enable (focused first)
        s.key("\033o", delay=1.0)                              # Ok
        s._drain(0.8)
        after = "\n".join(s.display())
    assert "Alt-G AI" not in before, "entry shown before enabling:\n" + before
    assert "Alt-G AI" in after, "bar did not refresh after enabling:\n" + after


def test_ai_menu_opens_and_lists_actions(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s.key(ALT.AI)                 # Alt-G opens the menu directly
        s._drain(0.8)
        disp = "\n".join(s.display())
    txt = trace.read_text() if trace.exists() else ""
    assert "menu open" in txt, "menu did not open:\n" + txt
    assert "Ask" in disp and "Multi-file" in disp, "menu actions not listed:\n" + disp


def test_menu_shows_clear_labels_and_state(tmp_path):
    # the menu uses clear labels, surfaces the active permission level, has a
    # divider between the actions and the settings, and points to Options>AI for
    # the full settings (Model/enable now live there, not in the menu).
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_POLICY": "edits"}
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s.key(ALT.AI); s._drain(0.6)
        disp = "\n".join(s.display())
    for want in ("Build & fix (agent)", "Permissions: edits",
                 "Clear conversation", "AI settings", "----"):
        assert want in disp, "menu missing %r:\n%s" % (want, disp)
    # the old jargon is gone, and the settings moved out of the action list
    for gone in ("Policy dial", "Fix the build", "Pick model", "Disable"):
        assert gone not in disp, "old menu label still present: %r\n%s" % (gone, disp)


def test_menu_settings_entry_opens_options(tmp_path):
    # "AI settings..." (Alt-G s) opens the Options > AI dialog.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
    with WpeSession(str(tmp_path), SEED, filename="notes.txt",
                    env_extra=env) as s:
        s.key(ALT.AI); s._drain(0.5)
        s.key("s"); s._drain(0.8)
        disp = "\n".join(s.display())
    assert "AI settings" in disp and "Backend" in disp and "Permission" in disp, \
        "the settings entry did not open Options > AI:\n" + disp
