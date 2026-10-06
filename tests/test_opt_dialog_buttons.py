"""Only mouse button 1 acts inside a modal dialog.

The option-dialog engine's mouse layer (e_opt_mouse) looks at which button a
report names: a button-1 press on a widget activates it, on the drawn close
box closes the dialog, elsewhere on the title row drags it.  A scroll-wheel
step and the middle and right buttons do nothing at all, so rolling the wheel
over "[ ] Enable" does not toggle it and a right click on the close box does
not dismiss the box.

Events are injected as SGR mouse reports: ESC [ < b ; col ; row M (press) and
... m (release), 1-based coordinates; b = 0 button 1, 1 button 2, 2 button 3,
64 wheel up, 65 wheel down, 32 added for motion with the button held.

Driven on the AI settings dialog (Options > AI), 64x21 cells.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN


def _ai_build():
    try:
        out = subprocess.run(["strings", os.path.abspath(WPE_BIN)],
                             stdout=subprocess.PIPE, timeout=30).stdout
        return b"AI settings" in out
    except Exception:
        return False


pytestmark = pytest.mark.skipif(not _ai_build(), reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock"}
SEED = "int main(void){return 0;}\n"
TITLE = "AI settings"
ESC = "\033"
CLOSE_GLYPH = "✕"
ENABLE_LABEL = "Enable AI"
RADIO_LABELS = ("Ollama", "Claude CLI")   # Backend radios; one is selected
ALT_U = "\033u"                  # focus the Endpoint text field
SGR_BUTTON1, SGR_BUTTON2, SGR_BUTTON3 = 0, 1, 2
SGR_WHEEL_UP, SGR_WHEEL_DOWN = 64, 65
SGR_MOTION = 32
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as l (top-left), k (top-right).
CORNER_TL, CORNER_TR = "l", "k"
DRAG_COLS = 4


def _open_ai_settings(tmp_path):
    s = WpeSession(str(tmp_path), SEED, env_extra=ENV)
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.7)
    return s


def _sgr(button, col, row, final):
    return "\033[<%d;%d;%d%s" % (button, col + 1, row + 1, final)


def _press_release(s, button, col, row):
    """Press and release `button` on the 0-based cell (col, row)."""
    s.key(_sgr(button, col, row, "M"), delay=0.2)
    s.key(_sgr(button, col, row, "m"), delay=0.5)


def _wheel(s, direction, col, row):
    """One wheel step (SGR_WHEEL_UP / SGR_WHEEL_DOWN) over the 0-based cell
    (col, row).  One step at a time: two steps over a checkbox that reacts to
    the wheel would flip it twice and look untouched."""
    s.key(_sgr(direction, col, row, "M"), delay=0.4)


def _is_open(s):
    return any(TITLE in row for row in s.display())


def _title_pos(s):
    for i, row in enumerate(s.display()):
        c = row.find(TITLE)
        if c >= 0:
            return (i, c)
    return None


def _label_pos(s, label):
    """(row, col) of the first cell of `label` on the screen."""
    for i, row in enumerate(s.display()):
        c = row.find(label)
        if c >= 0:
            return (i, c)
    raise AssertionError("%r not on screen:\n%s" % (label, "\n".join(s.display())))


def _mark(s, label):
    """The "[ ]" / "( )" glyph (with its X or *) drawn in front of a label."""
    row, col = _label_pos(s, label)
    return s.display()[row][col - 4:col - 1]


def _close_box(s):
    top, _ = _title_pos(s)
    col = s.display()[top].find(CLOSE_GLYPH)
    assert col >= 0, "no close glyph in the title row:\n" + "\n".join(s.display())
    return (col, top)


def _unselected_radio(s):
    """A Backend radio label that is not the selected one (which backend is
    selected depends on the configured backend)."""
    for label in RADIO_LABELS:
        if _mark(s, label) == "( )":
            return label
    raise AssertionError("every backend radio is marked selected:\n" + "\n".join(s.display()))


# @scenario SC-dialog-engine-42
@pytest.mark.parametrize("direction", [SGR_WHEEL_UP, SGR_WHEEL_DOWN], ids=["up", "down"])
def test_wheel_over_checkbox_does_not_toggle(tmp_path, direction):
    with _open_ai_settings(tmp_path) as s:
        row, col = _label_pos(s, ENABLE_LABEL)
        before = _mark(s, ENABLE_LABEL)
        _wheel(s, direction, col + 2, row)
        after = _mark(s, ENABLE_LABEL)
        still_open = _is_open(s)
        s.key(ESC, delay=0.6)
    assert still_open, "a wheel step closed the dialog"
    assert before == after, \
        "a wheel step over the checkbox flipped it: %r -> %r" % (before, after)


# @scenario SC-dialog-engine-42
def test_wheel_over_radio_does_not_select(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        label = _unselected_radio(s)
        row, col = _label_pos(s, label)
        _wheel(s, SGR_WHEEL_DOWN, col + 2, row)
        after = _mark(s, label)
        s.key(ESC, delay=0.6)
    assert after == "( )", \
        "a wheel step over the %r radio selected it: %r" % (label, after)


# @scenario SC-dialog-engine-43
@pytest.mark.parametrize("button", [SGR_BUTTON2, SGR_BUTTON3], ids=["button2", "button3"])
def test_other_buttons_over_checkbox_do_not_toggle(tmp_path, button):
    with _open_ai_settings(tmp_path) as s:
        row, col = _label_pos(s, ENABLE_LABEL)
        before = _mark(s, ENABLE_LABEL)
        _press_release(s, button, col + 2, row)
        after = _mark(s, ENABLE_LABEL)
        s.key(ESC, delay=0.6)
    assert before == after, \
        "button %d over the checkbox flipped it: %r -> %r" % (button + 1, before, after)


# @scenario SC-dialog-engine-43
@pytest.mark.parametrize("button", [SGR_BUTTON2, SGR_BUTTON3], ids=["button2", "button3"])
def test_other_buttons_on_close_box_do_not_close(tmp_path, button):
    # With the Endpoint text field focused a button-1 click on the close box
    # dismisses the dialog outright; buttons 2 and 3 there must not.
    with _open_ai_settings(tmp_path) as s:
        s.key(ALT_U, delay=0.4)
        col, row = _close_box(s)
        _press_release(s, button, col, row)
        still_open = _is_open(s)
        rows = s.display()
        s.key(ESC, delay=0.6)
    assert still_open, \
        "button %d on the close box closed the dialog:\n%s" % (button + 1, "\n".join(rows))


# @scenario SC-dialog-engine-43
def test_button2_on_title_row_does_not_drag(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        opened = _title_pos(s)
        assert opened is not None, "AI settings dialog did not open"
        top, at = opened
        grab = at + len(TITLE) + 2            # title row, right of the title text
        s.key(_sgr(SGR_BUTTON2, grab, top, "M"), delay=0.3)
        s.key(_sgr(SGR_BUTTON2 + SGR_MOTION, grab + DRAG_COLS, top, "M"), delay=0.3)
        s.key(_sgr(SGR_BUTTON2, grab + DRAG_COLS, top, "m"), delay=0.5)
        moved = _title_pos(s)
        s.key(ESC, delay=0.6)
    assert moved == opened, \
        "a button-2 drag on the title row moved the dialog from %r to %r" % (opened, moved)


# @scenario SC-dialog-engine-07
def test_title_grab_near_left_corner_drags_not_closes(tmp_path):
    # The third cell from the left border is where the classic Borland close
    # box used to be; nothing is drawn there, so grabbing it drags the box.
    # The Endpoint field is focused first: there a close request would
    # dismiss the dialog at once, so a hidden close cell cannot hide.
    with _open_ai_settings(tmp_path) as s:
        s.key(ALT_U, delay=0.4)
        opened = _title_pos(s)
        assert opened is not None, "AI settings dialog did not open"
        top, at = opened
        left = s.display()[top].rfind(CORNER_TL, 0, at)
        assert left >= 0, "no top-left corner on the title row:\n" + "\n".join(s.display())
        grab = left + 3
        s.key(_sgr(SGR_BUTTON1, grab, top, "M"), delay=0.3)
        s.key(_sgr(SGR_BUTTON1 + SGR_MOTION, grab + DRAG_COLS, top, "M"), delay=0.3)
        s.key(_sgr(SGR_BUTTON1, grab + DRAG_COLS, top, "m"), delay=0.5)
        still_open = _is_open(s)
        moved = _title_pos(s)
        rows = s.display()
        s.key(ESC, delay=0.6)
    assert still_open, \
        "grabbing the title row at column xa+3 closed the dialog:\n" + "\n".join(rows)
    assert moved == (top, at + DRAG_COLS), \
        "the grab at xa+3 did not drag the title from %r to %r: %r" % (
            opened, (top, at + DRAG_COLS), moved)
