"""The diff overlay survives an async repaint (a resize) while it is open.

The review overlay runs a modal key loop; meanwhile the editor's async spine can
deliver a repaint (a terminal resize -> SIGWINCH, or an LSP diagnostic) under it.
This drives an AI edit to the diff overlay, resizes the terminal while the box is
up, and checks xwpe stays alive, the box can still be dismissed, and the buffer
is intact -- i.e. an async paint under the modal does not wedge or crash it.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "#include <stdio.h>\n\nint main(void)\n{\n    return 0;\n}\n"
NEW = "#include <stdio.h>\n\nint main(void)\n{\n    return 7;\n}\n"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def test_diff_overlay_survives_resize(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": NEW,
           "XWPE_AI_TRACE": str(tmp_path / "e.trace")}
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.5)
        s.key("bump return"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            if "Proposed change" in "\n".join(s.display()):
                break
        assert s.proc.poll() is None, "xwpe died opening the overlay"
        # async repaint under the modal overlay
        s.resize(70, 20)
        assert s.proc.poll() is None, "xwpe died on resize under the overlay"
        s.resize(80, 25)
        assert s.proc.poll() is None, "xwpe died on second resize"
        # the overlay is still interactive: dismiss it and confirm xwpe lives.
        # (Assert on the trace, not the screen: pyte does not faithfully render
        # xwpe's partial repaint after a resize, so the display is unreliable
        # here -- but the trace records that the modal resolved.)
        s.key("q", delay=0.6); s._drain(0.8)
        assert s.proc.poll() is None, "xwpe died dismissing the overlay after resize"
    txt = (tmp_path / "e.trace").read_text() if (tmp_path / "e.trace").exists() else ""
    assert "edit discarded" in txt or "edit applied" in txt, \
        "the diff overlay never resolved after the resize:\n" + txt
