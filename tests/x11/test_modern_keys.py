"""The Modern key table on the graphical display: F-keys and Ctrl-Shift chords.

The terminal rows (tests/test_modern_keys.py) send the bytes a terminal sends;
these press the keys on an X11 keyboard and read, from the editor's own trace
(XWPE_UI_TRACE, a file in the editor's start folder), which action the key
table gave the dispatcher.  Ctrl-Shift-letter is a key of its own here with no
protocol to ask for, and F11 and F12 are the keys F11 and F12 (the Sun keys
they also are under X do not quit or redo).  The Wayland twin runs this body
under its own fixture.
"""
import os

import pytest

from conftest import _sleep

TRACE = "trace.txt"
MODERN = {"XWPE_KEYS": "modern", "XWPE_UI_TRACE": TRACE}

# (the keys xdotool presses, the action the table must give)
KEYS = [
    ("F11", "Trace"),
    ("shift+F11", "Finish function"),
    ("F12", "Go to definition"),
    ("shift+F12", "References"),
    ("ctrl+shift+b", "Make"),
    ("ctrl+shift+z", "Redo"),
    ("ctrl+s", "Save"),
    ("ctrl+z", "Undo"),
    ("ctrl+Tab", "Next window"),
    ("ctrl+q", "Quit"),
]


def _actions(xwpe):
    path = os.path.join(os.path.dirname(xwpe.srcfile), TRACE)
    if not os.path.exists(path):
        return set()
    with open(path) as fh:
        return {line.strip().rsplit("action=", 1)[1] for line in fh if "action=" in line}


# @scenario SC-modern-keymap-11
@pytest.mark.parametrize("xwpe", [MODERN], indirect=True)
@pytest.mark.parametrize("keys, action", KEYS, ids=[k for k, _a in KEYS])
def test_the_modern_key_reaches_its_action_on_the_graphical_display(xwpe, keys, action):
    xwpe.key(keys)
    _sleep(0.5)
    assert action in _actions(xwpe), \
        "%s should reach %r, the table gave %s" % (keys, action, sorted(_actions(xwpe)))
