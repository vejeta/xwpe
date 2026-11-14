"""The bytes real terminals send, taken from docs/chapters/keys-delivery.txt.

Each row feeds the editor the exact bytes a terminal was measured to send for
a key (xterm's modifyOtherKeys form, Konsole's kitty form with its empty
shifted-key field and base-layout field, tmux's pass-through of the xterm
form) and asserts the editor reads the key the terminal meant: Ctrl-Z deletes
to the end of the line in the Borland style, and the report types nothing and
leaks nothing.
"""
import pytest

from wpe_driver import WpeSession

SEED = "abc\n"
ZED_FORMS = [
    ("legacy", "\x1a"),
    ("xterm modifyOtherKeys", "\x1b[27;5;122~"),
    ("tmux extended-keys", "\x1b[27;5;122~"),
    ("konsole kitty", "\x1b[122::46;5u"),
    ("kitty", "\x1b[122;5u"),
    ("kitty with alternates", "\x1b[122:90:121;5u"),
]


def _file_after(tmp_path, *keys):
    with WpeSession(str(tmp_path), SEED, filename="t.c") as w:
        w.key(*keys, delay=0.3)
        w.save()
        return w.text()


# @scenario SC-keymap-20
@pytest.mark.parametrize("form", [f for _n, f in ZED_FORMS],
                         ids=[n for n, _f in ZED_FORMS])
def test_ctrl_z_arrives_as_each_terminal_sends_it(tmp_path, form):
    """Ctrl-Z deletes to the end of the line; the report is that key whole."""
    assert _file_after(tmp_path, form) == "\n", "%r did not delete the line" % form


# @scenario SC-keymap-20
@pytest.mark.parametrize("form", [f for _n, f in ZED_FORMS[1:]],
                         ids=[n for n, _f in ZED_FORMS[1:]])
def test_a_measured_report_leaks_no_tail(tmp_path, form):
    """Typing Q after the report leaves only the Q: nothing of the report's
    tail (the ':' fields, the final byte) is typed."""
    assert _file_after(tmp_path, form, "Q") == "Q\n", \
        "%r left something typed" % form
