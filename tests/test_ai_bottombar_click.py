"""AI assistant -- clicking the bottom-bar 'Alt-G AI' entry opens the menu.

The bottom status bar shows a clickable 'Alt-G AI' entry (the same idiom as the
LSP 'Alt-Q' entry).  A left click on it feeds the WPE_AI_MENU synthetic keycode
into the editor.  That code shares the numeric range of real Unicode codepoints,
so a too-low ceiling in the character-insert guard used to TYPE it into the
buffer as U+07D4 ('ok' -> the buffer gained a stray glyph) instead of opening the
menu.  This drives that exact click and asserts the buffer is untouched and the
action menu appears.

Red/green: with the guard ceiling at WPE_LSP_MENU the click inserts a character
and the menu never opens; with the ceiling at WPE_AI_MENU the click dispatches.
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

AI_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
SRC = "int main(void){return 0;}\n"


def _sgr(button, col, row, press=True):
    """An SGR 1006 mouse report at 1-based screen (col, row)."""
    return "\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")


def _find_ai_entry(disp):
    """Return the 1-based (col, row) of the 'Alt-G AI' bottom-bar entry."""
    for r, line in enumerate(disp):
        i = line.find("Alt-G AI")
        if i >= 0:
            return i + 1, r + 1
    return None


def test_bottombar_click_opens_ai_menu(tmp_path):
    with WpeSession(str(tmp_path), SRC, env_extra=AI_ENV) as w:
        disp = w.display()
        pos = _find_ai_entry(disp)
        assert pos, "'Alt-G AI' entry not on the bottom bar:\n" + "\n".join(disp)
        col, row = pos
        w.key(_sgr(0, col, row, True), _sgr(0, col, row, False), delay=0.6)
        w._drain(0.4)
        screen = "\n".join(w.display())
        assert w.alive(), "wpe died clicking the AI bottom-bar entry"
        # The buffer must be untouched -- no synthetic keycode typed as a glyph.
        assert w.text() == SRC, \
            "clicking 'Alt-G AI' altered the buffer:\n%r" % w.text()
        # The action menu opened, exactly as Alt-G would.
        assert "Ask" in screen and "Edit" in screen, \
            "bottom-bar click did not open the AI menu:\n" + screen
