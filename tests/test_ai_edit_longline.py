"""Editing a long line an AI rewrite applied must not crash the editor.

xwpe is a WordPerfect-style editor: every physical line lives in an mx.x+1 byte
buffer and holds at most mx.x-1 columns, because the file reader soft-wraps
anything longer at the margin.  The whole-buffer rebuild path
(e_buffer_set_text / e_buffer_append_line, used by an AI/LSP/Undo apply) used to
store an over-long line verbatim in a content-sized buffer; the first keystroke
on it then drove the insert/auto-wrap machinery to write past the line and
corrupt the heap -- a hard SIGSEGV.  The rebuild path now soft-wraps like the
file reader, so the applied line is legal and editable.

Red/green: with the pre-fix rebuild the child SIGSEGVs on the first keystroke
(s.alive() -> False); with the fix it survives.
"""
import os
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"precise code editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

# A single logical line well over MAXCOLUM (120), made of real words so it wraps
# at word boundaries -- exactly the shape of the markdown the assistant emits.
LONG = " ".join("palabra%02d" % i for i in range(24))          # ~210 columns
NEWFILE = LONG + "\nsecond short line\n"


def test_typing_on_applied_long_line_does_not_crash(tmp_path):
    trace = tmp_path / "ai.trace"
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": NEWFILE,
        "XWPE_AI_TRACE": str(trace),
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("e")                       # Edit
        s.key("rewrite it")
        s.key("\r", delay=1.3)           # generate -> per-hunk preview
        s._drain(1.0)
        s.key("y", delay=1.0)            # accept -> apply the long line to buffer
        s._drain(0.6)
        assert s.alive(), "editor died applying the long line"
        # Type on the applied long line: pre-fix this SIGSEGVs in e_ins_nchar.
        s.key("Hello there ", delay=0.6)
        s.key(" ", " ", " ", delay=0.3)
        s._drain(0.5)
        assert s.alive(), "editor crashed typing on the AI-applied long line"


def test_applied_long_line_saves_back_rejoined(tmp_path):
    # The soft-wrap is invisible on save: e_write drops the soft breaks, so the
    # file round-trips the long logical line rather than gaining hard newlines.
    env = {
        "XWPE_AI_ENABLE": "1",
        "XWPE_AI_BACKEND": "mock",
        "XWPE_AI_MOCK_REPLY": NEWFILE,
    }
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env) as s:
        s.key(ALT.AI)
        s.key("e")
        s.key("rewrite it")
        s.key("\r", delay=1.3)
        s._drain(1.0)
        s.key("y", delay=1.0)
        s._drain(0.6)
        assert s.alive()
        s.save()
        disk = s.text()
    assert LONG in disk, \
        "the long logical line was not rejoined on save:\n" + repr(disk)
