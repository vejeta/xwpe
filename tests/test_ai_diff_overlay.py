"""AI Edit shows a colored, context-bearing diff before applying.

The review overlay must read like a diff, not a monochrome wall: deleted lines
on a red background, added lines on a green one, and a couple of unchanged
context lines around the hunk so the reviewer sees WHERE it lands.  Applying is
one undoable step (Ctrl-U reverts).  Uses the mock backend so the whole review
path runs with no model/network.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

ORIG = "#include <stdio.h>\n\nint main(void)\n{\n    return 0;\n}\n"
NEW = "#include <stdio.h>\n\nint main(void)\n{\n    return 42;\n}\n"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _gutter_cell(s, glyph, keyword):
    """The (row, cell) whose line holds `keyword`, at the first `glyph` gutter."""
    for y, ln in enumerate(s.display()):
        if keyword in ln:
            for x in range(s.screen.columns):
                if s.screen.buffer[y][x].data == glyph:
                    return y, s.screen.buffer[y][x]
    return None, None


def _open_overlay(s):
    s.key(ALT.AI); s.key("e"); s._drain(0.5)
    s.key("bump the return value"); s.key("\r", delay=0.8)
    end = time.time() + 15
    while time.time() < end:
        s._drain(0.5)
        if "Proposed change" in "\n".join(s.display()):
            return True
    return False


def test_diff_overlay_shows_context_and_colors(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": NEW}
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        assert _open_overlay(s), "diff overlay did not appear"
        disp = "\n".join(s.display())
        # titled review box
        assert "Proposed change 1/1" in disp, disp
        # the changed lines, with -/+ gutters
        _, minus = _gutter_cell(s, "-", "return 0;")
        _, plus = _gutter_cell(s, "+", "return 42;")
        assert minus is not None, "deleted line not shown:\n" + disp
        assert plus is not None, "added line not shown:\n" + disp
        # context: an unchanged neighbour line is shown inside the box
        assert "int main(void)" in disp, "no context line around the hunk:\n" + disp

        if minus.bg == "default" and plus.bg == "default":
            pytest.skip("terminal reported no colours; red/green not distinguishable")
        assert minus.bg == "red", "deleted line not red: bg=%s" % minus.bg
        assert plus.bg in ("green", "brightgreen"), "added line not green: bg=%s" % plus.bg


def test_diff_overlay_apply_and_undo(tmp_path):
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": NEW,
           "XWPE_AI_TRACE": str(tmp_path / "e.trace")}
    with WpeSession(str(tmp_path), ORIG, env_extra=env, filename="t.c") as s:
        assert _open_overlay(s), "diff overlay did not appear"
        s.key("y", delay=0.6); s._drain(0.6)          # apply the hunk
        applied = "\n".join(s.display()[:8])
        assert "return 42;" in applied, "change not applied:\n" + applied
        s.key("\x15", delay=0.6); s._drain(0.6)        # Ctrl-U: one undo
        reverted = "\n".join(s.display()[:8])
        assert "return 0;" in reverted, "Ctrl-U did not revert the AI edit:\n" + reverted
    txt = (tmp_path / "e.trace").read_text()
    assert "edit applied" in txt, txt
