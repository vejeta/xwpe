"""A pending AI edit must survive its target window being closed.

Alt-G e starts an edit that finishes from the fd-loop; until the reply lands,
a heartbeat repaints the "[AI edit] working" spinner through the target window
every ~120 ms.  Closing that window (Ctrl-W) while the edit is in flight used
to leave the spinner holding a freed window: SIGSEGV on a normal build, a
heap-use-after-free report under AddressSanitizer.

This starts an edit against the mock backend with a long reply delay, closes
the file window mid-wait and asserts that the editor stays up, that the edit
is dropped at close time ("edit target gone", never "edit applied"), that the
file on disk is untouched and that the editor still answers a key afterwards.

Red/green: before the fix the process dies within one heartbeat of the close
(exit -11 under wpe, the sanitizer report under wpe-asan); with it the trace
shows "edit target gone" and the editor keeps working.
"""
import os
import subprocess
import time
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, WAIT_SCALE

ORIG = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n"
NEW = "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 55;\n"
# The reply lands long after the window is closed.  Scaled like every wait of
# the driver, or on a slow runner the reply beats the close it must follow.
MOCK_DELAY_MS = int(3000 * WAIT_SCALE)
CTRL_W = "\x17"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _trace(path):
    return path.read_text() if path.exists() else ""


def _wait_trace(s, path, needle, tmo):
    end = time.time() + tmo
    while time.time() < end:
        s._drain(0.5)
        if needle in _trace(path):
            return True
    return False


# @scenario SC-ai-edit-14
def test_close_window_during_pending_edit(tmp_path):
    trace = tmp_path / "e.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": NEW,
        "XWPE_AI_MOCK_DELAY_MS": str(MOCK_DELAY_MS),
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("e"); s._drain(0.4)
        s.key("do it"); s.key("\r", delay=0.8)
        assert _wait_trace(s, trace, "edit stream fd=", 10), \
            "the edit never started:\n" + _trace(trace)

        # Close the file window while the spinner is still running on it.
        s.key(CTRL_W, delay=0.8)
        s._drain(1.0)                       # several heartbeats' worth
        assert s.alive(), \
            "wpe died after closing the edit's window (exit %r):\n%s" \
            % (s.exit_code(), "\n".join(s.display()))

        # The close drops the pending edit; nothing is ever applied.
        assert _wait_trace(s, trace, "edit target gone", 8), \
            "the pending edit was not dropped at close:\n" + _trace(trace)
        s._drain(MOCK_DELAY_MS / 1000.0 + 1.0)     # outlive the mock's reply delay
        txt = _trace(trace)
        assert "edit applied" not in txt, \
            "a reply for a closed window was APPLIED:\n" + txt
        assert s.text() == ORIG, "the file on disk changed:\n" + repr(s.text())

        # The editor is still interactive: a menu opens and closes on demand.
        s.key(ALT.FILE); s._drain(0.4)
        disp = s.display()
        assert any("New" in r for r in disp), \
            "the File menu did not open after the close:\n" + "\n".join(disp)
        s.key("\033"); s._drain(0.4)
        assert s.alive(), "wpe died while taking keys after the close"
