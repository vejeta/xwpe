"""The plain editor (`we`) keeps Go to Line on Alt-G and still offers Options > AI.

The one executable picks its mode from the name it is started under.  Under
`we` the programming layer, and with it the Alt-G prefix, is off; under `wpe`
Alt-G opens the assistant's dropdown.  These two tests start `we` on purpose,
through WPE_BIN, to pin what the plain editor shows.
"""
import os
import pytest
import wpe_driver
from wpe_driver import WpeSession, ALT, assistant_build

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
GO_TO_LINE_ROW_WITH_KEY = "Go to Line        Alt G"


def _plain_editor_binary():
    """The `we` binary next to the one under test, or skip when it is absent."""
    here = os.path.dirname(os.path.abspath(wpe_driver.WPE_BIN))
    path = os.path.join(here, "we")
    if not os.path.exists(path):
        pytest.skip("no plain-editor binary next to %s" % wpe_driver.WPE_BIN)
    return path


def _row_with(disp, needle):
    return next((r for r in disp if needle in r), "")


# @scenario SC-ai-menu-02
def test_we_alt_g_is_go_to_line_and_the_search_row_advertises_it(tmp_path, monkeypatch):
    """Under `we`, Alt-G opens the "Goto Line Number" box and the Search row
    reads "Go to Line        Alt G"; under `wpe` the same row shows no key."""
    with WpeSession(str(tmp_path), "L1\nL2\nL3\n", filename="p.txt", env_extra=dict(ENV)) as s:
        s.key(ALT.SEARCH, delay=0.6)
        wpe_row = _row_with(s.display(), "Go to Line")
        s.key("\033", delay=0.3)
    monkeypatch.setattr(wpe_driver, "WPE_BIN", _plain_editor_binary())
    with WpeSession(str(tmp_path), "L1\nL2\nL3\n", filename="q.txt", env_extra=dict(ENV)) as s:
        s.key(ALT.SEARCH, delay=0.6)
        we_row = _row_with(s.display(), "Go to Line")
        s.key("\033", delay=0.3)
        s.key(ALT.AI, delay=0.6)
        after_alt_g = "\n".join(s.display())
        s.key("\033", delay=0.3)
    assert "Goto Line Number" in after_alt_g, \
        "under we, Alt-G did not open the Goto Line box:\n" + after_alt_g
    assert "Ask (chat)" not in after_alt_g, "under we, Alt-G opened the assistant dropdown"
    assert GO_TO_LINE_ROW_WITH_KEY in we_row, "we: the Search row does not read Alt G: %r" % we_row
    assert "Alt G" not in wpe_row, "wpe: the Search row still advertises Alt G: %r" % wpe_row


# @scenario SC-ai-menu-05
def test_we_options_menu_ends_with_the_ai_row_and_i_opens_the_dialog(tmp_path, monkeypatch):
    """Under `we`, with the assistant off, the last Options row is AI and the
    letter i opens the AI settings dialog."""
    monkeypatch.setattr(wpe_driver, "WPE_BIN", _plain_editor_binary())
    env = {"XWPE_AI_BACKEND": "mock"}               # assistant off: the row must be there anyway
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=env) as s:
        s.key(ALT.OPTIONS, delay=0.7)
        menu = s.display()
        s.key("i", delay=1.0)
        dialog = "\n".join(s.display())
        s.key("\033", delay=0.5)
    ai = next((i for i, r in enumerate(menu) if " AI " in r and r.count("x") >= 2), None)
    assert ai is not None, "no AI row in the Options menu under we:\n" + "\n".join(menu)
    assert "mqq" in menu[ai + 1], "AI is not the last Options row (next row: %r)" % menu[ai + 1]
    assert "AI settings" in dialog and "Backend" in dialog, \
        "i did not open the AI settings dialog under we:\n" + dialog
