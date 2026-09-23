"""Text can be copied out of the AI chat pane to the clipboard.

The AI pane is a read-only tool/output window (ins == 8).  The clipboard copy
(Edit > Copy / ^C) used to refuse EVERY ins == 8 window -- a guard meant only
to stop the clipboard viewer copying from itself -- so a marked AI reply could
not be copied.  Select an AI reply, copy it, and open Show Buffer: the copied
text must be in the clipboard.
"""
import os
import subprocess
import tempfile

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


# The two "Copy" commands a user reaches for.  ^C is the clipboard copy
# (e_edt_copy); ^K ^C is the WordStar Block-Copy (e_blck_copy), which in a
# read-only pane now also copies to the clipboard instead of doing nothing.
@pytest.mark.parametrize("copy_keys", [("\x03",), ("\x0b", "c")],
                         ids=["ctrl-c", "ctrl-k-c"])
def test_copy_ai_reply_to_clipboard(tmp_path, copy_keys):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "COPYME123"}
    with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        s.key(ALT.AI, delay=0.5); s.key("a", delay=0.6)
        s.key("hi", delay=0.3); s.key("\r", delay=1.5)

        row = col = None
        for i, r in enumerate(s.display()):
            c = r.find("COPYME123")
            if c >= 0:
                row, col = i, c
                break
        assert row is not None, "the mock reply did not appear in the pane"

        # Drag-select the reply word (SGR mouse -- the keyboard arrows belong to
        # the chat input), then copy, then Show Buffer.
        s.key("\033[<0;%d;%dM" % (col + 1, row + 1), delay=0.2)   # press at word start
        s.key("\033[<32;%d;%dM" % (col + 11, row + 1), delay=0.2)  # drag past word end
        s.key("\033[<0;%d;%dm" % (col + 11, row + 1), delay=0.3)   # release
        for k in copy_keys:
            s.key(k, delay=0.4)                                    # ^C or ^K ^C
        s.key("\033y", delay=0.8)                                  # Alt-Y -> Show Buffer
        disp = s.display()

    joined = "\n".join(disp)
    assert "Buffer" in joined, "Show Buffer did not open the clipboard viewer:\n" + joined
    # The clipboard viewer sits at the top; the copied text must be there (the
    # transcript copy stays near the bottom, so a top-rows hit is the clipboard).
    assert any("COPYME" in r for r in disp[:12]), \
        "the AI reply was not copied to the clipboard:\n" + joined
