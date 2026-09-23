"""A dragged dialog keeps its position across an in-place repaint.

The model/provider pickers (Alt-M / Alt-V) return -1 so the option engine
repaints the dialog in place.  That repaint used to re-centre the dialog, so a
box the user had dragged elsewhere jumped back to the centre.  It must now stay
where it was moved.
"""
import os
import tempfile

from wpe_driver import WpeSession


def _title_pos(s):
    for i, row in enumerate(s.display()):
        c = row.find("AI settings")
        if c >= 0:
            return (i, c)
    return None


def test_dialog_keeps_dragged_position_after_model_picker(tmp_path):
    home = tempfile.mkdtemp()
    os.makedirs(os.path.join(home, ".config", "xwpe"), exist_ok=True)
    env = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "HOME": home}
    with WpeSession(tempfile.mkdtemp(), "int main(void){return 0;}\n",
                    env_extra=env, filename="a.c") as s:
        s.key("\033o", delay=0.5); s.key("i", delay=0.7)   # Options > AI
        opened = _title_pos(s)
        assert opened is not None, "AI settings dialog did not open"

        # Drag the title bar (0-based row 1 -> SGR row 2) down and to the right.
        s.key("\033[<0;31;2M", delay=0.3)                  # press
        s.key("\033[<32;38;5M", delay=0.2)                 # motion
        s.key("\033[<32;45;7M", delay=0.2)                 # motion
        s.key("\033[<0;45;7m", delay=0.4)                  # release
        dragged = _title_pos(s)
        assert dragged is not None and dragged != opened, \
            "the drag did not move the dialog: opened=%r dragged=%r" % (opened, dragged)

        # Alt-M opens the model picker (repaints the dialog in place); Esc closes it.
        s.key("\033m", delay=0.8)
        s.key("\033", delay=0.6)
        after = _title_pos(s)

    assert after == dragged, \
        "the dialog moved on the Alt-M repaint: dragged=%r after=%r" % (dragged, after)
    assert after != opened, \
        "the dialog jumped back to its original centred position: %r" % (after,)
