"""The Alt-G dropdown: row letters, closing, movement, clicks, geometry, titles.

Each test pins one scenario of the ai-menu-and-popups capability and asserts
what the user sees: the screen, the XWPE_AI_TRACE log or the saved file.  The
mock backend answers every turn, so nothing here reaches a network.  A .txt
file is used where the contextual language-server bar would only get in the
way; a .c file where an action needs source to work on.
"""
import os
import pytest
from wpe_driver import (WpeSession, ALT, WAIT_SCALE, assistant_build,
                        wait_screen, pane_text_with_history)

pytestmark = pytest.mark.skipif(not assistant_build(),
                                reason="wpe built without --enable-ai")

ENV = {"XWPE_AI_ENABLE": "1", "XWPE_AI_BACKEND": "mock", "XWPE_AI_MOCK_REPLY": "ok"}
C_SRC = "int main(void){return 0;}\n"
DOWN = "\033[B"
HOME = "\033[H"
END = "\033[F"
ALT_0 = "\0330"
ALT_U = "\033u"
F3_FILES = "\033OR"
MENU_OPEN_LINE = "menu open n=10\n"
DROPDOWN_MARK = "Ask (chat)"
MENU_ROW_LABELS = ("Ask (chat)", "Edit current file", "Complete at cursor",
                   "Multi-file edit", "Agent (tools)", "Build & fix", "----",
                   "Permissions:", "Clear conversation", "AI settings")
# A frame cell is drawn as an ACS letter: "x" is the vertical line.
FRAME_VERTICAL = "x"


def _env(tmp_path, **extra):
    env = dict(ENV, XWPE_AI_TRACE=str(tmp_path / "t.trace"))
    env.update(extra)
    return env


def _trace(tmp_path):
    path = tmp_path / "t.trace"
    if path.exists():
        return path.read_text()
    return ""


def _screen(disp):
    return "\n".join(disp)


def _sgr(button, col, row, press=True):
    """SGR 1006 mouse report, 1-based column and row."""
    if press:
        final = "M"
    else:
        final = "m"
    return "\x1b[<%d;%d;%d%s" % (button, col, row, final)


def _find(disp, needle):
    """(1-based column, 1-based row) of ``needle`` on the screen, or None."""
    for r, line in enumerate(disp):
        i = line.find(needle)
        if i >= 0:
            return i + 1, r + 1
    return None


def _cell(s, y, x):
    """Colour attributes of one cell: what a highlight changes."""
    c = s.screen.buffer[y][x]
    return (c.fg, c.bg, c.reverse, c.bold)


def _open_dropdown(s, delay=0.6):
    s.key(ALT.AI, delay=delay)
    assert DROPDOWN_MARK in _screen(s.display()), \
        "Alt-G did not open the dropdown:\n" + _screen(s.display())


def _box_geometry(disp):
    """(top row, bottom row, left column, right column) of the dropdown frame,
    0-based, read from its vertical frame cells; None when no box is up."""
    top = next((i for i, r in enumerate(disp) if DROPDOWN_MARK in r), None)
    if top is None:
        return None
    ask = disp[top]
    left = ask.rfind(FRAME_VERTICAL, 0, ask.index(DROPDOWN_MARK))
    right = ask.index(FRAME_VERTICAL, ask.index("Alt-G A"))
    rows = [i for i, r in enumerate(disp)
            if len(r) > left and r[left] == FRAME_VERTICAL and r[right:right + 1] == FRAME_VERTICAL]
    return top - 1, rows[-1] + 1, left, right


# @scenario SC-ai-menu-13
def test_letter_y_cycles_the_permission_level_and_flashes_it(tmp_path):
    """Alt-G y runs the Permissions row: the bar flashes the level and the
    trace records the new policy."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key("y", delay=0.7)
        disp = s.display()
        trace = _trace(tmp_path)
    assert "Permissions:" in disp[-1], "y did not flash the permission bar: %r" % disp[-1]
    assert "policy set edits" in trace, "y did not move the level ask -> edits (trace):\n" + trace
    assert DROPDOWN_MARK not in _screen(disp), "the dropdown stayed open after y"


# @scenario SC-ai-menu-13
def test_upper_case_letter_f_opens_the_multi_file_prompt(tmp_path):
    """A row's letter works in upper case: F opens the Multi-file popup."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key("F", delay=0.9)
        disp = s.display()
        s.key("\033", delay=0.5)
    assert "Multi-file edit: task" in _screen(disp), \
        "F did not open the Multi-file popup:\n" + _screen(disp)
    assert DROPDOWN_MARK not in _screen(disp), "the dropdown stayed open after F"


