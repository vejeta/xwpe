"""Agent write_file: the open file refreshes, and the changeset lists it once.

Two regressions this covers:
- After the agent writes a file that is open in a window, the editor showed the
  OLD contents (the buffer was never reloaded), so the change was invisible.
- The end-of-run changeset listed the same file twice with an inconsistent path
  (a doubled slash from a scope-list join).

Unattended (auto) policy so the write needs no approval and the changeset review
is produced; mock backend so no model/network is involved.
"""
import os
import time
import subprocess
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


def test_agent_write_refreshes_open_file_and_lists_once(tmp_path):
    turns = ("TOOL write_file t.c\nint main(void){return 7;}\n@@END"
             "@@TURN@@DONE changed t.c")
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "auto",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("change the return value in t.c"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            t = (tmp_path / "a.trace").read_text() if (tmp_path / "a.trace").exists() else ""
            if "changeset" in t:
                break
        disp = "\n".join(s.display())
        # A0: the open buffer shows the agent's new content, not the stale text
        assert "return 7;" in disp, "open file was not refreshed after write:\n" + disp
        # A1: the file is listed exactly once, with a clean path (no doubled slash)
        assert "1 change" in disp, "changeset did not list the write once:\n" + disp
        assert "//t.c" not in disp, "changeset path has a doubled slash:\n" + disp
        s.key("a", delay=0.5)                     # keep all -> leave the review
