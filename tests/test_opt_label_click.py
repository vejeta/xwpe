"""Clicking a radio/checkbox LABEL must select it, not only the tiny glyph.

Buttons already hit-test their whole label; radios and checkboxes used to react
only to a click on the 3-cell "( )" / "[ ]" glyph, so a user clicking the text
next to it saw nothing happen.  The hit region now spans the glyph plus the
label.  Driven on the AI settings dialog, which has both.
"""
import os
import tempfile

from wpe_driver import WpeSession


def _click(s, col, row):
    s.key("\033[<0;%d;%dM" % (col, row), delay=0.2)
    s.key("\033[<0;%d;%dm" % (col, row), delay=0.4)


def _open_ai_settings(tmp_path):
    home = tempfile.mkdtemp()
    os.makedirs(os.path.join(home, ".config", "xwpe"), exist_ok=True)
    return WpeSession(str(tmp_path), "int main(void){return 0;}\n",
                      env_extra={"XWPE_AI_ENABLE": "1", "HOME": home})


def test_radio_label_click_selects(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.7)
        # Default backend is Ollama.  Click the LABEL "Claude CLI (login)" (its
        # text, well past the glyph) on 0-based row 6 -> SGR row 7, col ~20.
        _click(s, 21, 7)
        disp = "\n".join(s.display())
    assert "(*) Claude CLI" in disp, \
        "clicking the radio label did not select it:\n" + disp
    assert "(*) Ollama" not in disp, \
        "the previously selected radio was not cleared:\n" + disp


def test_checkbox_label_click_toggles(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.7)
        before = "\n".join(s.display())
        # "[ ] Enable AI assistant" on 0-based row 3 -> SGR row 4; click the
        # label text (col ~20 -> SGR 21), not the glyph.
        _click(s, 21, 4)
        after = "\n".join(s.display())
    # The Enable box must have flipped (X <-> blank) from the label click.
    b_on = "[X] Enable AI" in before
    a_on = "[X] Enable AI" in after
    assert b_on != a_on, \
        "clicking the checkbox label did not toggle it:\nBEFORE:\n%s\nAFTER:\n%s" % (before, after)