# @scenario SC-ai-menu-13
# @scenario SC-ai-menu-21
def test_letter_b_runs_build_and_fix_with_no_prompt_and_a_pane_line(tmp_path):
    """Alt-G b starts the agent on its fixed goal: no popup opens, the trace
    names the build command and the pane says which command it will run."""
    env = _env(tmp_path, XWPE_AI_MOCK_REPLY="done")
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=env) as s:
        _open_dropdown(s)
        s.key("b", delay=2.0)
        s._drain(1.0)
        lines = pane_text_with_history(s, ["[fix-build] build command"])
        disp = s.display()
        trace = _trace(tmp_path)
        s.key("\033", delay=0.4)
    assert "fix-build cmd=" in trace, "b did not start Build & fix (trace):\n" + trace
    assert any("[fix-build] build command" in line for line in lines), \
        "no [fix-build] build command line in the pane:\n" + "\n".join(lines)
    shown = _screen(disp)
    for popup in ("AI agent task", "AI edit instruction", "Multi-file edit: task"):
        assert popup not in shown, "Build & fix asked for a prompt (%s):\n%s" % (popup, shown)


# @scenario SC-ai-menu-13
# @scenario SC-ai-menu-21
def test_letter_c_runs_complete_at_once_with_no_prompt(tmp_path):
    """Alt-G c completes at the cursor straight away: no prompt popup opens
    and the trace shows the completion request."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c",
                    env_extra=_env(tmp_path, XWPE_AI_MOCK_REPLY="done")) as s:
        _open_dropdown(s)
        s.key("c", delay=1.5)
        s._drain(1.0)
        disp = s.display()
        trace = _trace(tmp_path)
        s.key("\033", delay=0.5)
    assert "complete stream" in trace, "c did not run Complete (trace):\n" + trace
    shown = _screen(disp)
    for popup in ("AI agent task", "AI edit instruction", "Multi-file edit: task"):
        assert popup not in shown, "Complete asked for a prompt (%s):\n%s" % (popup, shown)


# @scenario SC-ai-menu-16
def test_esc_and_alt_zero_close_the_dropdown_and_run_nothing(tmp_path):
    """Esc and Alt-0 both close the dropdown: nothing but the open line is in
    the trace and the saved file is the one that was opened."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key("\033", delay=0.5)
        after_esc = s.display()
        _open_dropdown(s)
        s.key(ALT_0, delay=0.5)
        after_alt0 = s.display()
        s.save()
        trace = _trace(tmp_path)
        text = s.text()
    assert DROPDOWN_MARK not in _screen(after_esc), "Esc did not close the dropdown"
    assert DROPDOWN_MARK not in _screen(after_alt0), "Alt-0 did not close the dropdown"
    assert trace.replace(MENU_OPEN_LINE, "") == "", "an action ran on Esc or Alt-0: %r" % trace
    assert text == "hello\n", "the buffer changed: %r" % text


