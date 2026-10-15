"""Options -> AI: the Web access checkbox, beside "Enable AI assistant".

The switch decides whether anything the assistant does leaves this machine, so
it is on the dialog's first row rather than behind the Advanced button.  The
box shows the saved `AIWeb`, Ok writes what the box says, and Esc leaves the
file as it was.  Clearing a box that was never answered is not a refusal: the
chat's first-use notice still gets its one chance to ask, so only a box that
was ticked turns the switch off.
"""
import os
import subprocess

import pytest
from wpe_driver import WpeSession, ALT, WPE_BIN

SEED = "int main(void){return 0;}\n"
BOX_ON = "[X] Web access"
BOX_OFF = "[ ] Web access"
OK = "\033o"                      # Alt-O: the dialog's Ok
TOGGLE = "\033w"                  # Alt-W: the Web access box
ESC = "\033"
ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
# The dialog is kept within twenty rows so a 24-line terminal shows its Ok.
SMALL_ROWS = 24
SMALL_COLS = 80


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")


def _rc_path(tmp_path):
    return tmp_path / ".config" / "xwpe" / "xwperc"


def _seed_rc(tmp_path, lines):
    """Write a [Programming] section into the test HOME's xwperc; the driver
    points HOME at the workdir, so this is what the editor reads."""
    cfg = tmp_path / ".config" / "xwpe"
    cfg.mkdir(parents=True, exist_ok=True)
    (cfg / "xwperc").write_text("[Programming]\n" + "".join(l + "\n" for l in lines))


def _rc_web(tmp_path):
    """The AIWeb lines xwperc carries, [] when it names none."""
    p = _rc_path(tmp_path)
    if not p.exists():
        return []
    return [l.strip() for l in p.read_text().splitlines() if l.strip().startswith("AIWeb")]


def _open_dialog(s):
    s.key(ALT.OPTIONS)
    s.key("i", delay=1.0)
    s._drain(0.5)
    screen = "\n".join(s.display(drain=0))
    assert "AI settings" in screen, screen
    return screen


def _box_row(s):
    for r in s.display(drain=0):
        if "Web access" in r:
            return r
    return ""


# @scenario SC-ai-options-55
def test_checkbox_shows_the_saved_state_and_ok_writes_it(tmp_path):
    """Saved on: the box is ticked; clearing it and pressing Ok saves off."""
    _seed_rc(tmp_path, ["AIWeb : on"])
    with WpeSession(str(tmp_path), SEED, env_extra=ENV) as s:
        _open_dialog(s)
        shown = _box_row(s)
        s.key(TOGGLE, delay=0.4)
        s._drain(0.3)
        cleared = _box_row(s)
        s.key(OK, delay=1.2)
        s._drain(1.0)
    assert BOX_ON in shown, "the box does not show the saved AIWeb : on: %r" % shown
    assert BOX_OFF in cleared, "Alt-W did not clear the box: %r" % cleared
    assert _rc_web(tmp_path) == ["AIWeb : off"], \
        "Ok did not save the cleared box: %r" % _rc_web(tmp_path)


# @scenario SC-ai-options-56
def test_esc_leaves_the_switch_as_it_was(tmp_path):
    """Toggling the box and leaving with Esc changes nothing on disk."""
    _seed_rc(tmp_path, ["AIWeb : on"])
    before = _rc_path(tmp_path).read_bytes()
    with WpeSession(str(tmp_path), SEED, env_extra=ENV) as s:
        _open_dialog(s)
        s.key(TOGGLE, delay=0.4)
        s._drain(0.3)
        s.key(ESC, delay=1.0)
        s._drain(0.8)
    assert _rc_path(tmp_path).read_bytes() == before, \
        "Esc wrote to xwperc: %r" % _rc_path(tmp_path).read_text()


# @scenario SC-ai-options-55
def test_an_unanswered_switch_survives_ok(tmp_path):
    """Ok on an untouched, never-answered box leaves it unanswered, so the
    chat's first-use notice still gets its chance to ask."""
    _seed_rc(tmp_path, ["AIPolicy : ask"])
    with WpeSession(str(tmp_path), SEED, env_extra=ENV) as s:
        _open_dialog(s)
        shown = _box_row(s)
        s.key(OK, delay=1.2)
        s._drain(1.0)
    assert BOX_OFF in shown, "an unanswered switch should show an empty box: %r" % shown
    assert _rc_web(tmp_path) == [], \
        "Ok answered the switch the user never touched: %r" % _rc_web(tmp_path)


# @scenario SC-ai-options-57
def test_the_dialog_still_fits_a_24_line_terminal(tmp_path):
    """The Ok button and the box are both on screen at 80x24."""
    _seed_rc(tmp_path, ["AIWeb : on"])
    with WpeSession(str(tmp_path), SEED, env_extra=ENV,
                    cols=SMALL_COLS, rows=SMALL_ROWS) as s:
        screen = _open_dialog(s)
        box = _box_row(s)
    assert "Web access" in box, "the box is off screen at 80x24:\n" + screen
    assert "Ok" in screen, "the Ok button is off screen at 80x24:\n" + screen
