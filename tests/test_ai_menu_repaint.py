"""A working op's spinner must not repaint over an open menu.

While Edit/Plan/Agent stream, a heartbeat spinner repaints the AI pane about once
a second.  If a menu is open (its box overlaps the bottom-docked pane), that
repaint used to draw over it and corrupt it -- the menu "flickered away" under
the cursor animation.  The pane paint now defers while a menu/dialog is up
(wpe_modal_active), the same guard the async language-server painter uses.

Reproduction: start a slow (delayed-mock) Edit so the spinner is live, click the
bottom-bar "Alt-G AI" entry to open the action menu (that click opens the menu
even while busy), then wait past a spinner tick and assert the menu items are
still on screen.

Red/green: without the guard the spinner tick repaints the pane over the menu and
the items vanish within a second; with it they persist.
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


def _sgr(button, col, row, press=True):
    return "\x1b[<%d;%d;%d%s" % (button, col, row, "M" if press else "m")


def _find_ai_entry(disp):
    for r, line in enumerate(disp):
        i = line.find("Alt-G AI")
        if i >= 0:
            return i + 1, r + 1
    return None


def test_spinner_does_not_repaint_over_menu(tmp_path):
    # a long mock delay keeps the op in-flight (spinner ticking) for the whole test
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "x",
           "XWPE_AI_MOCK_DELAY_MS": "6000", "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    with WpeSession(str(tmp_path), "hello\nworld\n", env_extra=env, filename="t.txt") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.4)
        s.key("do something"); s.key("\r", delay=0.8)     # Edit starts -> spinner live
        pos = _find_ai_entry(s.display())
        assert pos, "'Alt-G AI' bottom-bar entry not found:\n" + "\n".join(s.display())
        col, row = pos
        s.key(_sgr(0, col, row, True), _sgr(0, col, row, False), delay=0.6)
        assert "Ask" in "\n".join(s.display()), \
            "the AI menu did not open on the bottom-bar click:\n" + "\n".join(s.display())
        s._drain(2.0)                                     # let a spinner tick land
        disp = "\n".join(s.display())
    # the menu survived the repaint that fires while it is open
    assert "Ask" in disp and "Fix the build" in disp, \
        "the spinner repainted over the open menu (items lost):\n" + disp
