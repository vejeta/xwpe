"""Ctrl-A / Ctrl-E move the chat input caret to line start / end.

They must move the INSERTION point, not just the on-screen cursor: pressing
Ctrl-E used to move the cursor to the end visually while typing still inserted
where the caret had been, because the keys leaked to the editor and never
updated the chat input's own offset. They now mirror Home/End.
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


def test_ctrl_a_and_e_move_insertion_point(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "ok"}
    with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        s.key(ALT.AI, delay=0.5); s.key("a", delay=0.6)   # open chat input
        s.key("hello", delay=0.4)
        s.key("\x01", delay=0.3)                           # Ctrl-A -> line start
        s.key("A", delay=0.3)                              # insert at start -> Ahello
        s.key("\x05", delay=0.3)                           # Ctrl-E -> line end
        s.key("E", delay=0.3)                              # insert at end -> AhelloE
        row = next((r for r in s.display() if "hello" in r), "")

    assert "AhelloE" in row, \
        "Ctrl-A/Ctrl-E did not move the insertion point (got %r)" % row.strip()
