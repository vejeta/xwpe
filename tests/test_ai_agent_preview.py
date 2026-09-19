"""Agent write_file (ask policy) previews the change as a colored diff.

Instead of a blind "write_file x (N bytes)" prompt, the user sees WHAT will be
written: for an overwrite, a diff of the existing file against the new content,
with deleted lines red and added lines green, and a couple of context lines.
Approving with 'y' performs the write; the open file then shows the new content.
"""
import os
import time
import subprocess
import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

OLD = "int main(void)\n{\n    return 0;\n}\n"
NEWBODY = "int main(void)\n{\n    return 7;\n}\n"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"xwpe console editor" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _gutter_cell(s, glyph, keyword):
    for y, ln in enumerate(s.display()):
        if keyword in ln:
            for x in range(s.screen.columns):
                if s.screen.buffer[y][x].data == glyph:
                    return s.screen.buffer[y][x]
    return None


def test_agent_write_shows_colored_diff_then_applies(tmp_path):
    turns = "TOOL write_file t.c\n" + NEWBODY + "@@END@@TURN@@DONE done"
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_POLICY": "ask",
           "XWPE_AI_MOCK_REPLY": turns, "XWPE_AI_TRACE": str(tmp_path / "a.trace")}
    with WpeSession(str(tmp_path), OLD, env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g"); s._drain(0.5)
        s.key("rewrite main"); s.key("\r", delay=0.8)
        end = time.time() + 15
        while time.time() < end:
            s._drain(0.5)
            if "Write t.c" in "\n".join(s.display()):
                break
        disp = "\n".join(s.display())
        assert "Write t.c" in disp, "no write preview box:\n" + disp
        # the diff of the overwrite, with context
        minus = _gutter_cell(s, "-", "return 0;")
        plus = _gutter_cell(s, "+", "return 7;")
        assert minus is not None, "preview missing the removed line:\n" + disp
        assert plus is not None, "preview missing the added line:\n" + disp
        assert "int main(void)" in disp, "preview shows no context:\n" + disp
        if not (minus.bg == "default" and plus.bg == "default"):
            assert minus.bg == "red", "removed line not red: %s" % minus.bg
            assert plus.bg in ("green", "brightgreen"), "added line not green: %s" % plus.bg
        # allow the write, then let the agent finish
        s.key("y", delay=0.6); s._drain(1.2)
    # the write landed on disk, and the confirm was recorded as allowed
    assert "return 7;" in (tmp_path / "t.c").read_text(), "write not applied after y"
    txt = (tmp_path / "a.trace").read_text()
    assert "agent write confirm t.c -> allow" in txt, txt
