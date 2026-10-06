"""Moving a modal dialog from the keyboard.

Alt-F2 (the default key style) puts the option-dialog engine into move mode
(e_opt_move): the arrow keys shift the box one cell at a time, the box never
leaves the screen (columns 0..MAXSCOL-1, rows 1..MAXSLNS-2, clear of the menu
bar and the status line), and Enter or Esc leaves move mode with the focus
back on the widgets.

On the terminal, Alt-F2 is ESC followed by the F2 sequence (ESC O Q); both are
sent in one write so the ESC-Alt window catches the function key.

Driven on the AI settings dialog (Options > AI), 64x21 cells.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN, COLS, ROWS, incoherence


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
ALT_F2 = "\033\033OQ"
ALT_U = "\033u"
ENTER = "\r"
UP, DOWN, RIGHT, LEFT = "\033[A", "\033[B", "\033[C", "\033[D"
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as l (top-left), k (top-right), m (bottom-left).
CORNER_TL, CORNER_TR, CORNER_BL = "l", "k", "m"
FAR = 100            # more arrow presses than the screen has cells


def _open_ai_settings(tmp_path):
    s = WpeSession(str(tmp_path), SEED, env_extra=ENV)
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.7)
    return s


def _title_pos(s):
    for i, row in enumerate(s.display()):
        c = row.find(TITLE)
        if c >= 0:
            return (i, c)
    return None


def _frame(s):
    """(top, bottom, left, right) rows/cols of the dialog frame, from the
    title row: the corners around the title and the bottom-left corner below."""
    rows = s.display()
    pos = _title_pos(s)
    assert pos is not None, "no dialog title on screen:\n" + "\n".join(rows)
    top, at = pos
    left = rows[top].rfind(CORNER_TL, 0, at)
    right = rows[top].find(CORNER_TR, at)
    bottom = next((i for i in range(top + 1, len(rows)) if rows[i][left] == CORNER_BL), -1)
    assert left >= 0 and right > left and bottom > top, \
        "dialog frame not intact (%d,%d,%d,%d):\n%s" % (top, bottom, left, right, "\n".join(rows))
    return top, bottom, left, right


def _row_with(s, needle):
    return next((r for r in s.display() if needle in r), "")


def _arrows(s, key, times):
    """Hold an arrow key: one write of `times` presses, one drain."""
    s.key(key * times, delay=1.0)


# @scenario SC-dialog-engine-08
def test_keyboard_move_moves_and_restores_focus(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        opened = _title_pos(s)
        assert opened is not None, "AI settings dialog did not open"
        s.key(ALT_F2, delay=0.6)
        _arrows(s, RIGHT, 3)
        _arrows(s, DOWN, 2)
        s.key(ENTER, delay=0.6)
        moved = _title_pos(s)
        s.key(ALT_U, delay=0.4)             # focus the Endpoint field
        s.key("XYZ", delay=0.4)
        endpoint = _row_with(s, "Endpoint")
    assert moved == (opened[0] + 2, opened[1] + 3), \
        "Right x3 / Down x2 did not move the title from %r to %r: %r" % (
            opened, (opened[0] + 2, opened[1] + 3), moved)
    assert "XYZ" in endpoint, \
        "typing after the move did not reach the Endpoint field: %r" % endpoint


# @scenario SC-dialog-engine-08
@incoherence("leaving move mode with Enter toggles the Enable checkbox that had the "
             "focus: e_opt_kst_run re-enters the checkbox with sw == 0 and c == cold, "
             "which its handler reads as a click")
def test_keyboard_move_does_not_toggle_focused_widget(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        before = _row_with(s, "Enable AI")
        s.key(ALT_F2, delay=0.6)
        _arrows(s, RIGHT, 1)
        s.key(ENTER, delay=0.6)
        after = _row_with(s, "Enable AI")
    assert before.strip() == after.strip(), \
        "the move flipped the focused checkbox:\nBEFORE: %r\nAFTER:  %r" % (before, after)


# @scenario SC-dialog-engine-09
def test_keyboard_move_clamps_to_screen(tmp_path):
    with _open_ai_settings(tmp_path) as s:
        s.key(ALT_F2, delay=0.6)
        _arrows(s, LEFT, FAR)
        left_rows = s.display()
        _, _, left, _ = _frame(s)
        assert left == 0, \
            "Left x%d did not stop at column 0 (left border at %d):\n%s" % (
                FAR, left, "\n".join(left_rows))
        _arrows(s, RIGHT, FAR)
        _, _, _, right = _frame(s)
        assert right == COLS - 1, \
            "Right x%d did not stop at the last column (right border at %d):\n%s" % (
                FAR, right, "\n".join(s.display()))
        _arrows(s, DOWN, FAR)
        _, bottom, _, _ = _frame(s)
        assert bottom == ROWS - 2, \
            "Down x%d did not stop above the status line (bottom at %d):\n%s" % (
                FAR, bottom, "\n".join(s.display()))
        _arrows(s, UP, FAR)
        top, _, _, _ = _frame(s)
        assert top == 1, \
            "Up x%d did not stop below the menu bar (top at %d):\n%s" % (
                FAR, top, "\n".join(s.display()))
        s.key(ENTER, delay=0.6)
        assert s.alive() and _title_pos(s) is not None, \
            "the dialog did not survive the clamped moves"
