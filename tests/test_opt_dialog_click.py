"""Mouse clicks and Enter on a modal dialog's buttons and frame.

The option-dialog engine (e_opt_mouse / e_get_opt_sw) hit-tests a button on
every cell of its label, treats a click outside the frame or on the left and
bottom borders as inert, closes the box from the close glyph in the title row
as if Esc had been pressed, and fires the focused button on Enter.

Clicks are injected as SGR mouse reports (press ESC [ < 0 ; col ; row M,
release ... m, 1-based coordinates).  Whether Ok was applied is read from the
AI trace: Ok writes an "options backend=..." line, Cancel writes nothing.

Driven on the AI settings dialog (Options > AI), 64x21 cells.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN, incoherence


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
ENTER = "\r"
TAB = "\t"
ALT_U = "\033u"              # focus the Endpoint text field
F2_SAVE = "\033OQ"
CLOSE_GLYPH = "✕"       # the close box drawn in the title row
OK_LABEL = "  Ok  "          # the button header, spaces included
CANCEL_LABEL = "Cancel"
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as l (top-left), k (top-right), m (bottom-left).
CORNER_TL, CORNER_TR, CORNER_BL = "l", "k", "m"
MAX_TABS = 20                # more Tab presses than the dialog has widgets


def _open_ai_settings(tmp_path, trace):
    env = dict(ENV, XWPE_AI_TRACE=str(trace))
    s = WpeSession(str(tmp_path), SEED, env_extra=env)
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.7)
    return s


def _click(s, col, row):
    """Press and release button 1 on the 0-based cell (col, row)."""
    s.key("\033[<0;%d;%dM" % (col + 1, row + 1), delay=0.2)
    s.key("\033[<0;%d;%dm" % (col + 1, row + 1), delay=0.5)


def _is_open(s):
    return any(TITLE in row for row in s.display())


def _frame(s):
    """(top, bottom, left, right) rows/cols of the dialog frame, from the
    title row: the corners around the title and the bottom-left corner below."""
    rows = s.display()
    top = next((i for i, r in enumerate(rows) if TITLE in r), None)
    assert top is not None, "no dialog title on screen:\n" + "\n".join(rows)
    at = rows[top].index(TITLE)
    left = rows[top].rfind(CORNER_TL, 0, at)
    right = rows[top].find(CORNER_TR, at)
    bottom = next((i for i in range(top + 1, len(rows)) if rows[i][left] == CORNER_BL), -1)
    assert left >= 0 and right > left and bottom > top, \
        "dialog frame not intact (%d,%d,%d,%d):\n%s" % (top, bottom, left, right, "\n".join(rows))
    return top, bottom, left, right


def _label_pos(s, label):
    """(row, col) of the first cell of `label` on the screen."""
    for i, row in enumerate(s.display()):
        c = row.find(label)
        if c >= 0:
            return (i, c)
    raise AssertionError("%r not on screen:\n%s" % (label, "\n".join(s.display())))


def _options_applied(trace):
    text = trace.read_text() if trace.exists() else ""
    return any(line.startswith("options ") for line in text.splitlines())


def _dismiss(s):
    """Esc closes the dialog.  From the Enable checkbox, which has the initial
    focus, the engine spends the first Esc moving the focus to Cancel; press it
    again when the box is still up."""
    s.key(ESC, delay=0.6)
    if _is_open(s):
        s.key(ESC, delay=0.6)


def _tab_to_cancel(s):
    """Tab until the Cancel label is drawn in the focused colour: its
    background changes from the unfocused one it has when the dialog opens."""
    row, col = _label_pos(s, CANCEL_LABEL)
    unfocused = s.cell_bg(row, col)
    for _ in range(MAX_TABS):
        s.key(TAB, delay=0.25)
        if s.cell_bg(row, col) != unfocused:
            return
    raise AssertionError("Tab never focused the Cancel button:\n" + "\n".join(s.display()))


def _click_close_box(s):
    top, _, _, _ = _frame(s)
    col = s.display()[top].find(CLOSE_GLYPH)
    assert col >= 0, "no close glyph in the title row:\n" + "\n".join(s.display())
    _click(s, col, top)
    s._drain(0.4)


# @scenario SC-dialog-engine-15
def test_ok_label_click_applies_and_closes(tmp_path):
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        row, col = _label_pos(s, OK_LABEL)
        _click(s, col + len(OK_LABEL) - 1, row)      # the LAST cell of the label
        s._drain(0.4)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "clicking the last cell of the Ok label did not close the dialog:\n" + "\n".join(rows)
    assert _options_applied(trace), \
        "the Ok click did not apply the options (no trace line):\n" + \
        (trace.read_text() if trace.exists() else "<no trace>")


# @scenario SC-dialog-engine-16
def test_clicks_outside_frame_are_inert(tmp_path):
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        top, bottom, left, right = _frame(s)
        mid_row = (top + bottom) // 2
        mid_col = (left + right) // 2
        spots = {
            "above": (mid_col, top - 1),
            "below": (mid_col, bottom + 1),
            "left of": (left - 2, mid_row),
            "right of": (right + 2, mid_row),
            "left border": (left, mid_row),
            "bottom border": (mid_col, bottom),
        }
        for where, (col, row) in spots.items():
            _click(s, col, row)
            assert _is_open(s), \
                "a click %s the frame at (%d,%d) closed the dialog:\n%s" % (
                    where, col, row, "\n".join(s.display()))
        _dismiss(s)
        s.key(F2_SAVE, delay=0.6)
        saved = s.text()
    assert saved == SEED, "the clicks reached the editor: %r" % saved
    assert not _options_applied(trace), "an outside click applied the options"


# @scenario SC-dialog-engine-17n
@incoherence("from the initial focus (the Enable checkbox) the close box behaves exactly "
             "like Esc there: the first click only moves the focus to Cancel and the "
             "dialog stays open; a second click, or Esc, closes it")
def test_close_box_click_dismisses_like_esc(tmp_path):
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        _click_close_box(s)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "clicking the close glyph did not dismiss the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), "the close box applied the options like Ok"


# @scenario SC-dialog-engine-17n
def test_close_box_click_from_a_field_dismisses_like_esc(tmp_path):
    # With a text field focused (Alt-U, the Endpoint field) Esc cancels the
    # dialog outright, and the close box must do the same: box gone, nothing
    # applied.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(ALT_U, delay=0.4)
        _click_close_box(s)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "clicking the close glyph did not dismiss the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), "the close box applied the options like Ok"


# @scenario SC-dialog-engine-17n
@incoherence("with a radio group focused (Tab from the checkbox lands on Backend) the "
             "close box, like Esc there, APPLIES the options: the radio handler sets "
             "sw = 1 on WPE_ESC but still falls through e_get_opt_sw, which resets sw "
             "to 0, so the loop exits with ret == 0 and the caller treats it as Ok")
def test_close_box_click_from_a_radio_does_not_apply(tmp_path):
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(TAB, delay=0.3)                # Enable -> Backend radio group
        _click_close_box(s)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "clicking the close glyph did not dismiss the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), \
        "the close box applied the options like Ok:\n" + trace.read_text()


# @scenario SC-dialog-engine-20
def test_enter_on_focused_cancel_cancels(tmp_path):
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        _tab_to_cancel(s)
        s.key(ENTER, delay=0.6)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "Enter on the focused Cancel button did not close the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), "Enter on Cancel applied the options like Ok"
