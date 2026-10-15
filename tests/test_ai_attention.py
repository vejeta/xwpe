"""Lines that ask the user to act are drawn in a distinct colour.

Prompts in the AI pane that need the user's attention (approve a step, the
first-use notice, "type a follow-up") must not blend into the agent's ordinary
output -- they are marked and highlighted.  We check the rendered cell colour,
not just the text.

The expected colour used to be red (the first highlight, painted over the
pane after the fact).  Since the pane colours each line by its role from the
colour scheme (SC-ai-chat-36), the attention entry's default is white on green
(scheme key `aia`, 15 on 2), so the test checks that foreground and background
pair.
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


ATTENTION_FG = "white"    # scheme entry `aia`: 15 (white, bold) ...
ATTENTION_BG = "green"    # ... on 2 (green)


def _cell_of(s, needle):
    """The cell at the start of the first row containing `needle`."""
    disp = s.display()
    for y, r in enumerate(disp):
        if needle in r:
            x = r.index(needle)
            return s.screen.buffer[y][x]
    return None


# @scenario SC-ai-chat-36
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
        attn = _cell_of(s, ">> ")               # the follow-up prompt line
        normal = _cell_of(s, "[agent] task:")   # ordinary agent output
    assert attn is not None, "no attention (>>) line was shown"
    assert normal is not None, "no ordinary agent output line was shown"
    attn_fg, attn_bg = attn.fg, attn.bg
    normal_fg, normal_bg = normal.fg, normal.bg
    assert (attn_fg, attn_bg) != (normal_fg, normal_bg), \
        "the attention prompt is the same colour as normal output (fg=%r bg=%r)" \
        % (attn_fg, attn_bg)
    assert attn_fg == ATTENTION_FG, \
        "the attention prompt should be white, got fg=%r" % attn_fg
    assert attn_bg == ATTENTION_BG, \
        "the attention prompt should be on green, got bg=%r" % attn_bg
