"""The dialog-engine contracts that had no direct test.

Each test here pins one promise of the option-dialog engine (e_opt_kst) that
the older suites only touched on the way to something else: where a dialog
opens, that an in-place repaint neither moves nor toggles anything, that Enter
on a plain widget presses the default button, that one Esc from any widget
cancels and applies nothing, that Tab wraps and Shift-Tab reverses, and that
Alt-<letter> drives a widget in the ncurses build.

Driven on the AI settings dialog (Options > AI, a 64x21 box in the agent-host
build, 64x20 without the Agent row).  The oracle for "applied" is the AI trace
file: Ok writes an "options backend=..." line, Cancel writes nothing.
"""
import os
import subprocess

import pytest

from wpe_driver import WpeSession, ALT, WPE_BIN, checkbox_mark, row_with


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
SHIFT_TAB = "\033[Z"
DOWN = "\033[B"
SPACE = " "
ALT_E = "\033e"              # the Enable checkbox accelerator
ALT_M = "\033m"              # the Model button: floats the scrollable picker
ALT_U = "\033u"              # focus the Endpoint text field
ENABLE_LABEL = "Enable AI"
ENDPOINT_LABEL = "Endpoint"
PICKER_HINT = "PgUp/PgDn"    # the scrollable picker's key hint row
NEW_ENDPOINT = "http://edited.test:1"
# pyte shows the DEC line-drawing set as its ASCII code points: the frame
# corners come out as l (top-left), k (top-right), m (bottom-left), j
# (bottom-right); x is a vertical border cell.
CORNER_TL, CORNER_TR, CORNER_BL, CORNER_BR = "l", "k", "m", "j"
CENTRE_TOLERANCE = 2.0       # cells; integer division and clamping shift by one
# Tab stops of the AI settings dialog in the engine's column-major order
# (left column top to bottom, then the right column, then the button row); a
# radio group is one stop.  The Agent group exists only in the agent-host
# build, so it is counted from the screen.
TAB_STOPS_LEFT = ["Enable", "Backend", "Model", "Provider", "Endpoint",
                  "CA file", "API key", "Advanced"]
TAB_STOPS_RIGHT = ["Permission"]
TAB_STOPS_BUTTONS = ["Ok", "Cancel"]
AGENT_ROW_LABEL = "Built-in"


def _open_ai_settings(tmp_path, trace, env_overrides=None, **session_kw):
    env = dict(ENV, XWPE_AI_TRACE=str(trace))
    env.update(env_overrides or {})
    s = WpeSession(str(tmp_path), SEED, env_extra=env, **session_kw)
    _reopen(s)
    return s


def _reopen(s):
    s.key(ALT.OPTIONS, delay=0.5)
    s.key("i", delay=0.7)


def _is_open(s):
    return any(TITLE in row for row in s.display())


def _options_applied(trace):
    text = trace.read_text() if trace.exists() else ""
    return any(line.startswith("options ") for line in text.splitlines())


def _trace_text(trace):
    return trace.read_text() if trace.exists() else "<no trace>"


def _screen(s):
    return "\n".join(s.display())


def _dialog_frame(s):
    """(top, bottom, left, right) of the dialog frame, read from the screen:
    the corners around the title and the bottom-left corner below."""
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


def _editor_window_frame(s):
    """(top, bottom, left, right) of the active editor window before any
    dialog is open: its top border carries the file name and ends in the
    top-right corner; its bottom border starts with the bottom-left corner in
    the same column as the top-left one."""
    rows = s.display()
    top = next((i for i, r in enumerate(rows) if " t.c " in r), None)
    assert top is not None, "no editor window title row:\n" + "\n".join(rows)
    left = rows[top].index(CORNER_TL)
    right = rows[top].rindex(CORNER_TR)
    bottom = next((i for i in range(top + 1, len(rows)) if rows[i][left] == CORNER_BL), -1)
    assert bottom > top, "editor window has no bottom border:\n" + "\n".join(rows)
    return top, bottom, left, right


def _centre(frame):
    top, bottom, left, right = frame
    return (top + bottom) / 2.0, (left + right) / 2.0


def _label_cell(s, label):
    """(row, col) of the first cell of `label`; its background tells whether
    the widget is drawn focused (the engine highlights the focused label)."""
    for i, row in enumerate(s.display()):
        c = row.find(label)
        if c >= 0:
            return (i, c)
    raise AssertionError("%r not on screen:\n%s" % (label, _screen(s)))