# @scenario SC-ai-menu-17
def test_highlight_starts_on_ask_and_down_wraps_from_the_last_row(tmp_path):
    """The highlight is on Ask when the box opens; End reaches AI settings and
    one more Down wraps back to Ask."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s, delay=0.8)
        disp = s.display()
        ask = _find(disp, "Ask (chat)")
        last = _find(disp, "AI settings")
        ax, ay = ask[0] - 1, ask[1] - 1
        lx, ly = last[0] - 1, last[1] - 1
        lit = _cell(s, ay, ax)
        unlit = _cell(s, ly, lx)
        s.key(END, delay=0.3)
        end_ask, end_last = _cell(s, ay, ax), _cell(s, ly, lx)
        s.key(DOWN, delay=0.3)
        wrap_ask, wrap_last = _cell(s, ay, ax), _cell(s, ly, lx)
    assert lit != unlit, "the highlight is not visible: Ask %r, settings %r" % (lit, unlit)
    assert end_last == lit and end_ask != lit, "End did not move the highlight to AI settings"
    assert wrap_ask == lit and wrap_last != lit, "Down from the last row did not wrap to Ask"


# @scenario SC-ai-menu-17
def test_end_then_enter_opens_settings_and_home_then_enter_arms_ask(tmp_path):
    """End then Enter runs AI settings (the Options > AI dialog opens); Home
    then Enter runs Ask (the pane's input row is armed)."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key(END, delay=0.3)
        s.key("\r", delay=1.0)
        after_end = _screen(s.display())
        s.key("\033", delay=0.6)
        _open_dropdown(s)
        s.key(END, delay=0.3)
        s.key(HOME, delay=0.3)
        s.key("\r", delay=1.0)
        after_home = _screen(s.display())
    assert "AI settings" in after_end and "Backend" in after_end, \
        "End, Enter did not open Options > AI:\n" + after_end
    assert "Enter=send" in after_home, "Home, Enter did not arm the Ask input row:\n" + after_home


# @scenario SC-ai-menu-18
def test_an_unbound_key_keeps_the_dropdown_open(tmp_path):
    """A key that is neither a row letter nor a shortcut leaves the dropdown
    up (redrawn), and Esc then closes it."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key("x", delay=0.6)
        after_x = s.display()
        s.key("\033", delay=0.5)
        after_esc = s.display()
    assert DROPDOWN_MARK in _screen(after_x), \
        "the dropdown closed on an unbound letter:\n" + _screen(after_x)
    assert DROPDOWN_MARK not in _screen(after_esc), "Esc did not close the redrawn dropdown"


# @scenario SC-ai-menu-18
def test_a_global_shortcut_runs_with_the_dropdown_open(tmp_path):
    """Alt-U (Run) works with the dropdown open: the dropdown closes, the
    program runs and the file is unchanged."""
    with WpeSession(str(tmp_path), C_SRC, filename="a.c", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key(ALT_U, delay=1.2)
        ran = wait_screen(s, "Return-Code", tmo=12 * WAIT_SCALE)
        disp = s.display()
        s.save()
        text = s.text()
    assert DROPDOWN_MARK not in _screen(disp), "Alt-U left the dropdown open:\n" + _screen(disp)
    assert ran, "Alt-U did not run the program (no Return-Code in Messages):\n" + _screen(disp)
    assert text == C_SRC, "the buffer changed: %r" % text


def _click(s, col, row):
    s.key(_sgr(0, col, row, True), _sgr(0, col, row, False), delay=0.8)


# @scenario SC-ai-menu-19
def test_a_click_on_a_row_runs_it(tmp_path):
    """Clicking the Permissions row closes the dropdown and runs that row."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        col, row = _find(s.display(), "Permissions:")
        _click(s, col + 3, row)
        disp = s.display()
        trace = _trace(tmp_path)
    assert DROPDOWN_MARK not in _screen(disp), "the click did not close the dropdown"
    assert "Permissions:" in disp[-1], "the clicked row did not run: %r" % disp[-1]
    assert "policy set edits" in trace, "the clicked row did not change the level (trace):\n" + trace


# @scenario SC-ai-menu-19
def test_a_click_outside_closes_the_dropdown_and_runs_nothing(tmp_path):
    """A click away from the box closes it: no row runs and the file is as
    it was."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        _click(s, 5, 5)
        disp = s.display()
        s.save()
        trace = _trace(tmp_path)
        text = s.text()
    assert DROPDOWN_MARK not in _screen(disp), "a click outside did not close the dropdown"
    assert trace.replace(MENU_OPEN_LINE, "") == "", "a click outside ran an action: %r" % trace
    assert text == "hello\n", "the buffer changed: %r" % text


# @scenario SC-ai-menu-20
@pytest.mark.parametrize("cols,rows,left,right,top,bottom", [
    (80, 24, 45, 78, 11, 22),
    (40, 24, 5, 38, 11, 22),
    (40, 15, 5, 38, 2, 13),
])
def test_the_box_opens_upward_from_the_bar_and_stays_on_screen(
        tmp_path, cols, rows, left, right, top, bottom):
    """At 80x24, 40x24 and 40x15 the frame sits at the known columns and rows,
    its bottom edge on the row above the bar, and all ten rows are visible."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", cols=cols, rows=rows,
                    env_extra=_env(tmp_path)) as s:
        s.key(ALT.AI, delay=0.8)
        disp = s.display()
        geometry = _box_geometry(disp)
        s.key("\033", delay=0.3)
    assert geometry, "no dropdown at %dx%d:\n%s" % (cols, rows, _screen(disp))
    shown = _screen(disp)
    missing = [label for label in MENU_ROW_LABELS if label not in shown]
    assert not missing, "rows not visible at %dx%d: %r" % (cols, rows, missing)
    assert geometry == (top, bottom, left, right), \
        "frame (top, bottom, left, right) at %dx%d is %r, expected %r" % (
            cols, rows, geometry, (top, bottom, left, right))
    assert geometry[3] <= cols - 1 and geometry[1] == rows - 2


