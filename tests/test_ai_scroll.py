"""AI pane -- browse the transcript with the keyboard (deterministic mock).

The chat/agent pane pins its "> " input row at the bottom while you type.  When
the conversation grows past the pane, you must be able to read back the history:
PgUp/PgDn page through it and the arrows scroll it a line at a time, while typing
any character snaps the view back to the prompt so the next key lands in the
input, not lost against a scrolled-away view.

Red/green: before the fix the pane always re-pinned its view to the input caret
(a focused window follows its caret via e_cursor), so PgUp/arrows did nothing --
the oldest message stayed scrolled off no matter how far you paged up.  This test
fails on that behavior: `m0` never becomes visible.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

CUP = "\033[A"
CDO = "\033[B"
PGUP = "\033[5~"
PGDN = "\033[6~"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _fill_chat(s, n=9):
    """Send n short chat turns so the transcript overflows the pane."""
    s.key(ALT.AI)
    s.key("a")
    s._drain(0.4)
    for i in range(n):
        s.key("m%d" % i)
        s.key("\r", delay=0.3)
    s._drain(0.5)


def test_pgup_reveals_scrolled_off_history(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "reply"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        _fill_chat(s)
        bottom = "\n".join(s.display())
        assert "You: m0" not in bottom, \
            "the first message should have scrolled off the bottom view:\n" + bottom
        # page up until the oldest message is on screen
        seen = False
        for _ in range(4):
            s.key(PGUP); s._drain(0.2)
            if "You: m0" in "\n".join(s.display()):
                seen = True
                break
        assert seen, \
            "PgUp did not reveal the oldest message -- the view never scrolled:\n" \
            + "\n".join(s.display())


def test_arrow_scrolls_transcript_one_line(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "reply"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        _fill_chat(s)

        def top_line():
            disp = s.display()
            for i, r in enumerate(disp):
                if " AI " in r and ("q" in r or "-" in r):
                    return disp[i + 1].strip()
            return ""

        before = top_line()
        s.key(CUP); s._drain(0.2)
        after = top_line()
        assert after != before, \
            "Up arrow did not scroll the transcript (top line unchanged: %r)" % before


def test_typing_snaps_back_to_input(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "reply"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        _fill_chat(s)
        # scroll up to browse, then type -- the view must return to the prompt
        s.key(PGUP); s._drain(0.2)
        s.key(PGUP); s._drain(0.2)
        s.key("SNAPBACK"); s._drain(0.3)
        disp = "\n".join(s.display())
        assert "SNAPBACK" in disp, \
            "typing after a scroll did not reach the input row:\n" + disp
        # the newest turn is visible again (snapped back to the bottom)
        assert "You: m8" in disp, \
            "the view did not snap back to the prompt after typing:\n" + disp
