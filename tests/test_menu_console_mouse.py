"""A pseudo-terminal is not the console, whatever TERM says.

The editor used to choose the console-mouse path from the existence of a GPM
daemon alone, so on a machine where gpm serves the physical console every ssh,
tmux and screen session took that path too.  A submenu then paints and stops
dispatching keys: the menu asks for the mouse state, the console path waits for
an event this terminal cannot deliver, and nothing is read while it waits.  The
terminal's own mouse reports were never asked for, although they are what a pty
has.

TERM only decided this by accident, because the console connection is refused
for xterm-like values: the same pty works under xterm-256color and stalls under
screen-256color.  Both are asserted here so the test names the accident instead
of one value that happens to work.

Skipped when no GPM daemon is reachable: without it the console path is not
taken at all and there is nothing to prove.
"""
import os
import subprocess
import time

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN, WAIT_SCALE

SEED = "int main(void){return 0;}\n"
ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
ESC = "\033"
DIALOG_WAIT = 8.0 * WAIT_SCALE
KEY_WAIT = 6.0 * WAIT_SCALE
# A row of the Options submenu, spelled as the menu spells it, so its presence
# says the submenu is open and its absence says the submenu is gone.
SUBMENU_ROW = "ProGramming"
# Console-like TERM values: the ones that take the console path on a pty.
CONSOLE_TERMS = ["screen-256color", "tmux-256color"]
CONTROL_TERM = "xterm-256color"


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


def _gpm_reachable():
    return os.path.exists("/dev/gpmctl") or os.path.exists("/var/run/gpm.pid")


pytestmark = [
    pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai"),
    pytest.mark.skipif(not _gpm_reachable(),
                       reason="no GPM daemon reachable, so the console-mouse "
                              "path is not taken at all"),
]


def _screen(s):
    return "\n".join(s.display(drain=0))


def _wait_for(s, needle, limit, gone=False):
    """The screen once `needle` is there (or, with gone, once it is not), or the
    last screen seen when the limit passes."""
    deadline = time.time() + limit
    screen = ""
    while time.time() < deadline:
        s._drain(0.5)
        screen = _screen(s)
        if (needle not in screen) if gone else (needle in screen):
            return screen
    return screen


def _session(tmp_path, term):
    return WpeSession(str(tmp_path), SEED, cols=80, rows=30,
                      env_extra=dict(ENV, TERM=term))


def _open_ai_dialog(tmp_path, term):
    """Options > AI under one TERM; returns (screen, opened)."""
    with _session(tmp_path, term) as s:
        s.key(ALT.OPTIONS)
        s.key("i", delay=1.0)
        screen = _wait_for(s, "AI settings", DIALOG_WAIT)
        return screen, "AI settings" in screen


@pytest.mark.parametrize("term", CONSOLE_TERMS)
def test_the_ai_dialog_opens_on_a_pty_under_a_console_term(tmp_path, term):
    """SC-dialog-engine-45: the console path is not taken on a pty, so the
    submenu dispatches its accelerator and the dialog it names appears."""
    screen, opened = _open_ai_dialog(tmp_path, term)
    assert opened, (
        "Options > AI never painted its dialog under TERM=%s on a pty; the "
        "submenu is waiting for a console mouse event. Screen:\n%s"
        % (term, screen))


def test_the_ai_dialog_opens_under_xterm_as_the_control(tmp_path):
    """The same sequence on the same machine, under the TERM that happens to
    refuse the console connection.  If this one fails the test above proves
    nothing, so it is asserted beside it."""
    screen, opened = _open_ai_dialog(tmp_path, CONTROL_TERM)
    assert opened, (
        "the control case failed too, so this is not about TERM: the dialog did "
        "not open under %s. Screen:\n%s" % (CONTROL_TERM, screen))


@pytest.mark.parametrize("term", CONSOLE_TERMS)
def test_esc_closes_the_submenu_while_no_console_event_arrives(tmp_path, term):
    """SC-dialog-engine-46: no mouse poll may hold the keyboard.  Esc on an open
    submenu must close it even when no mouse event is ever going to arrive."""
    with _session(tmp_path, term) as s:
        s.key(ALT.OPTIONS)
        opened = _wait_for(s, SUBMENU_ROW, KEY_WAIT)
        assert SUBMENU_ROW in opened, (
            "the Options submenu never opened under TERM=%s; this test needs it "
            "open to prove the key is read. Screen:\n%s" % (term, opened))
        s.key(ESC)
        closed = _wait_for(s, SUBMENU_ROW, KEY_WAIT, gone=True)
        assert SUBMENU_ROW not in closed, (
            "Esc did not close the Options submenu under TERM=%s: the editor is "
            "still waiting on the console mouse and reads no key. Screen:\n%s"
            % (term, closed))
