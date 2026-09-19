"""Resizing with the AI pane open re-lays-out and never crashes (native Wayland).

Backend parity (rules 20/21): the AI pane is a docked FENSTER, and resize is the
class of bug rule 20 warns about (a docked window's relayout regressed unnoticed
because no suite resized with it open -- and Wayland reallocates the cell planes
to the exact size on every resize, so an off-grid write corrupts the heap).  This
opens the AI chat pane, then sweeps the surface through a deep shrink/grow and
asserts xwpe survives with the pane present.
"""
import time
import subprocess
import pytest
from conftest import XWPE_BIN, _xdo


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
    "XWPE_AI_MOCK_REPLY": "a docked reply that keeps the pane on screen",
}


@pytest.mark.skipif(not _has_ai(), reason="xwpe built without --enable-ai")
@pytest.mark.parametrize("xwpe", [AI_ENV], indirect=True)
def test_ai_pane_survives_resize_wayland(xwpe):
    xwpe.key("alt+g")                 # open the AI chat pane
    xwpe.key("a", delay=0.6)
    xwpe.type("hello")
    xwpe.key("Return", delay=1.5)     # pane docked at the bottom with a reply
    assert xwpe.proc.poll() is None, "xwpe died opening the AI pane"
    for rep in range(2):
        for w in range(1000, 380, -16):        # shrink deep (bottom bar overflows)
            _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.75)))
            time.sleep(0.03)
        for w in range(380, 1000, 16):
            _xdo("windowsize", xwpe.wid, str(w), str(int(w * 0.75)))
            time.sleep(0.03)
        assert xwpe.proc.poll() is None, \
            "xwpe died resizing with the AI pane open (rep %d)" % rep
    _xdo("windowsize", xwpe.wid, "1000", "750")
    time.sleep(0.4)
    assert xwpe.proc.poll() is None, "xwpe died after the AI-pane resize sweep"
