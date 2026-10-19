"""AI assistant -- the Alt-G dropdown and the prompt popup paint under the X11/Xft backend.

Backend parity: the dropdown and the popup are ordinary windows
of the editor, so they must paint under xwpe's real Xft path and not only under
ncurses.  This drives a real xwpe under the headless X server with the mock
backend: Alt-G paints the dropdown, the letter g runs the Agent row and opens
the "AI agent task" popup with its three buttons, and Esc restores the screen.
The buttons are found by their colour: one pixel row holds exactly three runs
of the button colour.  Self-skips if the X11 harness or --enable-ai is missing.
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
    "XWPE_AI_MOCK_REPLY": "ok",
}

BUTTON_RGB = (0, 197, 205)          # the cyan of a dialog button
BUTTON_MIN_RUN_PIXELS = 30          # one button is wider than this
BUTTONS_IN_POPUP = 3                # Send, Multi-line, Cancel
PAINTED_PIXELS = 2000               # a box that painted changes far more than this
RESTORED_PIXELS = 200               # a restored screen differs by cursor noise at most


def _button_runs_per_row(img):
    """The largest number of separate button-coloured runs on one pixel row."""
    width, height = img.size
    best = 0
    for y in range(height):
        lengths = []
        length = 0
        for x in range(width):
            if img.getpixel((x, y)) == BUTTON_RGB:
                length += 1
            else:
                lengths.append(length)
                length = 0
        lengths.append(length)
        runs = len([n for n in lengths if n >= BUTTON_MIN_RUN_PIXELS])
        best = max(best, runs)
    return best


# @scenario SC-ai-menu-55
@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_alt_g_dropdown_and_prompt_popup_paint_under_x11(xwpe):
    before = xwpe.screenshot()
    xwpe.key("alt+g", delay=1.0)
    menu = xwpe.screenshot()
    xwpe.key("g", delay=1.2)                  # the Agent row: its prompt popup opens
    popup = xwpe.screenshot()
    xwpe.key("Escape", delay=0.8)
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died"
    assert changed_pixels(before, menu) > PAINTED_PIXELS, \
        "Alt-G did not paint the dropdown (screen barely changed)"
    assert changed_pixels(menu, popup) > PAINTED_PIXELS, \
        "the letter g did not open the prompt popup (screen barely changed)"
    assert _button_runs_per_row(popup) == BUTTONS_IN_POPUP, \
        "the popup does not show its three buttons"
    assert changed_pixels(before, after) < RESTORED_PIXELS, \
        "Esc did not restore the screen"
