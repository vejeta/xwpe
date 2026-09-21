"""Lines that ask the user to act are drawn in a distinct colour.

Prompts in the AI pane that need the user's attention (approve a step, the
first-use notice, "type a follow-up") must not blend into the agent's ordinary
output -- they are marked and highlighted.  We check the rendered cell colour,
not just the text.
"""
import os
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


def _fg_of(s, needle):
    """Foreground colour of the first row containing `needle` (at its start)."""
    disp = s.display()
    for y, r in enumerate(disp):
        if needle in r:
            x = r.index(needle)
            return s.screen.buffer[y][x].fg
    return None


def test_attention_prompt_is_highlighted(tmp_path):
    # an agent run finishes and offers a follow-up -- an attention line -- among
    # its ordinary "[agent] ..." output lines.
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock",
           "XWPE_AI_MOCK_REPLY": "DONE ok"}
    with WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                    env_extra=env, filename="t.c") as s:
        s.key(ALT.AI); s.key("g")
        s.key("do it"); s.key("\r", delay=1.2)
        s._drain(1.0)
        attn_fg = _fg_of(s, ">> ")               # the follow-up prompt line
        normal_fg = _fg_of(s, "[agent] task:")   # ordinary agent output
    assert attn_fg is not None, "no attention (>>) line was shown"
    assert normal_fg is not None, "no ordinary agent output line was shown"
    assert attn_fg != normal_fg, \
        "the attention prompt is the same colour as normal output (fg=%r)" % attn_fg
    assert attn_fg == "red", \
        "the attention prompt should be red, got fg=%r" % attn_fg