def _enable_mark_or_none(s):
    """The Enable mark, or None when the dialog is no longer on screen (a
    stray key may have pressed a button), so the caller's assertion can say
    which it was instead of failing inside the helper."""
    if not _is_open(s):
        return None
    return checkbox_mark(s, ENABLE_LABEL)


def _tab_stop_count(s):
    stops = len(TAB_STOPS_LEFT) + len(TAB_STOPS_RIGHT) + len(TAB_STOPS_BUTTONS)
    if AGENT_ROW_LABEL in row_with(s, AGENT_ROW_LABEL):
        stops += 1
    return stops


# @scenario SC-dialog-engine-01
def test_dialog_opens_centred_on_editor_window(tmp_path):
    trace = tmp_path / "ai.trace"
    env = dict(ENV, XWPE_AI_TRACE=str(trace))
    with WpeSession(str(tmp_path), SEED, env_extra=env) as s:
        window = _editor_window_frame(s)
        _reopen(s)
        box = _dialog_frame(s)
        rows = s.display()
    want_row, want_col = _centre(window)
    got_row, got_col = _centre(box)
    assert box[0] >= 1, "the dialog frame starts above row 1: %r" % (box,)
    assert abs(got_row - want_row) <= CENTRE_TOLERANCE \
        and abs(got_col - want_col) <= CENTRE_TOLERANCE, \
        "dialog %r (centre %.1f,%.1f) is not centred on the editor window %r " \
        "(centre %.1f,%.1f):\n%s" % (box, got_row, got_col, window, want_row,
                                     want_col, "\n".join(rows))


# @scenario SC-dialog-engine-12
def test_picker_refocus_keeps_checkbox_and_one_esc_closes(tmp_path):
    # Tick Enable, float the model picker (its return repaints the dialog in
    # place and refocuses the checkbox), close the picker: the tick must
    # survive the refocus, and ONE Esc must then close the dialog without
    # applying anything.  The box shows the effective state, so the assistant
    # is left OFF here (no XWPE_AI_ENABLE) for the box to open as [ ].
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace, env_overrides={"XWPE_AI_ENABLE": ""}) as s:
        s.key(SPACE, delay=0.4)
        ticked = checkbox_mark(s, ENABLE_LABEL)
        s.key(ALT_M, delay=0.8)
        picker_open = any(PICKER_HINT in r for r in s.display())
        s.key(ESC, delay=0.6)                 # closes the picker only
        after_picker = checkbox_mark(s, ENABLE_LABEL)
        still_open_after_picker = _is_open(s)
        s.key(ESC, delay=0.7)                 # one Esc: the dialog
        still_open = _is_open(s)
        rows = s.display()
    assert ticked == "[X]", "Space did not tick Enable: %r" % ticked
    assert picker_open, "Alt-M did not float the model picker"
    assert still_open_after_picker, "closing the picker closed the dialog too"
    assert after_picker == "[X]", \
        "the in-place repaint after the picker toggled Enable: %r -> %r" % (ticked, after_picker)
    assert not still_open, \
        "one Esc after the picker repaint did not close the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), \
        "Esc applied the options:\n" + _trace_text(trace)


# @scenario SC-dialog-engine-19
def test_enter_on_checkbox_fires_default_button(tmp_path):
    # The dialog opens with the Enable checkbox focused; Enter there is not a
    # toggle but the default button (Ok): the dialog closes and the options
    # are applied.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(ENTER, delay=0.8)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "Enter on the focused checkbox did not close the dialog:\n" + "\n".join(rows)
    assert _options_applied(trace), \
        "Enter on the checkbox did not fire the default button (no options line):\n" \
        + _trace_text(trace)


# @scenario SC-dialog-engine-21
def test_esc_after_editing_field_applies_nothing(tmp_path):
    # Edit the Endpoint field, leave it for a radio with Shift-Tab (the edit is
    # kept in the field), then Esc: the dialog closes, nothing is applied, and
    # a reopened dialog shows the endpoint it had before.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        before = row_with(s, ENDPOINT_LABEL)
        s.key(ALT_U, delay=0.4)
        s.key(NEW_ENDPOINT, delay=0.4)       # the first key replaces the value
        edited = row_with(s, ENDPOINT_LABEL)
        s.key(SHIFT_TAB, delay=0.3)          # Endpoint -> Provider
        s.key(SHIFT_TAB, delay=0.3)          # Provider -> Model
        s.key(SHIFT_TAB, delay=0.3)          # Model -> Backend radio group
        s.key(ESC, delay=0.7)
        still_open = _is_open(s)
        _reopen(s)
        reopened = row_with(s, ENDPOINT_LABEL)
        s.key(ESC, delay=0.5)
    assert NEW_ENDPOINT in edited and edited != before, \
        "the Endpoint field was not edited: %r" % edited
    assert not still_open, "Esc after the edit did not close the dialog"
    assert not _options_applied(trace), \
        "Esc after editing a field applied the options:\n" + _trace_text(trace)
    assert reopened == before, \
        "the edit survived Esc: before %r, reopened %r" % (before, reopened)


