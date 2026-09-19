"""AI Edit diff overlay paints, in colour, under the native Wayland backend.

Backend parity (rules 20/21): the review overlay's red (deleted) / green (added)
lines come from a DIFFERENT colour path per backend -- ncurses colour pairs
(pyte tests) versus the Wayland/Cairo path here.  This drives a real xwpe as a
native Wayland client, runs an AI edit whose mock reply rewrites the file, and
asserts the overlay painted and added red AND green regions the editor did not
have before.  Self-skips if the graphical harness or --enable-ai is missing.
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


NEWFILE = "int main(void)\n{\n    return 42;\n}\n"
AI_ENV = {
    "XWPE_AI_ENABLE": "1",
    "XWPE_AI_BACKEND": "mock",
    "XWPE_AI_MOCK_REPLY": NEWFILE,
}


def _red_green(img):
    px = img.load()
    w, h = img.size
    red = green = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y][:3]
            if r > 130 and g < 80 and b < 80:
                red += 1
            elif g > 130 and r < 120 and b < 120:
                green += 1
    return red, green


@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_ai_diff_overlay_colours_under_wayland(xwpe):
    before = xwpe.screenshot()
    rb, gb = _red_green(before)
    xwpe.key("alt+g")                 # AI prefix
    xwpe.key("e", delay=0.6)          # e = Edit -> instruction dialog
    xwpe.type("rewrite the file")
    xwpe.key("Return", delay=2.2)     # stream (mock) then the diff overlay opens
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died running the AI edit"
    assert changed_pixels(before, after) > 2000, \
        "the diff overlay did not paint under native Wayland"
    ra, ga = _red_green(after)
    assert ra - rb > 150, \
        "no red (deleted) lines in the diff overlay: %d -> %d" % (rb, ra)
    assert ga - gb > 150, \
        "no green (added) lines in the diff overlay: %d -> %d" % (gb, ga)
    xwpe.key("q", delay=0.5)          # dismiss the overlay (cancel, no apply)