# @scenario SC-ai-menu-21
def test_edit_popup_titles_plain_and_over_a_marked_block(tmp_path):
    """Edit opens "AI edit instruction"; with a block marked the title says
    "(selection)"."""
    with WpeSession(str(tmp_path), C_SRC + "int x;\n", filename="a.c",
                    env_extra=_env(tmp_path)) as s:
        _open_dropdown(s)
        s.key("e", delay=0.9)
        plain = _screen(s.display())
        s.key("\033", delay=0.5)
        s.key(ALT.BLOCK, delay=0.5)
        s.key("o", delay=0.5)                       # Block > Mark whole file
        _open_dropdown(s)
        s.key("e", delay=0.9)
        marked = _screen(s.display())
        s.key("\033", delay=0.5)
    assert "AI edit instruction" in plain, "no Edit popup title:\n" + plain
    assert "(selection)" not in plain, "the plain Edit title says selection:\n" + plain
    assert "AI edit instruction (selection)" in marked, \
        "no (selection) title over a marked block:\n" + marked


# @scenario SC-ai-menu-07
def test_pane_focused_alt_g_opens_the_dropdown_and_edit_is_refused(tmp_path):
    """With the AI pane focused, Alt-G still opens the dropdown; Edit then
    gives the one pane line instead of a popup."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        _open_dropdown(s, delay=0.5)
        s.key("a", delay=0.8)                       # Ask: the pane is docked and focused
        s.key(ALT.AI, delay=0.7)
        opened = _screen(s.display())
        s.key("e", delay=0.9)
        refused = s.display()
        s.key("\033", delay=0.4)
    assert DROPDOWN_MARK in opened, "the dropdown did not open with the pane focused:\n" + opened
    shown = _screen(refused)
    assert "AI edit instruction" not in shown, "Edit opened its popup from the pane:\n" + shown
    assert "[AI] no file here - Edit, Complete and Multi-file work on a file window" in shown, \
        "no pane refusal line:\n" + shown


# @scenario SC-ai-menu-08
def test_alt_g_in_the_options_ai_dialog_is_ignored(tmp_path):
    """While Options > AI owns the keyboard, Alt-G opens no dropdown and the
    dialog stays as it was."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        s.key(ALT.OPTIONS, delay=0.7)
        s.key("i", delay=1.0)
        before = _screen(s.display())
        s.key(ALT.AI, delay=0.9)
        after = _screen(s.display())
        s.key("\033", delay=0.6)
        closed = _screen(s.display())
        trace = _trace(tmp_path)
    assert "AI settings" in before, "Options > AI did not open:\n" + before
    assert DROPDOWN_MARK not in after, "Alt-G opened the dropdown over the dialog:\n" + after
    assert "AI settings" in after, "Alt-G closed the dialog:\n" + after
    assert "menu open" not in trace, "the dropdown was opened (trace):\n" + trace
    assert "AI settings" not in closed, "Esc did not close the dialog"


# @scenario SC-ai-menu-08
def test_alt_g_in_the_file_manager_opens_the_dropdown_and_esc_leaves_it_intact(tmp_path):
    """With the File Manager open, Alt-G opens the dropdown over it; Esc
    closes only the dropdown and the File Manager and the file are intact."""
    with WpeSession(str(tmp_path), "hello\n", filename="t.txt", env_extra=_env(tmp_path)) as s:
        s.key(F3_FILES, delay=1.0)
        manager = _screen(s.display())
        s.key(ALT.AI, delay=0.9)
        opened = _screen(s.display())
        s.key("\033", delay=0.6)
        after = _screen(s.display())
        s.key("\033", delay=0.4)
        s.save()
        text = s.text()
    assert "File-Manager" in manager, "the File Manager did not open:\n" + manager
    assert DROPDOWN_MARK in opened, "Alt-G did not open the dropdown over the File Manager:\n" + opened
    assert DROPDOWN_MARK not in after, "Esc did not close the dropdown"
    assert "File-Manager" in after and "Files:" in after, \
        "Esc closed the File Manager too:\n" + after
    assert text == "hello\n", "the buffer changed: %r" % text