# @scenario SC-dialog-engine-23
def test_tab_wraps_and_shift_tab_reverses(tmp_path):
    # Tab visits every stop once and the Nth Tab is back on the first widget
    # (Enable): proved by Space toggling Enable afterwards.  Shift-Tab from the
    # first widget lands on the last (Cancel): proved by Enter there cancelling
    # the dialog with nothing applied.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        stops = _tab_stop_count(s)
        row, col = _label_cell(s, ENABLE_LABEL)
        focused = s.cell_bg(row, col)
        before = checkbox_mark(s, ENABLE_LABEL)
        backgrounds = []
        for _ in range(stops):
            s.key(TAB, delay=0.25)
            backgrounds.append(s.cell_bg(row, col))
        s.key(SPACE, delay=0.4)
        after_wrap = _enable_mark_or_none(s)
        s.key(SHIFT_TAB, delay=0.3)          # first widget -> last widget
        s.key(ENTER, delay=0.7)
        still_open = _is_open(s)
        rows = s.display()
    assert all(bg != focused for bg in backgrounds[:-1]), \
        "Tab came back to Enable before visiting all %d stops: %r" % (stops, backgrounds)
    assert backgrounds[-1] == focused, \
        "Tab %d did not wrap back to Enable (focused bg %r): %r" % (stops, focused, backgrounds)
    assert after_wrap is not None and after_wrap != before, \
        "Space after the wrap did not toggle Enable (%r -> %r, None = dialog gone): " \
        "the focus is not on it" % (before, after_wrap)
    assert not still_open, \
        "Enter after Shift-Tab from the first widget did not close the dialog:\n" \
        + "\n".join(rows)
    assert not _options_applied(trace), \
        "Shift-Tab from the first widget did not land on Cancel: Enter applied the options:\n" \
        + _trace_text(trace)


# @scenario SC-dialog-engine-17e
def test_one_esc_from_checkbox_cancels(tmp_path):
    # From the initial focus (the Enable checkbox) one Esc closes the dialog
    # and applies nothing.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(ESC, delay=0.7)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "one Esc from the checkbox did not close the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), \
        "Esc from the checkbox applied the options:\n" + _trace_text(trace)


# @scenario SC-dialog-engine-17er
def test_one_esc_from_radio_does_not_apply(tmp_path):
    # Tab to the Backend radio group, move to a non-default option, then one
    # Esc: the dialog closes and nothing is applied, even though a radio
    # option was focused.
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(TAB, delay=0.3)                # Enable -> Backend radio group
        s.key(DOWN, delay=0.3)               # first option -> second option
        s.key(ESC, delay=0.7)
        still_open = _is_open(s)
        rows = s.display()
    assert not still_open, \
        "one Esc from a radio option did not close the dialog:\n" + "\n".join(rows)
    assert not _options_applied(trace), \
        "Esc from a radio applied the options:\n" + _trace_text(trace)


# @scenario SC-dialog-engine-26n
def test_alt_accelerator_activates_widget_ncurses(tmp_path):
    # Alt-M presses the Model button (the picker floats, its hint row shows);
    # Alt-E toggles the Enable checkbox (its mark flips).
    trace = tmp_path / "ai.trace"
    with _open_ai_settings(tmp_path, trace) as s:
        s.key(ALT_M, delay=0.8)
        picker = _screen(s)
        s.key(ESC, delay=0.6)                 # closes the picker only
        before = checkbox_mark(s, ENABLE_LABEL)
        s.key(ALT_E, delay=0.5)
        after = checkbox_mark(s, ENABLE_LABEL)
        s.key(ESC, delay=0.5)
    assert PICKER_HINT in picker, "Alt-M did not open the model picker:\n" + picker
    assert before != after, "Alt-E did not toggle Enable: %r -> %r" % (before, after)
