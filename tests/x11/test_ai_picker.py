"""AI model picker paints under the x11 backend.

Backend parity (rules 20/21): the scrollable model picker is a boxed overlay
drawn through each backend's paint path.  This opens Options > AI and presses the
Model button (Alt-M) to raise the picker, asserting the screen changed -- i.e.
the picker painted -- over the still-shown settings dialog.  claudecli's model
list is static (default/sonnet/opus/haiku), so no network is used.
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


AI_ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "claudecli", "XWPE_AI_MODEL": "sonnet"}


@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_ai_model_picker_paints_under_x11(xwpe):
    xwpe.menu("o", "i")               # Options -> AI settings dialog
    before = xwpe.screenshot()
    xwpe.key("alt+m", delay=0.8)      # Model button -> scrollable picker
    after = xwpe.screenshot()
    assert xwpe.proc.poll() is None, "xwpe died opening the model picker"
    assert changed_pixels(before, after) > 1500,         "the model picker did not paint under x11 (screen barely changed)"
    xwpe.key("Escape", delay=0.4)     # close the picker
