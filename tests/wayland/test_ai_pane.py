"""AI assistant -- the AI pane paints under the native Wayland backend.

Backend parity (rules 20/21): the AI pane is an ordinary FENSTER, so it must
render in every backend, not just ncurses.  This drives a real xwpe as a native
Wayland client, opens the AI chat (deterministic mock reply), and asserts the
frame changed -- i.e. the pane painted.  Self-skips if the graphical harness or
--enable-ai is missing.
"""
import subprocess
import pytest
from conftest import changed_pixels, XWPE_BIN


def _has_ai():
    try:
        out = subprocess.run(["strings", XWPE_BIN],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"Alt-G AI" in out
    except Exception:
        return False


AI_ENV = {
    "XWPE_AI_ENABLE": "1",
    "XWPE_AI_BACKEND": "mock",
    "XWPE_AI_MOCK_REPLY": "PONGMARKER hello from the mock",
}


@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_ai_pane_paints_under_wayland(xwpe):
    before = xwpe.screenshot()
    xwpe.key("alt+g")                 # AI prefix
    xwpe.key("a", delay=0.6)          # a = Ask -> prompt dialog
    xwpe.type("what does this do")    # a prompt
    xwpe.key("Return", delay=1.6)     # submit -> the AI pane paints the reply
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the AI chat"
    assert changed_pixels(before, after) > 2000, \
        "the AI pane did not paint under native Wayland (frame barely changed)"
